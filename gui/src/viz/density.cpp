#include "viz/density.h"

#include <algorithm>
#include <cmath>

namespace viz {

void DensityField::build(const Rect& chip, const float* x, const float* y,
                         const NodeGeometry& geom, int cols, int rows) {
    const unsigned long prevGen = generation_;
    *this = DensityField{};
    generation_ = prevGen + 1;
    cols_ = std::max(1, cols);
    rows_ = std::max(1, rows);
    chip_ = chip;
    const float bw = chip.width() / static_cast<float>(cols_);
    const float bh = chip.height() / static_cast<float>(rows_);
    binArea_ = bw * bh;
    if (binArea_ <= 0.f) return;

    overlap_.assign(static_cast<size_t>(cols_ * rows_), 0.f);

    const int n = static_cast<int>(geom.w.size());
    for (int i = 0; i < n; ++i) {
        const bool skip = (geom.flags[i] & F_NI) != 0 || geom.w[i] <= 0.f || geom.h[i] <= 0.f;
        if (skip) continue;

        const float lx = x[i] - geom.w[i] * 0.5f, rx = x[i] + geom.w[i] * 0.5f;
        const float ly = y[i] - geom.h[i] * 0.5f, hy = y[i] + geom.h[i] * 0.5f;

        // 节点矩形覆盖的 bin 区间（裁剪到网格内）。
        int c0 = std::clamp(static_cast<int>(std::floor((lx - chip_.lx) / bw)), 0, cols_ - 1);
        int c1 = std::clamp(static_cast<int>(std::floor((rx - chip_.lx) / bw)), 0, cols_ - 1);
        int r0 = std::clamp(static_cast<int>(std::floor((ly - chip_.ly) / bh)), 0, rows_ - 1);
        int r1 = std::clamp(static_cast<int>(std::floor((hy - chip_.ly) / bh)), 0, rows_ - 1);

        for (int r = r0; r <= r1; ++r) {
            const float binLy = chip_.ly + r * bh, binHy = binLy + bh;
            for (int c = c0; c <= c1; ++c) {
                const float binLx = chip_.lx + c * bw, binRx = binLx + bw;
                // 节点矩形与 bin 的真实重叠面积（不是中心点计数）。
                const float ox = std::min(rx, binRx) - std::max(lx, binLx);
                const float oy = std::min(hy, binHy) - std::max(ly, binLy);
                if (ox > 0.f && oy > 0.f)
                    overlap_[static_cast<size_t>(r * cols_ + c)] += ox * oy;
            }
        }
    }

    norm_.resize(overlap_.size());
    for (size_t i = 0; i < overlap_.size(); ++i) {
        norm_[i] = overlap_[i] / binArea_;
        maxNorm_ = std::max(maxNorm_, norm_[i]);
    }
}

float DensityField::sample(float wx, float wy) const {
    if (cols_ <= 0 || rows_ <= 0) return 0.f;
    const float bw = chip_.width() / static_cast<float>(cols_);
    const float bh = chip_.height() / static_cast<float>(rows_);
    const int c = static_cast<int>((wx - chip_.lx) / bw);
    const int r = static_cast<int>((wy - chip_.ly) / bh);
    if (c < 0 || c >= cols_ || r < 0 || r >= rows_) return 0.f;
    return norm_[static_cast<size_t>(r * cols_ + c)];
}

}  // namespace viz
