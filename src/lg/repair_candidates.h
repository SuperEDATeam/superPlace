#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "db/geometry.h"

namespace sp {

// Grid sampling can step over a narrow legal gap. Include contact coordinates
// on both sides of every nearby rectangle, together with the original point.
inline std::vector<std::pair<float, float>> contactCandidates(
    float originalX, float originalY, float currentX, float currentY,
    float width, float height, const Rect& core, const std::vector<Rect>& obstacles,
    double radius) {
    const float hw = width * .5f, hh = height * .5f;
    std::vector<float> xs{originalX, currentX, core.lx + hw, core.hx - hw};
    std::vector<float> ys{originalY, currentY, core.ly + hh, core.hy - hh};
    for (const Rect& b : obstacles) {
        if (b.hx < originalX - radius - hw || b.lx > originalX + radius + hw ||
            b.hy < originalY - radius - hh || b.ly > originalY + radius + hh) continue;
        xs.push_back(b.lx - hw);
        xs.push_back(b.hx + hw);
        ys.push_back(b.ly - hh);
        ys.push_back(b.hy + hh);
    }
    auto unique = [](auto& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    unique(xs);
    unique(ys);
    std::vector<std::pair<float, float>> out;
    for (float x : xs) for (float y : ys) {
        if (x < core.lx + hw || x > core.hx - hw ||
            y < core.ly + hh || y > core.hy - hh ||
            std::hypot(double(x - originalX), double(y - originalY)) > radius) continue;
        out.emplace_back(x, y);
    }
    return out;
}

}  // namespace sp
