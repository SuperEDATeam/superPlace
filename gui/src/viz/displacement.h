// T21 位移参考纯逻辑（可无头单测，不依赖 GL/ImGui）。
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace viz {

struct FrameViewLite {
    const float* x = nullptr;
    const float* y = nullptr;
};

// 位移参考模式
enum class DispRef {
    Instant = 0,   // |pos(hi) - pos(lo)|，与 alpha 无关
    ToInitEnd = 1, // 当前插值位置 - init 末帧位置
    ToInput = 2,   // 当前插值位置 - 第 0 帧位置
};

// 找 init 末帧：最后一个 stage=="init" 的帧；fallback 0。
inline int findInitEndFrame(const std::vector<std::string>& stages) {
    int last = 0;
    for (size_t i = 0; i < stages.size(); ++i)
        if (stages[i] == "init") last = (int)i;
    return last;
}

// 计算每节点位移幅值。lo/hi 为关键帧位置；alpha 为区间插值；ref 为参考帧位置。
// mode Instant: d=|hi-lo|（与 alpha 无关）；其他: d=|lerp(lo,hi,alpha)-ref|。
inline void dispMagnitudes(DispRef mode, const FrameViewLite& lo, const FrameViewLite& hi,
                           const FrameViewLite& ref, float alpha, int n,
                           std::vector<float>& out) {
    out.assign(n, 0.f);
    if (!lo.x) return;
    for (int i = 0; i < n; ++i) {
        float cx = lo.x[i], cy = lo.y[i];
        if (hi.x && hi.y) {
            cx += alpha * (hi.x[i] - cx);
            cy += alpha * (hi.y[i] - cy);
        }
        float dx, dy;
        if (mode == DispRef::Instant) {
            // |hi-lo|，与 alpha 无关（整段区间不变）
            dx = hi.x ? hi.x[i] - lo.x[i] : 0.f;
            dy = hi.y ? hi.y[i] - lo.y[i] : 0.f;
        } else {
            float rx = cx, ry = cy;
            if (ref.x) { rx = ref.x[i]; ry = ref.y[i]; }
            dx = cx - rx; dy = cy - ry;
        }
        out[i] = std::sqrt(dx * dx + dy * dy);
    }
}

// 第 p 百分位（输入会被拷贝排序）。
inline float percentile(std::vector<float> v, double p) {
    if (v.empty()) return 1.f;
    std::sort(v.begin(), v.end());
    size_t idx = (size_t)(v.size() * p);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

}  // namespace viz
