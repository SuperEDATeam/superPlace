#pragma once
#include <vector>
#include <algorithm>

namespace viz {

/// 视口剔除纯函数：给定节点几何/位置/层可见性/相机，返回可见节点索引（原顺序保留）。
/// 与 Canvas::rebuildActive 用同一判定，单测直接覆盖。
/// x0,y0,x1,y1 为世界坐标视口（已含 minPixelSize 扩张）。
inline std::vector<int> cullVisible(const float* cx, const float* cy,
                                   const float* halfW, const float* halfH,
                                   const float* kind, int count,
                                   const bool layerVisible[4],
                                   float x0, float x1, float y0, float y1) {
    std::vector<int> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        int k = (int)kind[i];
        if (k < 0 || k >= 4 || !layerVisible[k]) continue;
        if (cx[i] + halfW[i] < x0 || cx[i] - halfW[i] > x1) continue;
        if (cy[i] + halfH[i] < y0 || cy[i] - halfH[i] > y1) continue;
        out.push_back(i);
    }
    return out;
}

}  // namespace viz
