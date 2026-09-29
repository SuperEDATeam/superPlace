// Bin 网格与密度场验证（05 §5.5.3 / §5.5.4 / §5.5.8）。
//
// 重点防三类错误：
//   1. 密度缩放规则搞错（宏要乘 targetDensity，标准单元与 filler 不乘）
//   2. 统计侧与梯度侧的缩放不一致——easyPlace 的已确认缺陷（02 §2.3）
//   3. τ 忘记取正部或误把 filler 计入，导致它失去停止判据的意义
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "gp/bin_grid.h"
#include "test_util.h"

namespace {

/// 在 size×size 的 core 上造一个库。cells 给出 (cx, cy, w, h, flags)。
///
/// 节点按 [可移动 | 固定 | filler] 三段排列——这是 PlaceDB 的约定（05 §3.2）。
/// filler **不在** [0, numMovable) 区间内，因此不会进入 τ 的分母。
sp::PlaceDB makeDb(float size, const std::vector<std::array<float, 5>>& cells) {
    sp::PlaceDB db;
    int movable = 0, fixed = 0, fillers = 0;
    auto classify = [](uint8_t f) {
        if (f & sp::F_FILLER) return 2;
        if (f & sp::F_FIXED) return 1;
        return 0;
    };
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& c : cells) {
            const auto flags = static_cast<uint8_t>(c[4]);
            if (classify(flags) != pass) continue;
            db.addNode("n" + std::to_string(db.node_x.size()), c[2], c[3], flags);
            db.node_x.back() = c[0];
            db.node_y.back() = c[1];
            if (pass == 0) ++movable;
            else if (pass == 1) ++fixed;
            else ++fillers;
        }
    }
    db.numMovable = movable;
    db.numNodes = movable + fixed;
    db.numFillers = fillers;
    db.numPins = 0;
    db.numNets = 0;
    db.net2pin_start.assign(1, 0);
    db.node2pin_start.assign(static_cast<size_t>(db.numNodes) + 1, 0);
    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, size, 0.f, 1.f, static_cast<int>(size)});
    db.computeRegions();
    return db;
}

double sum(const std::vector<float>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0);
}

// ---------------------------------------------------------------- 缩放规则
/// 标准单元不乘 targetDensity，宏单元乘。
void testScalingRule() {
    constexpr float kSize = 64.f;
    constexpr float kTarget = 0.5f;

    // 一个标准单元（高度 = 行高 64? 不，用小尺寸）与一个宏，面积相同
    // 这样密度贡献的差异只可能来自缩放规则
    sp::PlaceDB dbCell = makeDb(kSize, {{32.f, 32.f, 16.f, 16.f, 0}});
    sp::PlaceDB dbMacro = makeDb(kSize, {{32.f, 32.f, 16.f, 16.f, sp::F_MACRO}});

    sp::BinGrid gc, gm;
    gc.initialize(dbCell, kTarget, 8);
    gm.initialize(dbMacro, kTarget, 8);
    gc.accumulate(dbCell);
    gm.accumulate(dbMacro);

    const double sc = sum(gc.nodeDensity());
    const double sm = sum(gm.nodeDensity());
    std::printf("  [scaling]  标准单元 %.2f   宏单元 %.2f   比值 %.4f (期望 %.2f)\n", sc, sm,
                sm / sc, kTarget);
    CHECK_NEAR(sc, 16.0 * 16.0, 1e-3);            // 标准单元：原样计入面积
    CHECK_NEAR(sm, 16.0 * 16.0 * kTarget, 1e-3);  // 宏单元：乘 targetDensity
}

/// filler 不乘 targetDensity，且计入 fillerDensity 而非 nodeDensity
void testFillerSeparate() {
    constexpr float kTarget = 0.5f;
    sp::PlaceDB db = makeDb(64.f, {{32.f, 32.f, 16.f, 16.f, sp::F_FILLER}});
    sp::BinGrid g;
    g.initialize(db, kTarget, 8);
    g.accumulate(db);

    CHECK_NEAR(sum(g.nodeDensity()), 0.0, 1e-4);
    CHECK_NEAR(sum(g.fillerDensity()), 16.0 * 16.0, 1e-3);
}

// ------------------------------------------------------------ local smoothing
/// 比 bin 小的单元被虚拟放大，但总面积必须守恒
void testLocalSmoothingConservesArea() {
    constexpr float kSize = 64.f;
    sp::PlaceDB db = makeDb(kSize, {{32.f, 32.f, 2.f, 2.f, 0}});   // 2x2，远小于 bin
    sp::BinGrid g;
    g.initialize(db, 1.0f, 8);                                      // bin = 8x8
    g.accumulate(db);

    const double total = sum(g.nodeDensity());
    std::printf("  [smoothing] 2x2 单元在 8x8 bin 上累加得 %.4f（期望 4.0）\n", total);
    CHECK_NEAR(total, 4.0, 1e-3);   // 面积守恒

    // 且应当摊到多个 bin 上，而不是集中在一个
    int touched = 0;
    for (float v : g.nodeDensity())
        if (v > 1e-6f) ++touched;
    std::printf("  [smoothing] 覆盖 %d 个 bin（未做平滑时应为 1）\n", touched);
    CHECK_TRUE(touched > 1);
}

// ------------------------------------------------------------------------ τ
/// τ 手算对拍：均匀铺满且恰好达到目标密度时，τ 应为 0
void testOverflowZeroWhenBalanced() {
    constexpr float kSize = 64.f;
    constexpr float kTarget = 1.0f;
    // 64 个 8x8 单元恰好铺满 64x64（8x8 网格，每 bin 一个单元恰好填满）
    std::vector<std::array<float, 5>> cells;
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            cells.push_back({4.f + 8.f * i, 4.f + 8.f * j, 8.f, 8.f, 0});
    sp::PlaceDB db = makeDb(kSize, cells);

    sp::BinGrid g;
    g.initialize(db, kTarget, 8);
    g.accumulate(db);
    const float tau = g.overflow();
    std::printf("  [tau]      恰好铺满时 τ = %.6f（期望 0）\n", tau);
    CHECK_NEAR(tau, 0.0, 1e-4);
}

/// 全部堆在一角时 τ 应显著大于 0
void testOverflowHighWhenCrowded() {
    constexpr float kSize = 64.f;
    std::vector<std::array<float, 5>> cells;
    for (int i = 0; i < 64; ++i) cells.push_back({4.f, 4.f, 8.f, 8.f, 0});   // 全挤在一个 bin
    sp::PlaceDB db = makeDb(kSize, cells);

    sp::BinGrid g;
    g.initialize(db, 1.0f, 8);
    g.accumulate(db);
    const float tau = g.overflow();
    std::printf("  [tau]      全部堆叠时 τ = %.4f（应远大于 0）\n", tau);
    CHECK_TRUE(tau > 0.5f);
}

/// filler 不得计入 τ——否则插入 filler 就能"刷低"溢出率
void testOverflowExcludesFiller() {
    constexpr float kSize = 64.f;
    std::vector<std::array<float, 5>> base;
    for (int i = 0; i < 32; ++i) base.push_back({4.f, 4.f, 8.f, 8.f, 0});

    sp::PlaceDB dbA = makeDb(kSize, base);
    std::vector<std::array<float, 5>> withFiller = base;
    for (int i = 0; i < 32; ++i) withFiller.push_back({4.f, 4.f, 8.f, 8.f, sp::F_FILLER});
    sp::PlaceDB dbB = makeDb(kSize, withFiller);

    sp::BinGrid ga, gb;
    ga.initialize(dbA, 1.0f, 8);
    gb.initialize(dbB, 1.0f, 8);
    ga.accumulate(dbA);
    gb.accumulate(dbB);

    std::printf("  [tau]      无 filler τ=%.4f   有 filler τ=%.4f（应相等）\n", ga.overflow(),
                gb.overflow());
    CHECK_NEAR(ga.overflow(), gb.overflow(), 1e-5);
}

// ------------------------------------------------- 统计侧与梯度侧的缩放一致性
/// 针对 easyPlace 缺陷（02 §2.3）的定向测试。
///
/// 不能用"同一节点分别当标准单元与宏、比较受力"来测——改变节点类型会改变密度场本身，
/// ξ 跟着变，比值并非简单的 targetDensity，那样测出来的东西没有意义。
///
/// 正确的判据是不变量：**gatherForce 侧累加的总电荷必须等于 accumulate 侧的总密度**。
/// 两侧若用了不同的缩放规则，这个等式立刻不成立。
void testChargeMatchesDensity() {
    constexpr float kSize = 128.f;
    constexpr float kTarget = 0.6f;
    // 同时含标准单元、宏、filler，三种缩放规则都覆盖到
    std::vector<std::array<float, 5>> cells;
    for (int i = 0; i < 20; ++i)
        cells.push_back({20.f + 4.f * i, 30.f, 6.f, 6.f, 0});                    // 标准单元
    for (int i = 0; i < 5; ++i)
        cells.push_back({30.f + 18.f * i, 70.f, 16.f, 16.f, sp::F_MACRO});       // 宏
    for (int i = 0; i < 12; ++i)
        cells.push_back({25.f + 7.f * i, 100.f, 5.f, 5.f, sp::F_FILLER});        // filler
    sp::PlaceDB db = makeDb(kSize, cells);

    sp::BinGrid g;
    g.initialize(db, kTarget, 16);
    g.accumulate(db);

    const double density = sum(g.nodeDensity()) + sum(g.fillerDensity());
    const double charge = g.totalCharge(db);
    std::printf("  [consistency] 总密度 %.6f   总电荷 %.6f   相对差 %.3e\n", density, charge,
                std::fabs(density - charge) / std::max(density, 1e-12));
    CHECK_NEAR(charge, density, 1e-3 * std::max(density, 1.0));
}

// ---------------------------------------------------------------- 可复现性
/// 并行累加必须逐位可复现（铁律 7）
void testDeterminism() {
    constexpr float kSize = 256.f;
    std::vector<std::array<float, 5>> cells;
    for (int i = 0; i < 2000; ++i) {
        const float x = 10.f + std::fmod(i * 37.f, kSize - 20.f);
        const float y = 10.f + std::fmod(i * 61.f, kSize - 20.f);
        cells.push_back({x, y, 6.f, 6.f, 0});
    }
    sp::PlaceDB db = makeDb(kSize, cells);

    auto run = [&]() {
        sp::BinGrid g;
        g.initialize(db, 1.0f, 32);
        g.accumulate(db);
        g.solveField();
        std::vector<float> force(2 * static_cast<size_t>(db.totalNodes()), 0.f);
        g.gatherForce(db, force.data());
        return std::make_pair(g.nodeDensity(), force);
    };
    const auto a = run();
    const auto b = run();
    CHECK_TRUE(a.first == b.first);
    CHECK_TRUE(a.second == b.second);
}

/// 网格维度自动推算应取 2 的幂
void testBinDimPowerOfTwo() {
    std::vector<std::array<float, 5>> cells;
    for (int i = 0; i < 500; ++i) cells.push_back({10.f + (i % 20) * 12.f, 10.f + (i / 20) * 12.f,
                                                   8.f, 8.f, 0});
    sp::PlaceDB db = makeDb(256.f, cells);
    sp::BinGrid g;
    g.initialize(db, 1.0f);
    const int d = g.dim();
    std::printf("  [bindim]   自动推算得 %d\n", d);
    CHECK_TRUE(d >= 4);
    CHECK_TRUE((d & (d - 1)) == 0);   // 2 的幂
}

}  // namespace

int main() {
    testScalingRule();
    testFillerSeparate();
    testLocalSmoothingConservesArea();
    testOverflowZeroWhenBalanced();
    testOverflowHighWhenCrowded();
    testOverflowExcludesFiller();
    testChargeMatchesDensity();
    testDeterminism();
    testBinDimPowerOfTwo();
    return sptest::summary("test_density");
}
