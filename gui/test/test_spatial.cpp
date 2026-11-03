// T13：空间索引与线性暴力扫描逐项一致；面积最小优先；零面积跳过；越界 -1。
#include "test_util.h"

#include <random>
#include <vector>

#include "viz/spatial_index.h"

using namespace viz;

namespace {

struct Pt { float x, y, w, h; uint8_t flags; };

int bruteForce(const std::vector<Pt>& pts, float wx, float wy) {
    int best = -1;
    float bestArea = 0.f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        if (p.flags & F_NI || p.w <= 0.f || p.h <= 0.f) continue;
        if (std::fabs(wx - p.x) > p.w * 0.5f || std::fabs(wy - p.y) > p.h * 0.5f) continue;
        const float area = p.w * p.h;
        if (best < 0 || area < bestArea) { best = static_cast<int>(i); bestArea = area; }
    }
    return best;
}

}  // namespace

void testSpatial() {
    const std::vector<Pt> pts = {
        {20.f, 20.f, 20.f, 20.f, 1},    // 大宏，覆盖 (20,20)
        {20.f, 20.f, 2.f, 2.f, 0},      // 叠在宏上的小单元：应被优先选中
        {80.f, 80.f, 5.f, 5.f, 0},
        {50.f, 50.f, 0.f, 0.f, 8},      // 零面积 F_NI，跳过
    };
    const int n = static_cast<int>(pts.size());

    NodeGeometry geom;
    geom.w.resize(n);
    geom.h.resize(n);
    geom.flags.resize(n);
    geom.kind.resize(n);
    std::vector<float> x(n), y(n);
    for (int i = 0; i < n; ++i) {
        geom.w[i] = pts[i].w;
        geom.h[i] = pts[i].h;
        geom.flags[i] = pts[i].flags;
        geom.kind[i] = static_cast<uint8_t>(kindOf(pts[i].flags));
        x[i] = pts[i].x;
        y[i] = pts[i].y;
    }

    SpatialIndex idx;
    const Rect chip{0.f, 0.f, 100.f, 100.f};
    idx.build(chip, x.data(), y.data(), geom, 32, 32);

    // 重叠处选面积最小者（小单元 1 而非宏 0）
    CHECK_EQ(idx.pick(20.f, 20.f), 1);
    CHECK_EQ(idx.pick(80.f, 80.f), 2);
    // 零面积节点不命中
    CHECK_EQ(idx.pick(50.f, 50.f), -1);
    // 空白处 / 越界
    CHECK_EQ(idx.pick(5.f, 5.f), -1);
    CHECK_EQ(idx.pick(-50.f, 50.f), -1);
    CHECK_EQ(idx.pick(150.f, 50.f), -1);

    // 随机点与暴力扫描逐项一致
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> d(0.f, 100.f);
    for (int k = 0; k < 300; ++k) {
        const float wx = d(rng), wy = d(rng);
        CHECK_EQ(idx.pick(wx, wy), bruteForce(pts, wx, wy));
    }

    // R1-4：隐藏图层不参与命中。隐藏 Cell(2)：叠在宏上的小单元(1)被跳过，
    // 命中可见的大宏(0)，而不是仍选中小单元。
    bool visible[4] = {true, true, false, true};
    CHECK_EQ(idx.pick(20.f, 20.f, visible), 0);
    // 全部隐藏 -> -1
    bool none[4] = {false, false, false, false};
    CHECK_EQ(idx.pick(20.f, 20.f, none), -1);
}
