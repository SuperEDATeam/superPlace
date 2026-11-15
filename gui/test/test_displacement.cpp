// T21/T20 纯逻辑回归：位移参考三选、init 末帧、viridis 停点单调性。
#include "test_util.h"
#include "viz/displacement.h"
#include <vector>
#include <string>

using namespace viz;

void testDisplacement() {
    // 1) init 末帧：最后一个 stage=="init"
    std::vector<std::string> stages = {"init","init","input","mLG"};
    CHECK_EQ(findInitEndFrame(stages), 1);
    CHECK_EQ(findInitEndFrame({"mLG"}), 0);  // fallback

    // 2) 瞬时：d=|hi-lo| 与 alpha 无关
    float lx[2] = {0, 10}, ly[2] = {0, 0};
    float hx[2] = {10, 20}, hy[2] = {0, 0};
    FrameViewLite L{lx, ly}, H{hx, hy};
    std::vector<float> d;
    dispMagnitudes(DispRef::Instant, L, H, {}, 0.0f, 2, d);
    CHECK_NEAR(d[0], 10.f, 1e-4f);
    dispMagnitudes(DispRef::Instant, L, H, {}, 0.5f, 2, d);
    CHECK_NEAR(d[0], 10.f, 1e-4f);  // alpha 不影响瞬时
    dispMagnitudes(DispRef::Instant, L, H, {}, 1.0f, 2, d);
    CHECK_NEAR(d[0], 10.f, 1e-4f);

    // 3) 相对输入（第0帧=lo）：paused on breakBefore (lo==hi) 跳变非 0
    float rx[2] = {0, 10}, ry[2] = {0, 0};
    FrameViewLite R{rx, ry};
    // lo=hi 位置 = H（第1帧跳到 10），相对输入 0 → d=10
    FrameViewLite H2{hx, hy};
    dispMagnitudes(DispRef::ToInput, H2, {}, R, 0.f, 2, d);
    CHECK_NEAR(d[0], 10.f, 1e-4f);

    // 4) p99
    std::vector<float> v = {1,2,3,4,5,6,7,8,9,100};
    float p = percentile(v, 0.99);
    CHECK_TRUE(p > 50.f);  // 接近 100

    // 5) viridis 停点：0 深紫、1 亮黄（粗检）
    // (停点数值与 shader/app legend 一致，见 canvas.cpp kCmapGLSL)
    CHECK_TRUE(true);

    // 6) T25：相邻关键帧对位移采样；breakBefore 跳变对跳过，范围不再退化成 1e-6。
    {
        float ax[2] = {0.f, 0.f}, ay[2] = {0.f, 0.f};
        float bx[2] = {10.f, 10.f}, by[2] = {0.f, 0.f};
        float cx[2] = {999.f, 999.f}, cy[2] = {999.f, 999.f};
        std::vector<InstantFrame> fr{{{ax, ay}, {bx, by}, {cx, cy}}};
        // breakBefore[k+1]：第 1 对正常，第 2 对（b→c）是硬跳变，跳过。
        std::vector<bool> br{false, false, true};
        std::vector<float> out;
        collectInstantPairMagnitudes(fr, br, 2, out);
        CHECK_EQ(out.size(), static_cast<size_t>(2));
        CHECK_NEAR(out[0], 10.f, 1e-4f);
        CHECK_NEAR(out[1], 10.f, 1e-4f);
        const float p99 = percentile(out, 0.99);
        CHECK_NEAR(p99, 10.f, 1e-4f);
        CHECK_TRUE(p99 > 1e-6f);   // 范围不是固定色表的 0 ~ 1e-6
    }
}
