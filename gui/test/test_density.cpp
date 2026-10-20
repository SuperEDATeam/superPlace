// T13：密度场——面积重叠累加、零面积跳过、越界采样 0、左密右疏单调。
// 注意这是可视化面积占比，不是优化器 τ（见 density.h 注释）。
#include "test_util.h"

#include <vector>

#include "viz/density.h"

using namespace viz;

void testDensity() {
    const int n = 100;
    NodeGeometry geom;
    geom.w.resize(n);
    geom.h.resize(n);
    geom.flags.resize(n);
    geom.kind.resize(n);
    std::vector<float> x(n), y(n);
    for (int i = 0; i < n; ++i) {
        geom.w[i] = 2.f;
        geom.h[i] = 2.f;
        geom.flags[i] = 0;
        geom.kind[i] = static_cast<uint8_t>(Kind::Cell);
        x[i] = 5.f + (i % 10) * 4.f;          // 全部落在左半 [5,45]
        y[i] = 5.f + (i / 10) * 9.f;
    }
    // 一个零面积节点，必须被跳过
    geom.w.push_back(0.f);
    geom.h.push_back(0.f);
    geom.flags.push_back(F_NI);
    geom.kind.push_back(static_cast<uint8_t>(Kind::Cell));
    x.push_back(80.f);
    y.push_back(80.f);

    DensityField f;
    const Rect chip{0.f, 0.f, 100.f, 100.f};
    f.build(chip, x.data(), y.data(), geom, 10, 10);

    CHECK_TRUE(f.maxNorm() > 0.f);
    CHECK_TRUE(f.sample(20.f, 50.f) > 0.f);    // 左半有密度
    CHECK_NEAR(f.sample(90.f, 50.f), 0.f, 1e-6f);  // 右半空白
    CHECK_NEAR(f.sample(-10.f, 50.f), 0.f, 1e-6f); // 越界
    CHECK_NEAR(f.sample(110.f, 50.f), 0.f, 1e-6f);
}
