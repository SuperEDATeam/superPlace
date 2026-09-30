// mGP 主循环回归测试（05 §5.5）。
//
// 此前的单测只覆盖到【组件】——泊松解析对拍、WA 梯度数值对拍、密度缩放、filler
// 面积配平。主循环本身没有任何自动化保护：把密度项的符号写反、把 λ 或 γ 的调度
// 改坏、把停滞保护删掉，`ctest` 一样全绿，只能靠人工跑 benchmark 才发现。
// 本文件补上这个缺口。
//
// 夹具是几千个单元的合成设计，跑完整的 EPlace 循环只需几秒，可以进 ctest；
// 而 adaptec1 跑一次要 14 秒、还得先跑 14 秒 QP，不适合。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "gp/bin_grid.h"
#include "gp/eplace.h"
#include "gp/filler.h"
#include "test_util.h"
#include "util/config.h"
#include "util/metrics.h"
#include "util/rng.h"
#include "util/timer.h"

namespace {

constexpr float kCoreSize  = 1200.f;
constexpr float kRowHeight = 12.f;

struct Fixture {
    sp::PlaceDB db;
    sp::Config  cfg;
};

/// 合成设计：numCells 个标准单元 + numMacros 个宏，全部堆在 core 中心，
/// 再按确定性规则连成网表。"全部堆在中心"是刻意的——它模拟 QP 的输出，
/// 也让"单元是否被density force 推开"成为一个可判定的现象。
Fixture makeFixture(int numCells, int numMacros, unsigned seed = 7) {
    Fixture f;
    sp::PlaceDB& db = f.db;

    const float cx = kCoreSize * 0.5f, cy = kCoreSize * 0.5f;
    sp::Rng rng(seed);

    for (int i = 0; i < numCells; ++i) {
        db.addNode("c" + std::to_string(i), 8.f, kRowHeight, 0);
        // 初值挤在中心一小团里，但不是同一点——完全重合会让初始梯度处处相同
        db.node_x.back() = cx + rng.uniform(-20.f, 20.f);
        db.node_y.back() = cy + rng.uniform(-20.f, 20.f);
    }
    for (int i = 0; i < numMacros; ++i) {
        db.addNode("m" + std::to_string(i), 100.f, 9 * kRowHeight, sp::F_MACRO);
        db.node_x.back() = cx + rng.uniform(-20.f, 20.f);
        db.node_y.back() = cy + rng.uniform(-20.f, 20.f);
    }
    const int numMovable = numCells + numMacros;

    // 四角各一个固定终端，制造非平凡的 terminalDensity
    for (int i = 0; i < 4; ++i) {
        db.addNode("t" + std::to_string(i), 60.f, 60.f, sp::F_FIXED);
        db.node_x.back() = (i % 2) ? kCoreSize - 40.f : 40.f;
        db.node_y.back() = (i / 2) ? kCoreSize - 40.f : 40.f;
    }

    db.numMovable = numMovable;
    db.numNodes = numMovable + 4;
    db.numFillers = 0;

    // 网表：每条 net 连 3 个节点，按固定步长挑选，保证同 seed 完全可复现
    std::vector<int> pinNet;
    int netCount = 0;
    for (int i = 0; i < numMovable; ++i) {
        const int a = i;
        const int b = (i * 7 + 3) % numMovable;
        const int c = (i * 13 + 11) % db.numNodes;   // 可能落到固定终端上，正是想要的
        if (a == b || b == c || a == c) continue;
        for (int nd : {a, b, c}) {
            db.pin2node.push_back(nd);
            // 引脚放在节点边缘，让引脚偏移不全为 0
            db.pin_offset_x.push_back(0.25f * db.node_w[static_cast<size_t>(nd)]);
            db.pin_offset_y.push_back(-0.25f * db.node_h[static_cast<size_t>(nd)]);
            pinNet.push_back(netCount);
        }
        ++netCount;
    }
    db.numPins = static_cast<int>(db.pin2node.size());
    db.numNets = netCount;
    db.net_weight.assign(static_cast<size_t>(netCount), 1.f);
    db.net_name.assign(static_cast<size_t>(netCount), "n");
    db.finalizeCSR(pinNet);

    const int rows = static_cast<int>(kCoreSize / kRowHeight);
    for (int r = 0; r < rows; ++r)
        db.rows.push_back(sp::PlaceDB::SiteRow{static_cast<float>(r) * kRowHeight, kRowHeight, 0.f,
                                               1.f, static_cast<int>(kCoreSize)});
    db.computeRegions();

    f.cfg.random_seed = 1002;
    f.cfg.target_density = 1.0f;
    f.cfg.target_overflow = 0.10f;
    f.cfg.cgp_target_overflow = 0.07f;
    f.cfg.gp_max_iter = 600;
    f.cfg.gp_stagnation_window = 200;
    f.cfg.ignore_net_degree = 100;
    return f;
}

/// 造一个【装不下】的设计：可移动总面积 ≈ 1.4 倍 core 面积。
///
/// 这样 τ 就有一个可以手算的正下界，与优化器好坏无关——按面积守恒，
/// 即使铺得完美每个 bin 也有 ρ=1.4，于是
///     τ_floor = (1.4−1)·coreArea / (cellArea + macroArea·ρt) = 0.4/1.4 ≈ 0.286
/// 远高于 target_overflow=0.10。这才是真正的停滞：不是"优化器不够努力"，
/// 而是"目标在物理上不可达"。前一版测试只是把目标设成 1e-4，
/// 但那个夹具足够稀疏，τ 会一路降到 0.0026，压根没停滞过。
Fixture makeOverfullFixture() {
    Fixture f = makeFixture(3000, 0);
    sp::PlaceDB& db = f.db;

    // 在可移动段的末尾插入大宏。PlaceDB 按 [可移动|固定|filler] 分段，
    // 直接 push 会落到固定段后面，所以这里重建一次节点数组。
    const int extraMacros = 40;
    const float mw = 208.f, mh = 208.f;
    sp::PlaceDB db2;
    for (int i = 0; i < db.numMovable; ++i)
        db2.addNode(db.node_name[static_cast<size_t>(i)], db.node_w[static_cast<size_t>(i)],
                    db.node_h[static_cast<size_t>(i)], db.node_flags[static_cast<size_t>(i)]);
    for (int i = 0; i < db.numMovable; ++i) {
        db2.node_x[static_cast<size_t>(i)] = db.node_x[static_cast<size_t>(i)];
        db2.node_y[static_cast<size_t>(i)] = db.node_y[static_cast<size_t>(i)];
    }
    sp::Rng rng(11);
    for (int i = 0; i < extraMacros; ++i) {
        db2.addNode("M" + std::to_string(i), mw, mh, sp::F_MACRO);
        db2.node_x.back() = kCoreSize * 0.5f + rng.uniform(-20.f, 20.f);
        db2.node_y.back() = kCoreSize * 0.5f + rng.uniform(-20.f, 20.f);
    }
    const int numMovable = db.numMovable + extraMacros;
    for (int i = db.numMovable; i < db.numNodes; ++i) {
        db2.addNode(db.node_name[static_cast<size_t>(i)], db.node_w[static_cast<size_t>(i)],
                    db.node_h[static_cast<size_t>(i)], db.node_flags[static_cast<size_t>(i)]);
        db2.node_x.back() = db.node_x[static_cast<size_t>(i)];
        db2.node_y.back() = db.node_y[static_cast<size_t>(i)];
    }
    db2.numMovable = numMovable;
    db2.numNodes = numMovable + (db.numNodes - db.numMovable);
    db2.numFillers = 0;

    // 网表沿用同一套生成规则，把新宏也串进去
    std::vector<int> pinNet;
    int nc = 0;
    for (int i = 0; i < numMovable; ++i) {
        const int a = i, b = (i * 7 + 3) % numMovable, c = (i * 13 + 11) % db2.numNodes;
        if (a == b || b == c || a == c) continue;
        for (int nd : {a, b, c}) {
            db2.pin2node.push_back(nd);
            db2.pin_offset_x.push_back(0.25f * db2.node_w[static_cast<size_t>(nd)]);
            db2.pin_offset_y.push_back(-0.25f * db2.node_h[static_cast<size_t>(nd)]);
            pinNet.push_back(nc);
        }
        ++nc;
    }
    db2.numPins = static_cast<int>(db2.pin2node.size());
    db2.numNets = nc;
    db2.net_weight.assign(static_cast<size_t>(nc), 1.f);
    db2.net_name.assign(static_cast<size_t>(nc), "n");
    db2.finalizeCSR(pinNet);
    db2.rows = db.rows;
    db2.computeRegions();

    f.db = std::move(db2);
    return f;
}

int countOutOfCore(const sp::PlaceDB& db) {
    int n = 0;
    for (int i = 0; i < db.totalNodes(); ++i) {
        if (db.isFixed(i) || db.isNI(i)) continue;
        if (db.llx(i) < db.coreRegion.lx - 1e-2f || db.urx(i) > db.coreRegion.hx + 1e-2f ||
            db.lly(i) < db.coreRegion.ly - 1e-2f || db.ury(i) > db.coreRegion.hy + 1e-2f)
            ++n;
    }
    return n;
}

float measureOverflow(const sp::PlaceDB& db, const sp::Config& cfg) {
    sp::BinGrid g;
    g.initialize(db, cfg.target_density, cfg.gp_bin_dim);
    g.accumulate(db);
    return g.overflow();
}

// ==========================================================================
// 1. 最关键的一条：密度项符号
//
// 总梯度里密度项前是减号（∇D = −qξ）。写成加号的现象是单元向高密度区聚集，
// 即"越跑越挤"，而这在指标上极易被误判成"λ 给小了"。夹具把所有单元堆在中心，
// 于是"τ 是否显著下降"就成了这个符号的判定式。
// ==========================================================================
void testCellsSpreadOut() {
    Fixture f = makeFixture(3000, 12);
    sp::MetricsSink sink;

    const float tauBefore = measureOverflow(f.db, f.cfg);
    sp::initializeFillers(f.db, f.cfg);

    sp::EPlace engine(f.db, f.cfg, sink);
    const sp::GpResult r = engine.run(sp::GpStage::kMGP);

    std::printf("  [sign]     τ %.4f -> %.4f（%d 轮，%s）\n", tauBefore, r.overflow, r.iterations,
                r.converged ? "达标" : "未达标");
    CHECK_TRUE(tauBefore > 0.5f);          // 初值确实是挤成一团的
    CHECK_TRUE(r.overflow < tauBefore);    // 符号写反的话这一条必然失败
    CHECK_TRUE(r.converged);
    CHECK_TRUE(r.overflow < f.cfg.target_overflow);
}

// ==========================================================================
// 2. 数值健全性：无 NaN/Inf、不出界
// ==========================================================================
void testNoNaNAndInCore() {
    Fixture f = makeFixture(2000, 8);
    sp::MetricsSink sink;
    sp::initializeFillers(f.db, f.cfg);

    sp::EPlace engine(f.db, f.cfg, sink);
    const sp::GpResult r = engine.run(sp::GpStage::kMGP);

    int bad = 0;
    for (int i = 0; i < f.db.totalNodes(); ++i)
        if (!std::isfinite(f.db.node_x[static_cast<size_t>(i)]) ||
            !std::isfinite(f.db.node_y[static_cast<size_t>(i)]))
            ++bad;
    const int outside = countOutOfCore(f.db);

    std::printf("  [sane]     非有限坐标 %d 个，越界 %d 个，HPWL=%.6g\n", bad, outside, r.hpwl);
    CHECK_EQ(bad, 0);
    CHECK_EQ(outside, 0);
    CHECK_TRUE(std::isfinite(r.hpwl));
    CHECK_TRUE(std::isfinite(r.overflow));
}

// ==========================================================================
// 3. 初态已达标时必须一步不走
//
// 停止判据若只写在 step() 之后，已收敛的输入也会先迈一步；Nesterov 的首步步长
// 按扰动法估计、量级很大，足以把好布局推坏（thin1 实测 HPWL 29 -> 643）。
// ==========================================================================
void testAlreadyConvergedDoesNotMove() {
    // 单元数很少 -> 密度远低于目标 -> τ 初值就是 0
    Fixture f = makeFixture(200, 0);
    // 摊开放置，避免初值堆叠
    for (int i = 0; i < f.db.numMovable; ++i) {
        f.db.node_x[static_cast<size_t>(i)] = 100.f + static_cast<float>(i % 40) * 25.f;
        f.db.node_y[static_cast<size_t>(i)] = 100.f + static_cast<float>(i / 40) * 100.f;
    }
    sp::MetricsSink sink;

    const double hpwlBefore = sp::computeHPWL(f.db);
    const float tauBefore = measureOverflow(f.db, f.cfg);
    sp::EPlace engine(f.db, f.cfg, sink);
    const sp::GpResult r = engine.run(sp::GpStage::kMGP);
    const double hpwlAfter = sp::computeHPWL(f.db);

    std::printf("  [noop]     τ0=%.4f 已达标；轮数=%d，HPWL %.4f -> %.4f（比值 %.6f）\n", tauBefore,
                r.iterations, hpwlBefore, hpwlAfter, hpwlAfter / hpwlBefore);
    CHECK_TRUE(tauBefore < f.cfg.target_overflow);
    CHECK_EQ(r.iterations, 0);
    CHECK_TRUE(r.converged);
    // 只允许边界夹取带来的 epsilon 级变化
    CHECK_TRUE(hpwlAfter <= hpwlBefore * 1.001 + 1.0);
}

// ==========================================================================
// 4. 停滞保护
//
// λ 调度是 ΔHPWL 的速率控制器，本身没有"τ 不动就收手"这一项。把目标 τ 设成
// 一个达不到的值，正常情况下会空转到迭代上限、HPWL 单调膨胀；
// 有停滞保护则应提前停止，并回退到 τ 最优时的解。
// ==========================================================================
void testStagnationGuard() {
    constexpr int kMaxIter = 500;
    auto run = [](int window) {
        Fixture f = makeOverfullFixture();
        f.cfg.gp_max_iter = kMaxIter;
        f.cfg.gp_stagnation_window = window;
        sp::MetricsSink sink;
        sp::initializeFillers(f.db, f.cfg);   // 面积为负 -> 0 个 filler，顺带覆盖负值保护
        sp::EPlace engine(f.db, f.cfg, sink);
        return engine.run(sp::GpStage::kMGP);
    };

    const sp::GpResult guarded = run(120);
    const sp::GpResult unguarded = run(0);   // 0 = 关闭保护

    std::printf("  [stagnate] τ 的理论下界 ≈ 0.286（面积守恒），目标 0.10 不可达\n");
    std::printf("  [stagnate] 开保护：%d 轮 τ=%.4f HPWL=%.6g\n", guarded.iterations,
                guarded.overflow, guarded.hpwl);
    std::printf("  [stagnate] 关保护：%d 轮 τ=%.4f HPWL=%.6g（多跑 %d 轮，线长多涨 %.1f%%）\n",
                unguarded.iterations, unguarded.overflow, unguarded.hpwl,
                unguarded.iterations - guarded.iterations,
                100.0 * (unguarded.hpwl - guarded.hpwl) / guarded.hpwl);

    CHECK_TRUE(!guarded.converged);                          // 物理上达不到
    CHECK_TRUE(guarded.overflow > 0.2f);                     // 确实卡在下界附近
    CHECK_TRUE(guarded.iterations < unguarded.iterations);   // 确实提前止损了
    CHECK_EQ(unguarded.iterations, kMaxIter);                // 不开保护就跑满
    // 回退到最优解后，τ 不该比空转到底更差
    CHECK_TRUE(guarded.overflow <= unguarded.overflow + 1e-3f);
    // 关键收益：τ 不动时继续跑只会让线长单调膨胀
    CHECK_TRUE(guarded.hpwl < unguarded.hpwl);
}

// ==========================================================================
// 5. 确定性（铁律 7）：同配置两次运行逐位一致
// ==========================================================================
void testDeterminism() {
    auto run = [] {
        Fixture f = makeFixture(2000, 8);
        sp::MetricsSink sink;
        sp::initializeFillers(f.db, f.cfg);
        sp::EPlace engine(f.db, f.cfg, sink);
        engine.run(sp::GpStage::kMGP);
        std::vector<float> out;
        for (int i = 0; i < f.db.totalNodes(); ++i) {
            out.push_back(f.db.node_x[static_cast<size_t>(i)]);
            out.push_back(f.db.node_y[static_cast<size_t>(i)]);
        }
        return out;
    };
    const std::vector<float> a = run(), b = run();
    int diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++diff;
    std::printf("  [determ]   %zu 个坐标中不一致 %d 个\n", a.size(), diff);
    CHECK_EQ(diff, 0);
}

// ==========================================================================
// 6. cGP 期间宏必须逐位不动
// ==========================================================================
void testMacrosFrozenInCgp() {
    Fixture f = makeFixture(2000, 10);
    sp::MetricsSink sink;
    sp::initializeFillers(f.db, f.cfg);
    {
        sp::EPlace engine(f.db, f.cfg, sink);
        engine.run(sp::GpStage::kMGP);
    }
    // 模拟 mLG 的效果：把宏钉死
    std::vector<float> mx, my;
    for (int i = 0; i < f.db.numMovable; ++i) {
        if (!f.db.isMacro(i)) continue;
        f.db.node_flags[static_cast<size_t>(i)] |= sp::F_FIXED;
        mx.push_back(f.db.node_x[static_cast<size_t>(i)]);
        my.push_back(f.db.node_y[static_cast<size_t>(i)]);
    }
    {
        sp::EPlace engine(f.db, f.cfg, sink);
        engine.run(sp::GpStage::kCGP);
    }
    int moved = 0;
    size_t k = 0;
    for (int i = 0; i < f.db.numMovable; ++i) {
        if (!f.db.isMacro(i)) continue;
        if (f.db.node_x[static_cast<size_t>(i)] != mx[k] ||
            f.db.node_y[static_cast<size_t>(i)] != my[k])
            ++moved;
        ++k;
    }
    std::printf("  [cgp]      %zu 个宏中 cGP 期间发生位移的 %d 个\n", mx.size(), moved);
    CHECK_EQ(moved, 0);
}

// ==========================================================================
// 7. 质量带
//
// 钉死精确 HPWL 不可行：密度累加用的 per-thread 局部网格，其个数与节点划分都
// 随线程数变，跨线程数结果有 ~1e-5 的相对差（同线程数下逐位一致，见 5）。
// 因此这里用【区间】守住量级与收敛轮数——足以拦住符号写反、调度改坏、
// 发散这类真实回归，又不会因换台机器就误报。
// ==========================================================================
void testQualityBand() {
    Fixture f = makeFixture(3000, 12);
    sp::MetricsSink sink;
    const double hpwlBefore = sp::computeHPWL(f.db);
    sp::initializeFillers(f.db, f.cfg);
    sp::EPlace engine(f.db, f.cfg, sink);
    const sp::GpResult r = engine.run(sp::GpStage::kMGP);

    const double ratio = r.hpwl / hpwlBefore;
    std::printf("  [band]     %d 轮收敛；HPWL %.6g -> %.6g（涨 %.2fx）τ=%.4f\n", r.iterations,
                hpwlBefore, r.hpwl, ratio, r.overflow);

    // 铺开必然使线长上升，但上升幅度应当有界——失控发散会远超此界
    CHECK_TRUE(ratio > 1.0);
    CHECK_TRUE(ratio < 60.0);
    // 轮数区间：远少于下界说明提前误判收敛，远多于上界说明调度被改坏
    CHECK_TRUE(r.iterations > 30);
    CHECK_TRUE(r.iterations < 500);
}

}  // namespace

int main() {
    const struct { const char* name; void (*fn)(); } cases[] = {
        {"符号(铺开)", testCellsSpreadOut},
        {"数值健全", testNoNaNAndInCore},
        {"已达标不动", testAlreadyConvergedDoesNotMove},
        {"停滞保护", testStagnationGuard},
        {"确定性", testDeterminism},
        {"cGP宏冻结", testMacrosFrozenInCgp},
        {"质量带", testQualityBand},
    };
    for (const auto& c : cases) {
        sp::Timer t;
        c.fn();
        std::printf("             ^ %s 耗时 %.0f ms\n", c.name, t.elapsedMs());
    }
    return sptest::summary("test_eplace");
}
