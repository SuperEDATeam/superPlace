// DensityCache 无 GL 单测：并发发布一致、立即析构不挂、预算边界、失败/异常帧回退、
// 与同步 DensityField 真值一致、切 source 生命周期安全。
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "viz/density.h"
#include "viz/density_cache.h"

using namespace viz;

// 模拟同步算法逐帧构建：固定输入应得到确定性结果。
static DensityFieldData buildSync(int i) {
    const int n = 16;
    NodeGeometry geom;
    geom.w.resize(n, 2.f);
    geom.h.resize(n, 2.f);
    geom.flags.resize(n, 0u);
    geom.kind.resize(n, 0);
    std::vector<float> x(n), y(n);
    for (int k = 0; k < n; ++k) {
        x[k] = static_cast<float>((i * 7 + k) % 32);
        y[k] = static_cast<float>((i * 5 + k * 3) % 32);
    }
    DensityField df;
    Rect chip{0.f, 0.f, 32.f, 32.f};
    df.build(chip, x.data(), y.data(), geom, 8, 8);
    DensityFieldData out;
    out.data = df.field();
    out.maxNorm = df.maxNorm();
    out.ok = true;
    return out;
}

void testDrawValidPolicy() {
    // 与 App 共用 viz::shouldDrawPositions / shouldUploadPositions。
    CHECK_TRUE(!viz::shouldDrawPositions(false));
    CHECK_TRUE(!viz::shouldUploadPositions(false, true, false));
    CHECK_TRUE(!viz::shouldUploadPositions(true, false, false)); // 无指针不补传
    CHECK_TRUE(viz::shouldDrawPositions(true));
    CHECK_TRUE(viz::shouldUploadPositions(true, true, false));
    CHECK_TRUE(!viz::shouldUploadPositions(true, true, true)); // LOD且播放跳上传
}

void testDensityCache() {
    DensityCache c;
    std::atomic<int> built{0};
    c.start(8, 64, [&](int i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++built;
        return buildSync(i);
    });
    // 并发 reader：worker 仍在生产时轮询 get()，已发布帧必须与同步真值逐字段一致。
    std::atomic<bool> readerStop{false};
    std::atomic<int> readerChecks{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 2; ++t)
        readers.emplace_back([&]() {
            while (!readerStop.load()) {
                for (int i = 0; i < 8; ++i) {
                    DensityFieldData got = c.get(i);
                    if (!got.ok) continue; // 未发布/失败帧跳过
                    DensityFieldData truth = buildSync(i);
                    if (got.data.size() != truth.data.size()) continue;
                    bool ok = true;
                    for (size_t k = 0; k < got.data.size(); ++k)
                        if (std::abs(got.data[k] - truth.data[k]) > 1e-4f) ok = false;
                    if (std::abs(got.maxNorm - truth.maxNorm) >= 1e-4f) ok = false;
                    if (!ok) { CHECK_TRUE(false); }
                    ++readerChecks;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    for (int wait = 0; wait < 400 && c.ready() < 8; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    readerStop = true;
    for (auto& th : readers) th.join();
    CHECK_TRUE(readerChecks.load() > 0);
    CHECK_TRUE(c.done());
    CHECK_EQ(c.ready(), 8);
    for (int i = 0; i < 8; ++i) {
        DensityFieldData got = c.get(i);
        DensityFieldData truth = buildSync(i);
        CHECK_TRUE(got.ok);
        CHECK_EQ(got.data.size(), truth.data.size());
        CHECK_TRUE(std::abs(got.maxNorm - truth.maxNorm) < 1e-4f);
        for (size_t k = 0; k < got.data.size(); ++k)
            CHECK_TRUE(std::abs(got.data[k] - truth.data[k]) < 1e-4f);
    }
}

void testDensityCacheBudget() {
    DensityCache c;
    std::atomic<bool> exceeded{false};
    c.start(1000, 16, [&](int i) {
        if (i >= 16) exceeded = true;
        return buildSync(i);
    });
    for (int w = 0; w < 200 && !c.done(); ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_TRUE(c.done());
    CHECK_EQ(c.budgetFrames(), 16);
    CHECK_EQ(c.ready(), 16);
    CHECK_TRUE(!exceeded);
    CHECK_TRUE(!c.get(16).ok);
}

void testDensityCacheFault() {
    DensityCache c;
    c.start(4, 16, [](int i) -> DensityFieldData {
        if (i == 1) return DensityFieldData{};
        if (i == 2) throw std::runtime_error("boom");
        return buildSync(i);
    });
    for (int w = 0; w < 200 && !c.done(); ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_TRUE(c.done());
    CHECK_TRUE(!c.get(1).ok);
    CHECK_TRUE(!c.get(2).ok);
    CHECK_TRUE(c.get(0).ok);
    CHECK_TRUE(c.get(3).ok);
}

void testDensityCacheLifetime() {
    auto c = std::make_unique<DensityCache>();
    c->start(64, 64, [](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return buildSync(0);
    });
    c.reset(); // 立即析构不挂、不 terminate
    // 切 source：stop+join 后重启
    DensityCache c2;
    int gen = 0;
    c2.start(4, 16, [&](int) { ++gen; return buildSync(0); });
    for (int w = 0; w < 100 && !c2.done(); ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    c2.stop(); c2.join();
    int after1 = gen;
    c2.start(4, 16, [&](int) { ++gen; return buildSync(1); });
    for (int w = 0; w < 100 && !c2.done(); ++w)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK_TRUE(c2.done());
    CHECK_EQ(c2.ready(), 4);
    CHECK_TRUE(gen >= after1 + 4);
    // 进行中取消：worker 慢，未完成时 stop+join 必须立即返回、不挂、不跑满。
    DensityCache c3;
    std::atomic<int> built3{0};
    c3.start(200, 200, [&](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++built3;
        return buildSync(0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 跑到中途
    c3.stop();
    c3.join(); // 必须返回
    CHECK_TRUE(built3.load() < 200);
}
