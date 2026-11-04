// T14 连续回放控制器：纯逻辑、无 GL/ImGui 依赖，可无头单测。
//
// 时间轴用全局迭代数 t（§3.2），不用帧序号。播放速度单位"轮/秒"。
// 本类把"播放时长 vt（秒）"分段映射到"全局 t 与关键帧对"：
//   * 相邻帧 t 相等或下一帧 breakBefore → 硬切，不插值；
//   * iter == -1 的里程碑帧播放时停留 dwell（默认 0.5 s）；
//   * 其余按 t 线性插值 lo->hi，alpha∈[0,1]。
// 懒计算（热力图/点选索引/属性着色）在播放或拖动时取最近关键帧 statFrame，
// 停下来再由 App 按插值位置重算。
#pragma once

#include <string>
#include <vector>

#include "viz/scene.h"

namespace viz {

class PlaybackController {
public:
    struct DisplayState {
        int    lo = -1, hi = -1;   // 关键帧对；lo==hi 表示单帧/硬切
        double alpha = 0.0;         // lo->hi 插值系数
        bool   interpolating = false;
        int    statFrame = -1;      // 懒统计取最近关键帧
        std::string label;
        std::string stage;
        double globalT = 0.0;       // 映射到的全局 t
        double vt = 0.0;            // 当前播放秒
    };

    void setFrames(std::vector<FrameMeta> metas);

    void setSpeed(double tPerSec) {
        // 调速时保持当前 globalT/插值位置/hold 已走时长，不跳画面。
        const double g = state().globalT;
        const double holdElapsed = currentHoldElapsed();
        speed_ = tPerSec;
        recompute();
        vt_ = vtForGlobalT(g) + holdElapsed;
    }
    void setMilestoneDwell(double s) {
        const double g = state().globalT;
        const double holdElapsed = currentHoldElapsed();
        dwell_ = s;
        recompute();
        vt_ = vtForGlobalT(g) + holdElapsed;
    }
    void setLoop(bool loop) { loop_ = loop; }

    void play() {
        // 非循环：已到尾端再按播放 = 从头重播，而不是下一帧又立刻停在尾。
        if (!loop_ && total_ > 1e-9 && vt_ >= total_ - 1e-9) vt_ = 0.0;
        pinned_ = -1;          // 开始播放即解除显式选定关键帧
        playing_ = true;
    }
    void pause() { playing_ = false; }
    void toggle() { playing_ ? pause() : play(); }
    bool playing() const { return playing_; }
    bool loop() const { return loop_; }
    double speed() const { return speed_; }

    /// 显式选定某关键帧（同 arrival 的多帧必须能逐个精确到达）。
    /// 导航按钮/曲线 goto 用它；下次 advance 自动解除。
    void pinIndex(int idx);

    /// 推进真实 dt 秒（仅播放中生效；循环到尾自动回绕，否则停在末帧）。
    void advance(double dtRealSec);
    /// 直接定位到播放秒 vt（拖动进度条）。
    void scrubTo(double vtSec);

    /// 时间轴按 global t（非 vt 秒）归一化：[0,1] -> (t0,tlast)。
    double globalTRange() const;
    double playheadGlobal01() const;
    void   scrubToGlobal01(double g01);

    void nextKeyframe();
    void prevKeyframe();
    void nextStage();
    void prevStage();
    void toStart();
    void toEnd();

    double playhead() const { return vt_; }
    double totalDuration() const { return total_; }
    int    frameCount() const { return static_cast<int>(meta_.size()); }
    DisplayState state() const;

private:
    void recompute();
    /// 由 global t 反查播放秒 vt（调速/scrub 保持同一画面）。
    double vtForGlobalT(double g) const;
    /// 当前若正停在某里程碑 hold 内，返回已走时长；否则 0（调速保留不重走整段）。
    double currentHoldElapsed() const;
    bool milestone(int k) const {
        return k >= 0 && meta_[static_cast<size_t>(k)].iter == -1;
    }
    /// k -> k+1 是否硬切（breakBefore 或 t 相等/回退）。
    bool hardCut(int k) const;

    std::vector<FrameMeta> meta_;
    std::vector<double>    arrival_;  // arrival_[k]：开始显示第 k 帧的播放秒
    double total_ = 0.0;
    double vt_ = 0.0;
    double speed_ = 60.0;             // 轮（t 单位）/秒
    double dwell_ = 0.5;              // 里程碑停留秒
    bool   playing_ = false;
    bool   loop_ = false;
    int    pinned_ = -1;              // 显式选定关键帧（同 arrival 帧导航），-1 解除
};

}  // namespace viz
