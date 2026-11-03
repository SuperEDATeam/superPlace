// T11：实时源四阶段序列的无头测试。
//
// 不开窗口、不建 GL 上下文：直接 makeLiveSource 跑一个带少量可移动宏的迷你
// bookshelf 夹具，worker 线程异步出帧，主线程轮询到 "done" 帧后断言：
//   1. 帧阶段顺序 = init → mGP* → mLG → FILLERONLY* → cGP* → done；
//   2. mLG 恰好一帧（一次性跳变），且其 label 不含 "iter"（不伪造迭代）；
//   3. mGP/cGP 帧带真实迭代号；
//   4. mLG 之后宏位置逐位不再移动（FILLERONLY/cGP 都不该碰宏）；
//   5. mLG 相对 mGP 末帧确实发生了宏跳变（跳变清晰）。
//
// 只在集成构建（-DSUPERPLACE_ROOT）下编译——它要链 libsuperplace。
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"
#include "viz/live_source.h"
#include "viz/scene.h"
#include "viz/source.h"

#ifndef TINY_MACROS_AUX
#error "TINY_MACROS_AUX 未定义（应在 CMake 里指向迷你夹具 tiny.aux）"
#endif

namespace {

/// 轮询直到出现 "done" 帧（worker 的最后一帧）。超时返回空串。
std::string waitForDone(viz::IPlacementSource& src) {
    viz::FrameView v;
    for (int t = 0; t < 1200; ++t) {  // 最多 ~120s
        const int n = src.frameCount();
        if (n > 0 && src.frame(n - 1, v) && v.stage == "done") return v.stage;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {};
}

}  // namespace

void testLiveStages() {
    std::string err;
    auto src = viz::makeLiveSource(TINY_MACROS_AUX, /*strideIters=*/1, &err);
    CHECK_TRUE(src != nullptr);
    if (!src) {
        std::fprintf(stderr, "makeLiveSource 失败：%s\n", err.c_str());
        return;
    }

    CHECK_TRUE(!waitForDone(*src).empty());

    const int n = src->frameCount();
    CHECK_TRUE(n >= 6);

    // ---- 几何逐节点一致性（防 buildStatic 把 kind 错按类别值 k 索引而非按节点 i）
    const int total = src->scene().nodeCount;
    CHECK_EQ(static_cast<int>(src->geometry().kind.size()), total);
    CHECK_EQ(static_cast<int>(src->geometry().flags.size()), total);
    CHECK_EQ(static_cast<int>(src->geometry().w.size()), total);
    CHECK_EQ(static_cast<int>(src->geometry().h.size()), total);
    for (int i = 0; i < total; ++i) {
        const viz::Kind expect = viz::kindOf(src->geometry().flags[static_cast<size_t>(i)]);
        CHECK_EQ(static_cast<int>(src->geometry().kind[static_cast<size_t>(i)]),
                 static_cast<int>(expect));
    }
    int counted = 0;
    for (int k = 0; k < 4; ++k) counted += src->scene().countByKind[k];
    CHECK_EQ(counted, total);

    std::vector<std::string> stages(static_cast<size_t>(n));
    std::vector<std::string> labels(static_cast<size_t>(n));
    std::vector<const float*> xs(static_cast<size_t>(n));
    std::vector<const float*> ys(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        viz::FrameView v;
        CHECK_TRUE(src->frame(i, v));
        stages[i] = v.stage;
        labels[i] = v.label;
        xs[i] = v.x;
        ys[i] = v.y;
    }

    // ---- 首尾里程碑
    CHECK_STREQ(stages.front().c_str(), "init");
    CHECK_STREQ(stages.back().c_str(), "done");

    auto countStage = [&](const char* s) {
        int c = 0;
        for (const auto& x : stages)
            if (x == s) ++c;
        return c;
    };
    auto findFirst = [&](const char* s) {
        for (int i = 0; i < n; ++i)
            if (stages[i] == s) return i;
        return -1;
    };
    auto findLast = [&](const char* s) {
        for (int i = n - 1; i >= 0; --i)
            if (stages[i] == s) return i;
        return -1;
    };

    // ---- 四个阶段都出现，mLG 恰好一帧（一次性跳变）
    CHECK_TRUE(countStage("mGP") >= 1);
    CHECK_EQ(countStage("mLG"), 1);
    CHECK_TRUE(countStage("FILLERONLY") >= 1);
    CHECK_TRUE(countStage("cGP") >= 1);

    // ---- 严格先后顺序：mGP < mLG < FILLERONLY < cGP
    const int iMGP = findFirst("mGP");
    const int iMLG = findFirst("mLG");
    const int iFO = findFirst("FILLERONLY");
    const int iCGP = findFirst("cGP");
    CHECK_TRUE(iMGP >= 0);
    CHECK_TRUE(iMLG > iMGP);
    CHECK_TRUE(iFO > iMLG);
    CHECK_TRUE(iCGP > iFO);
    // mLG 之后不该再出现 mGP（四阶段只跑一遍 mGP）
    CHECK_TRUE(findLast("mGP") < iMLG);

    // ---- mLG 是跳变而非迭代：label 不得伪造迭代号；GP 帧则必须带真实迭代号
    CHECK_TRUE(labels[static_cast<size_t>(iMLG)].find("iter") == std::string::npos);
    CHECK_TRUE(labels[static_cast<size_t>(iMGP)].find("iter") != std::string::npos);
    CHECK_TRUE(labels[static_cast<size_t>(iCGP)].find("iter") != std::string::npos);

    // ---- 宏节点下标（几何在 buildStatic 时已定型，此时宏尚 movable，kind=Macro）
    const auto& geom = src->geometry();
    std::vector<int> macros;
    for (int i = 0; i < src->scene().nodeCount; ++i)
        if (geom.kind[static_cast<size_t>(i)] == static_cast<uint8_t>(viz::Kind::Macro))
            macros.push_back(i);
    CHECK_TRUE(!macros.empty());

    // ---- mLG 相对 mGP 末帧确实发生了宏跳变（跳变清晰，不是原地不动）
    const int iMGPend = findLast("mGP");
    int jumped = 0;
    for (int idx : macros) {
        if (xs[static_cast<size_t>(iMGPend)][idx] != xs[static_cast<size_t>(iMLG)][idx] ||
            ys[static_cast<size_t>(iMGPend)][idx] != ys[static_cast<size_t>(iMLG)][idx])
            ++jumped;
    }
    CHECK_TRUE(jumped >= 1);

    // ---- mLG 之后宏逐位不再动（到 done 帧）
    int movedAfterMlg = 0;
    for (int idx : macros) {
        if (xs[static_cast<size_t>(iMLG)][idx] != xs[static_cast<size_t>(n - 1)][idx] ||
            ys[static_cast<size_t>(iMLG)][idx] != ys[static_cast<size_t>(n - 1)][idx])
            ++movedAfterMlg;
    }
    CHECK_EQ(movedAfterMlg, 0);
}

int main() {
    testLiveStages();
    std::printf("viz_live_tests: %d 项检查, %d 失败\n", ::viztest::checks(),
                ::viztest::failures());
    return ::viztest::failures() ? 1 : 0;
}
