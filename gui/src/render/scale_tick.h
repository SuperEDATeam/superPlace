// T3 比例尺/坐标轴的"好看刻度"纯逻辑（1/2/5 × 10ⁿ）。
//
// 不依赖 ImGui/GL，单独可测：从全芯片缩放到单个标准单元，比例尺始终给出
// 合理的整数刻度。
#pragma once

#include <cmath>

namespace viz {

/// 取 1/2/5 × 10ⁿ 中 ≥ raw 的最小"好看"步长。raw ≤ 0 时退化为 1。
inline float niceStep(float raw) {
    if (!(raw > 0.f)) return 1.f;
    const float mag = std::pow(10.f, std::floor(std::log10(raw)));
    const float norm = raw / mag;
    float m;
    if (norm <= 1.f) m = 1.f;
    else if (norm <= 2.f) m = 2.f;
    else if (norm <= 5.f) m = 5.f;
    else m = 10.f;
    return m * mag;
}

/// 为左下角比例尺挑一个好看的世界长度：画出来约 targetPx 像素长，
/// 且长度本身是 1/2/5 × 10ⁿ。scalePxPerUnit = Camera::scale。
inline float scaleBarLength(float targetPx, float scalePxPerUnit) {
    if (!(scalePxPerUnit > 0.f)) return 1.f;
    return niceStep(targetPx / scalePxPerUnit);
}

}  // namespace viz
