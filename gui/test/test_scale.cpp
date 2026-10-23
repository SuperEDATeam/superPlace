// T13：niceStep 取 1/2/5×10ⁿ；scaleBarLength 给出约目标像素长的好看长度。
#include "test_util.h"

#include "render/scale_tick.h"

using namespace viz;

void testScale() {
    CHECK_NEAR(niceStep(0.3f), 0.5f, 1e-6);    // 0.3 -> 0.5
    CHECK_NEAR(niceStep(1.2f), 2.f, 1e-6);    // 1.2 -> 2
    CHECK_NEAR(niceStep(6.f), 10.f, 1e-6);     // 6 -> 10
    CHECK_NEAR(niceStep(42.f), 50.f, 1e-6);    // 42 -> 50
    CHECK_NEAR(niceStep(700.f), 1000.f, 1e-6);
    CHECK_NEAR(niceStep(0.f), 1.f, 1e-6);      // 退化

    // 目标 140px、2px/unit -> 原始 70 单位 -> 好看步长 100
    CHECK_NEAR(scaleBarLength(140.f, 2.f), 100.f, 1e-6);
    // 缩放变大（px/unit 大），比例尺世界长度应变小
    const float a = scaleBarLength(140.f, 2.f);
    const float b = scaleBarLength(140.f, 20.f);
    CHECK_TRUE(b < a);
}
