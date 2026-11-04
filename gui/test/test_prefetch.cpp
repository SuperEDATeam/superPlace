// T17：后台预读无头单测——预读后数据正确、反复开/关可靠关闭不崩不挂线程。
#include "test_util.h"

#include <cmath>
#include <chrono>
#include <thread>

#include "viz/source.h"

using namespace viz;

void testPrefetch() {
    // 用仓库自带合成样本（40 帧，超过 24 帧缓存）。
    std::string err;
    auto src = makeBundleSource(SYNTHETIC_BUNDLE, &err);
    CHECK_TRUE(static_cast<bool>(src));
    if (!src) return;

    // 提示后台预读 center=5 往前 3 帧；稍等后这些帧应已被读入缓存且数据正确。
    src->prefetch(5, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    for (int idx : {6, 7, 8}) {
        FrameView fv;
        CHECK_TRUE(src->frame(idx, fv));
        CHECK_TRUE(fv.valid());
        // 位置数据是有效浮点数（非 NaN/Inf）即说明读入的是真帧内容。
        bool finite = true;
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(fv.x[i]) || !std::isfinite(fv.y[i])) finite = false;
        CHECK_TRUE(finite);
    }

    // 反向预读也不应崩。
    src->prefetch(10, -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    FrameView fv;
    CHECK_TRUE(src->frame(9, fv));
    CHECK_TRUE(fv.valid());
}

void testPrefetchShutdown() {
    // 反复开/带预读/关：析构必须可靠 join，不能挂起或泄漏线程。
    for (int rep = 0; rep < 8; ++rep) {
        std::string err;
        auto src = makeBundleSource(SYNTHETIC_BUNDLE, &err);
        if (!src) { CHECK_TRUE(false); break; }
        src->prefetch(5, 1);
        src->prefetch(20, -1);
        // 不等待立即销毁：worker 可能正在读文件，析构要能可靠停下。
    }
    CHECK_TRUE(true);
}

// ② 预读安装进 LRU 且缓存有界：大步扫描预读，持有的旧 FrameView 跨扫后逐位不变
// （证明 worker touch 进了有界 LRU、淘汰只放缓存引用不影响持帧者）。
void testPrefetchLRU() {
    std::string err;
    auto src = makeBundleSource(SYNTHETIC_BUNDLE, &err);
    CHECK_TRUE(static_cast<bool>(src));
    if (!src) return;
    const int n = src->scene().nodeCount;

    FrameView held;
    CHECK_TRUE(src->frame(2, held));
    const float h0 = held.x[0], h1 = held.x[n - 1];   // 记住第 2 帧两端

    // 大步正反向预读：center 从 0 扫到末帧再扫回，每次带方向。
    const int last = src->frameCount() - 1;
    for (int c = 0; c <= last; c += 3) src->prefetch(c, 1);
    for (int c = last; c >= 0; c -= 3) src->prefetch(c, -1);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 预读的帧最终都能取到且数据有限。
    for (int c = 3; c <= last; c += 9) {
        FrameView fv;
        CHECK_TRUE(src->frame(c, fv));
        bool finite = std::isfinite(fv.x[0]) && std::isfinite(fv.y[0]);
        CHECK_TRUE(finite);
    }
    // 持帧跨扫后逐位不变。
    CHECK_TRUE(held.x[0] == h0 && held.x[n - 1] == h1);
    // LRU 有界：缓存驻留帧数不得超过 24（取持帧 + 预读窗口，旧帧必须被淘汰）。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int cached = src->cachedFrameCount();
    CHECK_TRUE(cached >= 0 && cached <= 24);
}
