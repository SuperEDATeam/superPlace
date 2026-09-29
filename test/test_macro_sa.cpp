// 宏单元模拟退火合法化验证（05 §5.6.2）。
//
// MMS adaptec1 有 21 万单元，跑一次要十几秒，不适合放进 ctest。这里用几十个节点的
// 手写夹具覆盖同样的代码路径：人造重叠能否被彻底消除、线长增量是否受控、
// 合法化后宏是否被钉死、同种子两次运行是否逐位一致。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "lg/macro_sa.h"
#include "test_util.h"
#include "util/config.h"
#include "util/metrics.h"

namespace {

struct NodeSpec {
    float   cx, cy, w, h;
    uint8_t flags;
};

/// 造一个 core = [0,size]² 的库。节点按 [可移动 | 固定] 分段（本测试不用 filler）。
/// nets 里每个元素是一组节点下标（指 specs 中的下标），构成一条 net，引脚偏移取 0。
sp::PlaceDB makeDb(float size, float rowHeight, const std::vector<NodeSpec>& specs,
                   const std::vector<std::vector<int>>& nets) {
    sp::PlaceDB db;
    std::vector<int> remap(specs.size(), -1);
    int movable = 0, fixed = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < specs.size(); ++i) {
            const bool isFixed = (specs[i].flags & sp::F_FIXED) != 0;
            if ((pass == 1) != isFixed) continue;
            remap[i] = db.addNode("n" + std::to_string(i), specs[i].w, specs[i].h, specs[i].flags);
            db.node_x.back() = specs[i].cx;
            db.node_y.back() = specs[i].cy;
            if (pass == 0) ++movable; else ++fixed;
        }
    }
    db.numMovable = movable;
    db.numNodes = movable + fixed;
    db.numFillers = 0;

    std::vector<int> pinNet;
    for (size_t n = 0; n < nets.size(); ++n) {
        for (int idx : nets[n]) {
            db.pin2node.push_back(remap[static_cast<size_t>(idx)]);
            db.pin_offset_x.push_back(0.f);
            db.pin_offset_y.push_back(0.f);
            pinNet.push_back(static_cast<int>(n));
        }
    }
    db.numPins = static_cast<int>(db.pin2node.size());
    db.numNets = static_cast<int>(nets.size());
    db.net_weight.assign(static_cast<size_t>(db.numNets), 1.f);
    db.net_name.assign(static_cast<size_t>(db.numNets), "net");
    db.finalizeCSR(pinNet);

    const int rows = static_cast<int>(size / rowHeight);
    for (int rIdx = 0; rIdx < rows; ++rIdx)
        db.rows.push_back(sp::PlaceDB::SiteRow{static_cast<float>(rIdx) * rowHeight, rowHeight,
                                               0.f, 1.f, static_cast<int>(size)});
    db.computeRegions();
    return db;
}

/// 直接按定义两两求交，作为 macro_sa 内部增量模型的独立对照。
double pairwiseMacroOverlap(const sp::PlaceDB& db) {
    std::vector<int> m;
    for (int i = 0; i < db.numNodes; ++i)
        if (db.isMacro(i)) m.push_back(i);
    double sum = 0.0;
    for (size_t a = 0; a < m.size(); ++a)
        for (size_t b = a + 1; b < m.size(); ++b)
            sum += sp::overlapArea(db.box(m[a]), db.box(m[b]));
    return sum;
}

sp::Config makeCfg() {
    sp::Config cfg;
    cfg.random_seed = 1002;
    cfg.macro_sa_moves = 30000;
    return cfg;
}

// --------------------------------------------------------------- 零重叠是硬约束
void testRemovesArtificialOverlap() {
    // 16 个宏全部堆在同一点上：最恶劣的初值，重叠面积接近 C(16,2) 个整宏面积
    std::vector<NodeSpec> specs;
    for (int i = 0; i < 16; ++i)
        specs.push_back({500.f, 500.f, 90.f, 90.f, sp::F_MACRO});
    // 四个角上放固定阻挡，顺带覆盖"宏不得压在固定块上"这条
    for (int i = 0; i < 4; ++i)
        specs.push_back({(i % 2) ? 950.f : 50.f, (i / 2) ? 950.f : 50.f, 80.f, 80.f, sp::F_FIXED});

    std::vector<std::vector<int>> nets;
    for (int i = 0; i + 1 < 16; ++i) nets.push_back({i, i + 1});

    sp::PlaceDB db = makeDb(1000.f, 10.f, specs, nets);
    // F_MACRO 由解析器按高度判定后写入；手工造库绕过了解析器，须显式给出
    int macroCount = 0;
    for (int i = 0; i < db.numMovable; ++i)
        if (db.isMacro(i)) ++macroCount;
    CHECK_EQ(macroCount, 16);

    const double before = pairwiseMacroOverlap(db);
    CHECK_TRUE(before > 0.0);

    sp::MetricsSink sink;
    sp::Config cfg = makeCfg();
    const sp::MacroSaResult r = sp::legalizeMacros(db, cfg, sink);

    const double after = pairwiseMacroOverlap(db);
    std::printf("  [overlap]  %d 个宏，重叠 %.0f -> %.0f（独立对照）\n", r.numMacros, before, after);
    CHECK_EQ(r.numMacros, 16);
    CHECK_NEAR(after, 0.0, 1e-6);
    // 内部增量模型算出的结果必须与独立的两两求交一致（含固定阻挡那部分会更大，
    // 所以只要求内部值不小于纯宏-宏的那部分）
    CHECK_TRUE(r.overlapAfter >= after - 1e-6);
    CHECK_NEAR(r.overlapAfter, 0.0, 1e-6);
}

// ------------------------------------------------------------------ 越界与对齐
void testStaysInCoreAndAligned() {
    std::vector<NodeSpec> specs;
    for (int i = 0; i < 12; ++i)
        specs.push_back({100.f + i * 5.f, 100.f + i * 5.f, 120.f, 120.f, sp::F_MACRO});
    std::vector<std::vector<int>> nets;
    for (int i = 0; i + 1 < 12; ++i) nets.push_back({i, i + 1});

    sp::PlaceDB db = makeDb(1000.f, 10.f, specs, nets);
    sp::MetricsSink sink;
    sp::Config cfg = makeCfg();
    sp::legalizeMacros(db, cfg, sink);

    int outside = 0, misaligned = 0;
    for (int i = 0; i < db.numNodes; ++i) {
        if (!db.isMacro(i)) continue;
        if (db.llx(i) < db.coreRegion.lx - 1e-3f || db.urx(i) > db.coreRegion.hx + 1e-3f ||
            db.lly(i) < db.coreRegion.ly - 1e-3f || db.ury(i) > db.coreRegion.hy + 1e-3f)
            ++outside;
        // 行对齐：左下角 y 必须落在行栅格上
        const float dy = std::fabs(std::remainder(db.lly(i) - db.coreRegion.ly, db.rowHeight));
        if (dy > 1e-3f) ++misaligned;
    }
    std::printf("  [legal]    越界 %d 个，未对齐到行 %d 个\n", outside, misaligned);
    CHECK_EQ(outside, 0);
    CHECK_EQ(misaligned, 0);
}

// -------------------------------------------------------------- 合法化后宏被钉死
void testMacrosBecomeFixed() {
    std::vector<NodeSpec> specs;
    for (int i = 0; i < 8; ++i) specs.push_back({300.f, 300.f, 100.f, 100.f, sp::F_MACRO});
    sp::PlaceDB db = makeDb(1000.f, 10.f, specs, {{0, 1}, {2, 3}, {4, 5}, {6, 7}});

    int fixedBefore = 0;
    for (int i = 0; i < db.numNodes; ++i)
        if (db.isFixed(i)) ++fixedBefore;

    sp::MetricsSink sink;
    sp::Config cfg = makeCfg();
    sp::legalizeMacros(db, cfg, sink);

    int fixedAfter = 0, stillMacro = 0;
    for (int i = 0; i < db.numNodes; ++i) {
        if (db.isFixed(i)) ++fixedAfter;
        if (db.isMacro(i)) ++stillMacro;
    }
    std::printf("  [fixed]    固定节点 %d -> %d，宏标志仍保留 %d 个\n", fixedBefore, fixedAfter,
                stillMacro);
    CHECK_EQ(fixedBefore, 0);
    CHECK_EQ(fixedAfter, 8);
    CHECK_EQ(stillMacro, 8);

    // 再跑一次：宏已被钉死，应当被识别为"无可移动宏"而直接返回
    const sp::MacroSaResult r2 = sp::legalizeMacros(db, cfg, sink);
    CHECK_EQ(r2.numMacros, 0);
}

// ------------------------------------------------------------------ 线长受控
void testWirelengthControlled() {
    // 已经无重叠的规整布局：SA 没有合法性压力，此时它不该把线长搞坏
    std::vector<NodeSpec> specs;
    for (int i = 0; i < 9; ++i)
        specs.push_back({150.f + (i % 3) * 300.f, 150.f + (i / 3) * 300.f, 100.f, 100.f, sp::F_MACRO});
    std::vector<std::vector<int>> nets;
    for (int i = 0; i + 1 < 9; ++i) nets.push_back({i, i + 1});

    sp::PlaceDB db = makeDb(1000.f, 10.f, specs, nets);
    CHECK_NEAR(pairwiseMacroOverlap(db), 0.0, 1e-6);

    sp::MetricsSink sink;
    sp::Config cfg = makeCfg();
    const sp::MacroSaResult r = sp::legalizeMacros(db, cfg, sink);

    const double ratio = r.hpwlAfter / r.hpwlBefore;
    std::printf("  [wl]       无重叠初值下 HPWL %.1f -> %.1f（比值 %.4f）\n", r.hpwlBefore,
                r.hpwlAfter, ratio);
    CHECK_NEAR(pairwiseMacroOverlap(db), 0.0, 1e-6);
    // 不要求严格不变（SA 有随机性且代价里还有重叠项），但不能明显劣化
    CHECK_TRUE(ratio < 1.05);
}

// ------------------------------------------------ 增量线长模型与全量计算一致
void testIncrementalMatchesFullHpwl() {
    // macro_sa 内部用"静态盒 + 局部盒"做增量；这里检查它报出的 hpwlAfter
    // 与用 computeHPWL 全量重算的结果一致——两条独立路径必须重合。
    std::vector<NodeSpec> specs;
    for (int i = 0; i < 10; ++i) specs.push_back({400.f + i * 8.f, 400.f, 80.f, 80.f, sp::F_MACRO});
    for (int i = 0; i < 20; ++i)   // 一批冻结的标准单元，构成静态盒
        specs.push_back({50.f + i * 45.f, 900.f, 4.f, 10.f, 0});
    std::vector<std::vector<int>> nets;
    for (int i = 0; i < 10; ++i) nets.push_back({i, 10 + i, 10 + ((i + 5) % 20)});

    sp::PlaceDB db = makeDb(1000.f, 10.f, specs, nets);
    sp::MetricsSink sink;
    sp::Config cfg = makeCfg();
    const sp::MacroSaResult r = sp::legalizeMacros(db, cfg, sink);

    const double full = sp::computeHPWL(db);
    std::printf("  [incr]     增量模型报 %.4f，全量重算 %.4f，相对差 %.2e\n", r.hpwlAfter, full,
                std::fabs(r.hpwlAfter - full) / std::max(1.0, full));
    CHECK_NEAR(r.hpwlAfter, full, 1e-6 * std::max(1.0, full));
}

// --------------------------------------------------------------------- 确定性
void testDeterminism() {
    auto run = [] {
        std::vector<NodeSpec> specs;
        for (int i = 0; i < 14; ++i) specs.push_back({500.f, 500.f, 100.f, 100.f, sp::F_MACRO});
        std::vector<std::vector<int>> nets;
        for (int i = 0; i + 1 < 14; ++i) nets.push_back({i, i + 1});
        sp::PlaceDB db = makeDb(1000.f, 10.f, specs, nets);
        sp::MetricsSink sink;
        sp::Config cfg = makeCfg();
        sp::legalizeMacros(db, cfg, sink);
        std::vector<float> out;
        for (int i = 0; i < db.numNodes; ++i) {
            out.push_back(db.node_x[static_cast<size_t>(i)]);
            out.push_back(db.node_y[static_cast<size_t>(i)]);
        }
        return out;
    };
    const std::vector<float> a = run(), b = run();
    int diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) ++diff;
    std::printf("  [determ]   同种子两次运行，%zu 个坐标中不一致 %d 个\n", a.size(), diff);
    CHECK_EQ(diff, 0);
}

}  // namespace

int main() {
    testRemovesArtificialOverlap();
    testStaysInCoreAndAligned();
    testMacrosBecomeFixed();
    testWirelengthControlled();
    testIncrementalMatchesFullHpwl();
    testDeterminism();
    return sptest::summary("test_macro_sa");
}
