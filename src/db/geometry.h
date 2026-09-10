// 基础几何类型。纯 POD，无依赖。
// 规范依据：work_report/05-开发执行规范.md §3.1
#pragma once

#include <algorithm>

namespace sp {

struct Point {
    float x = 0.f;
    float y = 0.f;
};

struct Rect {
    float lx = 0.f, ly = 0.f, hx = 0.f, hy = 0.f;

    float width()  const { return hx - lx; }
    float height() const { return hy - ly; }
    float area()   const { return width() * height(); }
    float cx()     const { return 0.5f * (lx + hx); }
    float cy()     const { return 0.5f * (ly + hy); }

    bool valid() const { return hx > lx && hy > ly; }

    void expand(const Rect& o) {
        lx = std::min(lx, o.lx);
        ly = std::min(ly, o.ly);
        hx = std::max(hx, o.hx);
        hy = std::max(hy, o.hy);
    }
};

/// 两矩形重叠面积，无重叠返回 0。
/// 布局器侧唯一的重叠面积实现；tools/check 必须自行另写一份（05 §4.6）。
inline float overlapArea(const Rect& a, const Rect& b) {
    const float dx = std::min(a.hx, b.hx) - std::max(a.lx, b.lx);
    const float dy = std::min(a.hy, b.hy) - std::max(a.ly, b.ly);
    return (dx > 0.f && dy > 0.f) ? dx * dy : 0.f;
}

/// 一维区间重叠长度，用于按行索引加速（05 §5.5.3 的性能要求）。
inline float overlap1D(float alo, float ahi, float blo, float bhi) {
    const float d = std::min(ahi, bhi) - std::max(alo, blo);
    return d > 0.f ? d : 0.f;
}

}  // namespace sp
