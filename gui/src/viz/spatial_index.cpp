#include "viz/spatial_index.h"

#include <algorithm>
#include <cmath>

namespace viz {

void SpatialIndex::build(const Rect& chip, const float* x, const float* y,
                         const NodeGeometry& geom, int gridCols, int gridRows) {
    *this = SpatialIndex{};
    cols_ = std::max(1, gridCols);
    rows_ = std::max(1, gridRows);
    chip_ = chip;
    cellW_ = chip.width() / static_cast<float>(cols_);
    cellH_ = chip.height() / static_cast<float>(rows_);
    if (cellW_ <= 0.f || cellH_ <= 0.f) return;

    const int n = static_cast<int>(geom.w.size());
    const int buckets = cols_ * rows_;
    std::vector<int> count(static_cast<size_t>(buckets), 0);

    // 每个记录覆盖的桶区间（第一趟算出并保存，第二趟复用，避免重复求交）。
    std::vector<int> covC0, covC1, covR0, covR1;
    covC0.reserve(n); covC1.reserve(n); covR0.reserve(n); covR1.reserve(n);
    orig_.reserve(n);
    kd_.reserve(n);
    cx_.reserve(n);
    cy_.reserve(n);
    rw_.reserve(n);
    rh_.reserve(n);

    auto clampC = [&](int c) { return std::clamp(c, 0, cols_ - 1); };
    auto clampR = [&](int r) { return std::clamp(r, 0, rows_ - 1); };

    // 第一趟：过滤零面积/F_NI，拷几何，计算覆盖桶区间并统计每桶计数。
    for (int i = 0; i < n; ++i) {
        const bool skip = (geom.flags[i] & F_NI) != 0 || geom.w[i] <= 0.f || geom.h[i] <= 0.f;
        if (skip) continue;

        const float lx = x[i] - geom.w[i] * 0.5f, rx = x[i] + geom.w[i] * 0.5f;
        const float ly = y[i] - geom.h[i] * 0.5f, hy = y[i] + geom.h[i] * 0.5f;
        int c0 = clampC(static_cast<int>(std::floor((lx - chip_.lx) / cellW_)));
        int c1 = clampC(static_cast<int>(std::floor((rx - chip_.lx) / cellW_)));
        int r0 = clampR(static_cast<int>(std::floor((ly - chip_.ly) / cellH_)));
        int r1 = clampR(static_cast<int>(std::floor((hy - chip_.ly) / cellH_)));

        orig_.push_back(i);
        kd_.push_back(geom.kind[i]);
        cx_.push_back(x[i]);
        cy_.push_back(y[i]);
        rw_.push_back(geom.w[i]);
        rh_.push_back(geom.h[i]);
        covC0.push_back(c0); covC1.push_back(c1);
        covR0.push_back(r0); covR1.push_back(r1);

        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                ++count[static_cast<size_t>(r * cols_ + c)];
    }

    // CSR 前缀和。
    start_.assign(static_cast<size_t>(buckets) + 1, 0);
    for (int b = 0; b < buckets; ++b)
        start_[static_cast<size_t>(b + 1)] = start_[static_cast<size_t>(b)] + count[static_cast<size_t>(b)];

    items_.resize(static_cast<size_t>(start_[static_cast<size_t>(buckets)]));
    std::vector<int> cursor = start_;

    // 第二趟：把记录写进它覆盖的每个桶。单元通常只覆盖 1 桶，宏覆盖几桶，
    // 总量与节点数同量级——多桶登记保证宏的边缘也能命中，而不是只在中心桶。
    for (size_t k = 0; k < orig_.size(); ++k) {
        for (int r = covR0[k]; r <= covR1[k]; ++r)
            for (int c = covC0[k]; c <= covC1[k]; ++c) {
                const int b = r * cols_ + c;
                items_[static_cast<size_t>(cursor[static_cast<size_t>(b)]++)] = static_cast<int>(k);
            }
    }
}

int SpatialIndex::pick(float wx, float wy, const bool* visible) const {
    if (!valid() || orig_.empty()) return -1;
    const int c = static_cast<int>((wx - chip_.lx) / cellW_);
    const int r = static_cast<int>((wy - chip_.ly) / cellH_);
    if (c < 0 || c >= cols_ || r < 0 || r >= rows_) return -1;

    int best = -1;
    float bestArea = 0.f;
    // 记录已登记进覆盖的每个桶，查本桶即可；邻域循环是防御性冗余，开销可忽略。
    for (int dr = -1; dr <= 1; ++dr) {
        const int rr = r + dr;
        if (rr < 0 || rr >= rows_) continue;
        for (int dc = -1; dc <= 1; ++dc) {
            const int cc = c + dc;
            if (cc < 0 || cc >= cols_) continue;
            const int b = rr * cols_ + cc;
            for (int p = start_[static_cast<size_t>(b)]; p < start_[static_cast<size_t>(b + 1)]; ++p) {
                const size_t k = static_cast<size_t>(items_[static_cast<size_t>(p)]);
                // 隐藏图层不参与命中：既不选中，也不会用小物体挡住可见大物体。
                if (visible && !visible[kd_[k]]) continue;
                const float halfW = rw_[k] * 0.5f, halfH = rh_[k] * 0.5f;
                if (std::fabs(wx - cx_[k]) > halfW || std::fabs(wy - cy_[k]) > halfH) continue;
                const float area = rw_[k] * rh_[k];
                // 重叠时面积最小者优先：压在宏上的小单元才能被选中。
                if (best < 0 || area < bestArea) {
                    best = orig_[k];
                    bestArea = area;
                }
            }
        }
    }
    return best;
}

int SpatialIndex::bucketCount(int col, int row) const {
    if (col < 0 || col >= cols_ || row < 0 || row >= rows_) return 0;
    const int b = row * cols_ + col;
    return start_[static_cast<size_t>(b + 1)] - start_[static_cast<size_t>(b)];
}

}  // namespace viz
