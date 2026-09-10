// adaptec1 解析结果逐项对拍。
// 基准来源：work_report/05-开发执行规范.md §5.1.4（源自课程资料的 DATABASE SUMMARY）。
// 任何一个数字对不上即判定解析器错误。
#include <cstdio>
#include <string>

#include "db/db_stats.h"
#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "test_util.h"

int main(int argc, char** argv) {
    const std::string aux =
        (argc > 1) ? argv[1] : std::string(SP_TEST_DATA_DIR) + "/adaptec1/adaptec1.aux";

    sp::PlaceDB db;
    sp::BookshelfReader reader;
    reader.read(aux, db);

    const sp::DbStats s = sp::DbStats::compute(db);
    std::fputs(s.toString().c_str(), stdout);

    // ------------------------------------------------------------------ 区域
    CHECK_NEAR(s.coreLx, 459.0, 1e-6);
    CHECK_NEAR(s.coreLy, 459.0, 1e-6);
    CHECK_NEAR(s.coreHx, 11151.0, 1e-6);
    CHECK_NEAR(s.coreHy, 11139.0, 1e-6);
    CHECK_NEAR(s.rowHeight, 12.0, 1e-6);
    CHECK_EQ(s.rowCount, 890);
    CHECK_NEAR(s.siteStep, 1.0, 1e-6);

    // ------------------------------------------------------------------ 面积
    CHECK_NEAR(s.coreArea, 114190560.0, 1.0);
    CHECK_NEAR(s.movableArea, 37286292.0, 1.0);
    CHECK_NEAR(s.fixedArea, 64093992.0, 1.0);
    CHECK_NEAR(s.fixedAreaInCore, 49164072.0, 1.0);

    // 百分比（保留两位后比对，与资料一致）
    CHECK_NEAR(s.cellAreaRatio() * 100.0, 32.65, 0.005);
    CHECK_NEAR(s.fixedAreaRatio() * 100.0, 56.13, 0.005);
    CHECK_NEAR(s.fixedInCoreRatio() * 100.0, 43.05, 0.005);
    CHECK_NEAR(s.placementUtil() * 100.0, 57.34, 0.005);
    CHECK_NEAR(s.coreDensity() * 100.0, 75.71, 0.005);

    // ------------------------------------------------------------------ 计数
    CHECK_EQ(s.nodeCount, 210904);
    CHECK_EQ(s.objectCount, 211447);
    CHECK_EQ(s.fixedCount, 543);
    CHECK_EQ(s.macroCount, 0);
    CHECK_EQ(s.netCount, 221142);
    CHECK_EQ(s.pinCount, 944053);
    CHECK_EQ(s.maxNetDegree, 2271);

    // ------------------------------------------------------ net degree 分布
    // 参考口径（课程资料原文标签）
    const auto ref = s.referenceBins();
    CHECK_EQ(ref[0], 117104);
    CHECK_EQ(ref[1], 86566);
    CHECK_EQ(ref[2], 17470);
    CHECK_EQ(ref[3], 2);
    // 精确口径（真实边界，见 DbStats 注释）
    CHECK_EQ(s.degreeHistogram[0], 1348);    // deg == 1
    CHECK_EQ(s.degreeHistogram[1], 117104);  // deg == 2
    CHECK_EQ(s.degreeHistogram[2], 85218);   // 3..9
    CHECK_EQ(s.degreeHistogram[3], 17470);   // 10..99
    CHECK_EQ(s.degreeHistogram[4], 2);       // >= 100
    // 五箱之和必须等于 net 总数
    int sum = 0;
    for (int v : s.degreeHistogram) sum += v;
    CHECK_EQ(sum, s.netCount);

    // ------------------------------------------------------------ 结构不变量
    CHECK_EQ(db.numMovable, 210904);
    CHECK_EQ(db.numNodes, 211447);
    // 分段约定：[0, numMovable) 可移动，[numMovable, numNodes) 固定
    bool partitionOk = true;
    for (int i = 0; i < db.numMovable; ++i)
        if (db.isFixed(i)) partitionOk = false;
    for (int i = db.numMovable; i < db.numNodes; ++i)
        if (!db.isFixed(i)) partitionOk = false;
    CHECK_TRUE(partitionOk);

    return sptest::summary("test_parser");
}
