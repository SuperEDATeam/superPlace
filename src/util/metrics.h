// 可观测性子系统（05 §4.5，决策 4）。
// 全局布局是会震荡、会发散、会安静收敛到垃圾的迭代过程——没有逐迭代指标就是在盲调。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sp {

struct IterMetrics {
    int    iter             = 0;
    double hpwl             = 0.0;
    float  overflow         = 0.f;   // τ
    float  lambda           = 0.f;   // 密度惩罚系数
    float  gamma            = 0.f;   // 线长平滑参数
    float  step_size        = 0.f;   // α
    float  grad_norm_wl     = 0.f;
    float  grad_norm_den    = 0.f;
    float  max_displacement = 0.f;
    double elapsed_ms       = 0.0;
};

/// "此刻的布局值得录一帧"的通知（前端回放用，见 io/viz_writer.h）。
struct FrameMark {
    std::string stage;              ///< input / init / mGP / FILLERONLY / mLG / cGP
    int         iter = -1;          ///< 阶段内迭代号；-1 表示阶段里程碑而非某一轮
    double      t = 0.0;            ///< 全局进度：累计 GP 迭代数，前端按它均匀播放
    bool        breakBefore = false;///< 与上一帧之间是跳变（如 mLG），前端不得插值
    std::string label;
};

/// 逐迭代指标写 CSV，阶段耗时与总览写 summary.json。
class MetricsSink {
public:
    void setOutputDir(const std::string& dir);

    /// 阶段可嵌套：全局布局阶段内部还会分出 mGP / FILLERONLY / cGP 三个子阶段，
    /// 各自有独立的迭代曲线与耗时。因此当前阶段名是一个【栈】而不是单个字符串。
    void beginStage(const std::string& stage);
    void push(const IterMetrics& m);
    void endStage(double elapsed_ms);

    /// ScopedTimer 用：记录任意标签的耗时
    void recordDuration(const std::string& label, double ms);

    /// 记录一个标量总览项（最终 HPWL、节点数等）
    void setSummary(const std::string& key, double value);
    void setSummary(const std::string& key, const std::string& value);

    void writeSummary(const std::string& path) const;

    const std::string& outputDir() const { return out_dir_; }

    /// 录帧钩子。各阶段只负责说"这一刻值得录"，真正去读 PlaceDB、写文件的是
    /// 调用方注入的钩子——util 层因此不必认识 PlaceDB。未设置钩子时 markFrame 是空操作。
    using FrameHook = std::function<void(const FrameMark&)>;
    void setFrameHook(FrameHook hook) { frame_hook_ = std::move(hook); }
    void markFrame(const FrameMark& m) const {
        if (frame_hook_) frame_hook_(m);
    }

private:
    FrameHook frame_hook_;
    std::string out_dir_;
    std::vector<std::string> stage_stack_;
    std::vector<std::pair<std::string, std::vector<IterMetrics>>> stages_;
    std::map<std::string, double> durations_;
    std::map<std::string, double> summary_num_;
    std::map<std::string, std::string> summary_str_;

    void flushStageCsv() const;
};

}  // namespace sp
