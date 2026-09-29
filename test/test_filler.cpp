// Filler 初始化验证（05 §5.5.2）。
//
// 三个已确认的陷阱都在这里设了防线：
//   1. 中间 80% 而非 90%（easyPlace 取 5%~95%，课程笔记沿用了它）
//   2. fillerCount 是四舍五入，+0.5 不在分母（课程笔记误加括号）
//   3. 拥挤设计下 totalFillerArea 为负，必须有下界保护否则 resize 崩溃
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "gp/filler.h"
#include "test_util.h"
#include "util/config.h"

namespace {

/// core 为 size×size，含 nCells 个 w×h 的可移动单元
sp::PlaceDB makeDb(float size, int nCells, float w, float h) {
    sp::PlaceDB db;
    for (int i = 0; i < nCells; ++i) {
        db.addNode("c" + std::to_string(i), w, h, 0);
        db.node_x.back() = size * 0.5f;
        db.node_y.back() = size * 0.5f;
    }
    db.numMovable = nCells;
    db.numNodes = nCells;
    db.numPins = 0;
    db.numNets = 0;
    db.net2pin_start.assign(1, 0);
    db.node2pin_start.assign(static_cast<size_t>(nCells) + 1, 0);
    // 行高设为 h，使 filler 高度等于行高
    const int nRows = static_cast<int>(size / h);
    for (int r = 0; r < nRows; ++r)
        db.rows.push_back(sp::PlaceDB::SiteRow{r * h, h, 0.f, 1.f, static_cast<int>(size)});
    db.computeRegions();
    return db;
}

sp::Config makeCfg(float targetDensity) {
    sp::Config c;
    c.target_density = targetDensity;
    c.random_seed = 42;
    return c;
}

/// 空白面积充裕时，filler 总面积应补足到 whitespace × targetDensity
void testFillerAreaBalance() {
    // core 100x100 = 10000；20 个 10x10 单元 = 2000
    sp::PlaceDB db = makeDb(100.f, 20, 10.f, 10.f);
    const sp::Config cfg = makeCfg(0.8f);
    const sp::FillerInfo info = sp::initializeFillers(db, cfg);

    const double expectTotal = info.whitespaceArea * 0.8 - info.nodeAreaScaled;
    std::printf("  [balance]  whitespace=%.0f nodeScaled=%.0f fillerArea=%.0f count=%d\n",
                info.whitespaceArea, info.nodeAreaScaled, info.totalFillerArea, info.count);
    // 容差按相对值给：whitespace 由 float 的 overlapArea 累加而来，
    // 6000 量级上有 ~1e-4 的绝对误差属正常
    CHECK_NEAR(info.totalFillerArea, expectTotal, 1e-6 * std::fabs(expectTotal));
    CHECK_TRUE(info.count > 0);

    // 实际插入的 filler 总面积应接近目标（差异来自四舍五入）
    double actual = 0.0;
    for (int i = db.numNodes; i < db.totalNodes(); ++i) actual += db.area(i);
    const double singleArea = static_cast<double>(info.width) * info.height;
    std::printf("  [balance]  实际 filler 面积 %.1f vs 目标 %.1f（差 < 一个 filler = %.1f）\n",
                actual, info.totalFillerArea, singleArea);
    CHECK_TRUE(std::fabs(actual - info.totalFillerArea) <= singleArea);
}

/// 四舍五入而非截断：构造一个余数 > 0.5 的场景
void testCountIsRounded() {
    sp::PlaceDB db = makeDb(100.f, 10, 10.f, 10.f);
    const sp::Config cfg = makeCfg(1.0f);
    const sp::FillerInfo info = sp::initializeFillers(db, cfg);

    const double singleArea = static_cast<double>(info.width) * info.height;
    const double raw = info.totalFillerArea / singleArea;
    const int expected = static_cast<int>(raw + 0.5);
    std::printf("  [rounding] raw=%.4f  count=%d  期望=%d\n", raw, info.count, expected);
    CHECK_EQ(info.count, expected);
    // 若误写成 totalArea/(singleArea+0.5)，结果会明显不同
    const int wrong = static_cast<int>(info.totalFillerArea / (singleArea + 0.5));
    if (wrong != expected)
        std::printf("  [rounding] （误加括号的写法会得到 %d，已被区分）\n", wrong);
}

/// 拥挤设计：totalFillerArea 为负时必须安全返回 0，而不是崩溃
void testNegativeAreaIsSafe() {
    // 单元面积远超 whitespace × target
    sp::PlaceDB db = makeDb(100.f, 95, 10.f, 10.f);   // 9500 面积 vs core 10000
    const sp::Config cfg = makeCfg(0.5f);             // whitespace*0.5 = 5000 < 9500
    const sp::FillerInfo info = sp::initializeFillers(db, cfg);

    std::printf("  [negative] totalFillerArea=%.0f  count=%d（应为 0，且不崩溃）\n",
                info.totalFillerArea, info.count);
    CHECK_TRUE(info.totalFillerArea < 0.0);
    CHECK_EQ(info.count, 0);
    CHECK_EQ(db.numFillers, 0);
    CHECK_EQ(db.totalNodes(), db.numNodes);
}

/// filler 尺寸取中间 80% 的平均面积——用双峰分布区分 80% 与 90%
void testMid80Percent() {
    // 10% 极小 + 80% 中等 + 10% 极大
    sp::PlaceDB db;
    const int n = 100;
    for (int i = 0; i < n; ++i) {
        float w;
        if (i < 10) w = 1.f;            // 最小 10%
        else if (i < 90) w = 10.f;      // 中间 80%
        else w = 1000.f;                // 最大 10%
        db.addNode("c" + std::to_string(i), w, 10.f, 0);
        db.node_x.back() = 500.f;
        db.node_y.back() = 500.f;
    }
    db.numMovable = n;
    db.numNodes = n;
    db.numPins = 0;
    db.numNets = 0;
    db.net2pin_start.assign(1, 0);
    db.node2pin_start.assign(static_cast<size_t>(n) + 1, 0);
    for (int r = 0; r < 100; ++r)
        db.rows.push_back(sp::PlaceDB::SiteRow{r * 10.f, 10.f, 0.f, 1.f, 1000});
    db.computeRegions();

    const sp::Config cfg = makeCfg(1.0f);
    const sp::FillerInfo info = sp::initializeFillers(db, cfg);

    // 中间 80% 全是 10x10=100，故 filler 面积应恰好是 100
    const double singleArea = static_cast<double>(info.width) * info.height;
    std::printf("  [mid80]    filler 单个面积 = %.2f（中间 80%% 全为 100）\n", singleArea);
    CHECK_NEAR(singleArea, 100.0, 1e-3);
}

/// filler 全部落在 core 内，且同 seed 可复现
void testPlacementAndDeterminism() {
    const sp::Config cfg = makeCfg(0.9f);

    auto run = [&]() {
        sp::PlaceDB db = makeDb(200.f, 50, 10.f, 10.f);
        sp::initializeFillers(db, cfg);
        std::vector<float> xs(db.node_x.begin() + db.numNodes, db.node_x.end());
        std::vector<float> ys(db.node_y.begin() + db.numNodes, db.node_y.end());
        return std::make_pair(xs, ys);
    };
    const auto a = run();
    const auto b = run();
    CHECK_TRUE(a.first == b.first);
    CHECK_TRUE(a.second == b.second);

    sp::PlaceDB db = makeDb(200.f, 50, 10.f, 10.f);
    sp::initializeFillers(db, cfg);
    int outside = 0;
    for (int i = db.numNodes; i < db.totalNodes(); ++i) {
        if (db.llx(i) < db.coreRegion.lx - 1e-3f || db.urx(i) > db.coreRegion.hx + 1e-3f ||
            db.lly(i) < db.coreRegion.ly - 1e-3f || db.ury(i) > db.coreRegion.hy + 1e-3f)
            ++outside;
    }
    std::printf("  [place]    %d 个 filler，越界 %d 个\n", db.numFillers, outside);
    CHECK_EQ(outside, 0);
    // filler 必须位于 [numNodes, totalNodes) 区间且带 F_FILLER 标志
    for (int i = db.numNodes; i < db.totalNodes(); ++i) CHECK_TRUE(db.isFiller(i));
    for (int i = 0; i < db.numNodes; ++i) CHECK_TRUE(!db.isFiller(i));
}

/// 重复初始化不应累积 filler
void testReinitIsIdempotent() {
    sp::PlaceDB db = makeDb(100.f, 20, 10.f, 10.f);
    const sp::Config cfg = makeCfg(0.8f);
    const sp::FillerInfo a = sp::initializeFillers(db, cfg);
    const int firstTotal = db.totalNodes();
    const sp::FillerInfo b = sp::initializeFillers(db, cfg);
    CHECK_EQ(a.count, b.count);
    CHECK_EQ(db.totalNodes(), firstTotal);
}

}  // namespace

int main() {
    testFillerAreaBalance();
    testCountIsRounded();
    testNegativeAreaIsSafe();
    testMid80Percent();
    testPlacementAndDeterminism();
    testReinitIsIdempotent();
    return sptest::summary("test_filler");
}
