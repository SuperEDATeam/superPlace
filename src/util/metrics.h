// 可观测性子系统（05 §4.5，决策 4）。
// 全局布局是会震荡、会发散、会安静收敛到垃圾的迭代过程——没有逐迭代指标就是在盲调。
#pragma once

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

/// 逐迭代指标写 CSV，阶段耗时与总览写 summary.json。
class MetricsSink {
public:
    void setOutputDir(const std::string& dir);

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

private:
    std::string out_dir_;
    std::string cur_stage_;
    std::vector<std::pair<std::string, std::vector<IterMetrics>>> stages_;
    std::map<std::string, double> durations_;
    std::map<std::string, double> summary_num_;
    std::map<std::string, std::string> summary_str_;

    void flushStageCsv() const;
};

}  // namespace sp
