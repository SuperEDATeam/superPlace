// HPWL、net 包围盒、边界引脚与密度统计的验证。
// 用手工构造的小网表，逐 net 核对包围盒，避免"自己算自己"的循环论证。
#include <cmath>
#include <cstdio>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "test_util.h"

namespace {

/// 手工搭一个 3 节点 / 2 net 的小库。
///
///   node0 中心 (10,10)   node1 中心 (50,20)   node2 中心 (30,60)   均为 10x10
///   net0: node0(0,0)  node1(0,0)  node2(0,0)
///         x: 10..50 -> 40      y: 10..60 -> 50      => 90
///   net1: node0(+3,+4) node1(-2,-1)
///         abs (13,14) 与 (48,19)
///         x: 13..48 -> 35      y: 14..19 ->  5      => 40
///   合计 HPWL = 130
sp::PlaceDB makeTinyDb() {
    sp::PlaceDB db;

    db.addNode("n0", 10.f, 10.f, 0);
    db.addNode("n1", 10.f, 10.f, 0);
    db.addNode("n2", 10.f, 10.f, 0);
    db.numNodes = 3;
    db.numMovable = 3;

    db.node_x = {10.f, 50.f, 30.f};
    db.node_y = {10.f, 20.f, 60.f};

    //           net0: p0 p1 p2          net1: p3 p4
    db.pin2node     = {0, 1, 2,                0, 1};
    db.pin_offset_x = {0.f, 0.f, 0.f,          3.f, -2.f};
    db.pin_offset_y = {0.f, 0.f, 0.f,          4.f, -1.f};
    db.numPins = 5;

    db.net_name   = {"a", "b"};
    db.net_weight = {1.f, 1.f};
    db.numNets = 2;

    const std::vector<int> pinNet = {0, 0, 0, 1, 1};
    db.finalizeCSR(pinNet);

    // 一行足够覆盖上述坐标，使 coreRegion 有效
    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 80.f, 0.f, 1.f, 80});
    db.computeRegions();
    return db;
}

void testHpwlValue() {
    sp::PlaceDB db = makeTinyDb();
    CHECK_NEAR(sp::computeHPWL(db), 130.0, 1e-4);
}

void testBoundBoxes() {
    sp::PlaceDB db = makeTinyDb();
    std::vector<float> lox, hix, loy, hiy;
    sp::computeNetBoundBoxes(db, lox, hix, loy, hiy);

    CHECK_NEAR(lox[0], 10.0, 1e-4);
    CHECK_NEAR(hix[0], 50.0, 1e-4);
    CHECK_NEAR(loy[0], 10.0, 1e-4);
    CHECK_NEAR(hiy[0], 60.0, 1e-4);

    CHECK_NEAR(lox[1], 13.0, 1e-4);
    CHECK_NEAR(hix[1], 48.0, 1e-4);
    CHECK_NEAR(loy[1], 14.0, 1e-4);
    CHECK_NEAR(hiy[1], 19.0, 1e-4);
}

void testBoundPins() {
    sp::PlaceDB db = makeTinyDb();
    std::vector<int> pMinX, pMaxX, pMinY, pMaxY;
    sp::computeNetBoundPins(db, pMinX, pMaxX, pMinY, pMaxY);

    // net0：x 最小是 node0 的 p0，x 最大是 node1 的 p1
    CHECK_EQ(pMinX[0], 0);
    CHECK_EQ(pMaxX[0], 1);
    // y 最小是 p0(10)，y 最大是 p2(60)
    CHECK_EQ(pMinY[0], 0);
    CHECK_EQ(pMaxY[0], 2);

    // net1：p3 abs=(13,14)，p4 abs=(48,19)
    CHECK_EQ(pMinX[1], 3);
    CHECK_EQ(pMaxX[1], 4);
    CHECK_EQ(pMinY[1], 3);
    CHECK_EQ(pMaxY[1], 4);
}

/// 并列极值必须取下标最小者，否则结果不可复现（铁律 7）
void testBoundPinTieBreak() {
    sp::PlaceDB db = makeTinyDb();
    db.node_x[1] = db.node_x[0];   // node1 与 node0 的 x 相同
    std::vector<int> pMinX, pMaxX, pMinY, pMaxY;
    sp::computeNetBoundPins(db, pMinX, pMaxX, pMinY, pMaxY);
    // p0 与 p1 的 x 并列最小，应取 p0
    CHECK_EQ(pMinX[0], 0);
}

void testDegenerateNet() {
    // degree < 2 的 net 对 HPWL 贡献 0
    sp::PlaceDB db;
    db.addNode("only", 4.f, 4.f, 0);
    db.numNodes = 1;
    db.numMovable = 1;
    db.node_x = {7.f};
    db.node_y = {9.f};
    db.pin2node = {0};
    db.pin_offset_x = {0.f};
    db.pin_offset_y = {0.f};
    db.numPins = 1;
    db.net_name = {"solo"};
    db.net_weight = {1.f};
    db.numNets = 1;
    db.finalizeCSR({0});
    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 20.f, 0.f, 1.f, 20});
    db.computeRegions();

    CHECK_NEAR(sp::computeHPWL(db), 0.0, 1e-9);
}

void testDensityStats() {
    // 4 个 10x10 单元均匀铺在 40x40 的 core 上，2x2 网格下每格恰好被占满 1/4
    sp::PlaceDB db;
    for (int i = 0; i < 4; ++i) db.addNode("c" + std::to_string(i), 10.f, 10.f, 0);
    db.numNodes = 4;
    db.numMovable = 4;
    db.node_x = {5.f, 25.f, 5.f, 25.f};
    db.node_y = {5.f, 5.f, 25.f, 25.f};
    db.numPins = 0;
    db.numNets = 0;
    db.net2pin_start.assign(1, 0);
    db.node2pin_start.assign(5, 0);
    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 40.f, 0.f, 1.f, 40});
    db.computeRegions();

    const sp::DensityStats s = sp::computeDensityStats(db, 2, 1.0f);
    // 每格 20x20=400，单元 100 => 密度 0.25，四格相同故标准差为 0
    CHECK_NEAR(s.maxDensity, 0.25, 1e-5);
    CHECK_NEAR(s.meanDensity, 0.25, 1e-5);
    CHECK_NEAR(s.stdDensity, 0.0, 1e-5);
    CHECK_NEAR(s.overflowArea, 0.0, 1e-5);
}

}  // namespace

int main() {
    testHpwlValue();
    testBoundBoxes();
    testBoundPins();
    testBoundPinTieBreak();
    testDegenerateNet();
    testDensityStats();
    return sptest::summary("test_hpwl");
}
