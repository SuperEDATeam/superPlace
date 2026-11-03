#include "viz/playback.h"

#include <algorithm>
#include <cmath>

namespace viz {

void PlaybackController::setFrames(std::vector<FrameMeta> metas) {
    meta_ = std::move(metas);
    vt_ = 0.0;
    pinned_ = -1;
    recompute();
}

bool PlaybackController::hardCut(int k) const {
    const size_t n = meta_.size();
    if (k + 1 >= static_cast<int>(n)) return true;
    const FrameMeta& a = meta_[static_cast<size_t>(k)];
    const FrameMeta& b = meta_[static_cast<size_t>(k + 1)];
    if (b.breakBefore) return true;
    if (b.t <= a.t) return true;   // t 相等 = 瞬时切换
    return false;
}

void PlaybackController::recompute() {
    const int n = static_cast<int>(meta_.size());
    arrival_.assign(static_cast<size_t>(n), 0.0);
    if (n == 0) { total_ = 0.0; return; }
    double t = 0.0;
    for (int k = 0; k < n; ++k) {
        arrival_[static_cast<size_t>(k)] = t;
        const double hold = milestone(k) ? dwell_ : 0.0;
        double gap = 0.0;
        if (k + 1 < n) {
            if (!hardCut(k)) {
                gap = (meta_[static_cast<size_t>(k + 1)].t -
                       meta_[static_cast<size_t>(k)].t) / speed_;
            }
        }
        t += hold + gap;
    }
    // 末帧的 dwell 已在 k=n-1 那次循环里计入 t，不再额外加尾停留（否则重复）。
    total_ = t;
    if (total_ <= 0.0) total_ = 1e-6;
    vt_ = std::clamp(vt_, 0.0, total_);
}

void PlaybackController::advance(double dtRealSec) {
    if (!playing_ || meta_.empty()) return;
    pinned_ = -1;   // 真正推进即解除显式选定关键帧
    vt_ += dtRealSec;
    if (vt_ >= total_) {
        if (loop_) {
            const double span = total_ > 1e-9 ? total_ : 1.0;
            vt_ = std::fmod(vt_, span);
            if (vt_ < 0.0) vt_ += span;
        } else {
            vt_ = total_;
            playing_ = false;
        }
    }
}

void PlaybackController::scrubTo(double vtSec) {
    if (meta_.empty()) return;
    pinned_ = -1;
    vt_ = std::clamp(vtSec, 0.0, total_);
}

PlaybackController::DisplayState PlaybackController::state() const {
    DisplayState ds;
    const int n = static_cast<int>(meta_.size());
    if (n == 0) return ds;
    // 显式选定关键帧（同 arrival 多帧必须逐个可达）：pinned_ 有效时直接返回该单帧。
    if (pinned_ >= 0 && pinned_ < n) {
        const FrameMeta& pm = meta_[static_cast<size_t>(pinned_)];
        ds.lo = ds.hi = ds.statFrame = pinned_;
        ds.alpha = 0.0;
        ds.interpolating = false;
        ds.globalT = pm.t;
        ds.label = pm.label;
        ds.stage = pm.stage;
        ds.vt = vt_;
        return ds;
    }
    const double vt = std::clamp(vt_, 0.0, total_);

    // 找 vt 落在哪个帧段 [arrival_[k], arrival_[k+1])（末段上界为 total_）。
    int k = n - 1;
    for (int i = 0; i < n; ++i) {
        const double segEnd =
            (i + 1 < n) ? arrival_[static_cast<size_t>(i + 1)] : total_;
        if (vt >= arrival_[static_cast<size_t>(i)] && vt < segEnd) {
            k = i;
            break;
        }
    }

    const FrameMeta& fm = meta_[static_cast<size_t>(k)];
    const double segStart = arrival_[static_cast<size_t>(k)];
    const double holdK = milestone(k) ? dwell_ : 0.0;

    if (vt < segStart + holdK) {
        // 里程碑停留：整段显示帧 k。
        ds.lo = ds.hi = k;
        ds.statFrame = k;
        ds.alpha = 0.0;
        ds.interpolating = false;
        ds.globalT = fm.t;
    } else if (k + 1 < n) {
        const double gapEnd = arrival_[static_cast<size_t>(k + 1)];
        const double gapDur = gapEnd - (segStart + holdK);
        if (gapDur <= 1e-9) {
            ds.lo = ds.hi = k;
            ds.statFrame = k;
            ds.interpolating = false;
            ds.globalT = fm.t;
        } else {
            const FrameMeta& fb = meta_[static_cast<size_t>(k + 1)];
            ds.lo = k;
            ds.hi = k + 1;
            ds.alpha = std::clamp((vt - (segStart + holdK)) / gapDur, 0.0, 1.0);
            ds.interpolating = true;
            ds.statFrame = (ds.alpha < 0.5) ? k : k + 1;
            ds.globalT = fm.t + ds.alpha * (fb.t - fm.t);
        }
    } else {
        ds.lo = ds.hi = k;
        ds.statFrame = k;
        ds.interpolating = false;
        ds.globalT = fm.t;
    }

    const FrameMeta& stat = meta_[static_cast<size_t>(ds.statFrame)];
    ds.label = stat.label;
    ds.stage = stat.stage;
    ds.vt = vt;
    return ds;
}

void PlaybackController::nextKeyframe() {
    if (meta_.empty()) return;
    const int n = static_cast<int>(meta_.size());
    const DisplayState ds = state();
    // ③ 以当前显示段为准：处于 lo->hi 插值中段则跳到终点 hi；
    //    恰在关键帧（alpha≈0 且非插值，或 alpha≈1）才 ±1。
    int target;
    if (ds.interpolating && ds.alpha < 1.0 - 1e-6) target = ds.hi;
    else target = (ds.interpolating ? ds.hi : ds.lo) + 1;
    if (target >= n) target = n - 1;
    pinIndex(target);
}

void PlaybackController::prevKeyframe() {
    if (meta_.empty()) return;
    const int n = static_cast<int>(meta_.size());
    const DisplayState ds = state();
    int target;
    if (ds.interpolating && ds.alpha > 1e-6) target = ds.lo;   // 插值中段回到段起点
    else target = (ds.interpolating ? ds.lo : ds.lo) - 1;      // 关键帧回上一关键帧
    if (target < 0) target = 0;
    pinIndex(target);
}

void PlaybackController::nextStage() {
    if (meta_.empty()) return;
    const int n = static_cast<int>(meta_.size());
    const int lo = state().lo;
    const std::string cur = meta_[static_cast<size_t>(lo)].stage;
    for (int i = lo + 1; i < n; ++i) {
        if (meta_[static_cast<size_t>(i)].stage != cur) {
            pinIndex(i);   // 下一阶段首帧
            return;
        }
    }
    pinIndex(n - 1);
}

void PlaybackController::prevStage() {
    if (meta_.empty()) return;
    const int n = static_cast<int>(meta_.size());
    const std::string cur = meta_[static_cast<size_t>(state().lo)].stage;
    int s0 = 0;
    while (s0 < n && meta_[static_cast<size_t>(s0)].stage == cur) ++s0;
    if (s0 == 0) { pinIndex(0); return; }   // 已在首阶段
    const std::string prev = meta_[static_cast<size_t>(s0 - 1)].stage;
    int j = s0 - 1;
    while (j > 0 && meta_[static_cast<size_t>(j - 1)].stage == prev) --j;
    pinIndex(j);        // 上一阶段首帧
}

void PlaybackController::toStart() {
    if (meta_.empty()) return;
    pinIndex(0);
}

void PlaybackController::toEnd() {
    if (meta_.empty()) return;
    pinIndex(static_cast<int>(meta_.size()) - 1);
}

void PlaybackController::pinIndex(int idx) {
    if (meta_.empty()) return;
    idx = std::clamp(idx, 0, static_cast<int>(meta_.size()) - 1);
    pinned_ = idx;
    vt_ = arrival_[static_cast<size_t>(idx)];
    playing_ = false;
}

double PlaybackController::currentHoldElapsed() const {
    if (meta_.empty() || pinned_ >= 0) return 0.0;
    const int n = static_cast<int>(meta_.size());
    for (int k = 0; k < n; ++k) {
        if (!milestone(k)) continue;
        const double segStart = arrival_[static_cast<size_t>(k)];
        if (vt_ >= segStart && vt_ < segStart + dwell_)
            return vt_ - segStart;     // 停在 hold 内：调速保留已走时长
    }
    return 0.0;
}

double PlaybackController::vtForGlobalT(double g) const {
    const int n = static_cast<int>(meta_.size());
    if (n == 0) return 0.0;
    const double t0 = meta_[0].t;
    const double tlast = meta_[static_cast<size_t>(n - 1)].t;
    g = std::clamp(g, t0, tlast);
    for (int k = 0; k < n - 1; ++k) {
        const double ta = meta_[static_cast<size_t>(k)].t;
        const double tb = meta_[static_cast<size_t>(k + 1)].t;
        const double holdK = milestone(k) ? dwell_ : 0.0;
        if (hardCut(k)) {
            // 硬切/同 t：global t 在 ta 处停留，随后瞬时切到 tb。落到 ta 的停留段。
            if (g <= tb + 1e-9) return arrival_[static_cast<size_t>(k)] + holdK;
        } else if (g >= ta - 1e-9 && g <= tb + 1e-9) {
            if (g <= ta + 1e-9) return arrival_[static_cast<size_t>(k)];  // 处 hold 起点
            const double gapStart = arrival_[static_cast<size_t>(k)] + holdK;
            const double gapDur = arrival_[static_cast<size_t>(k + 1)] - gapStart;
            const double frac = (tb > ta) ? (g - ta) / (tb - ta) : 0.0;
            return gapStart + std::clamp(frac, 0.0, 1.0) * gapDur;
        }
    }
    return arrival_[static_cast<size_t>(n - 1)];
}

double PlaybackController::globalTRange() const {
    if (meta_.empty()) return 1.0;
    const double r = meta_.back().t - meta_.front().t;
    return r > 1e-9 ? r : 1.0;
}

double PlaybackController::playheadGlobal01() const {
    if (meta_.empty()) return 0.0;
    return (state().globalT - meta_.front().t) / globalTRange();
}

void PlaybackController::scrubToGlobal01(double g01) {
    if (meta_.empty()) return;
    g01 = std::clamp(g01, 0.0, 1.0);
    pinned_ = -1;
    const double g = meta_.front().t + g01 * globalTRange();
    vt_ = vtForGlobalT(g);
}

}  // namespace viz
