// Bound2Bound 模型的伸缩相消恒等式验证。
//
// ============================== 这个测试在防什么 ==============================
// easyPlace 在 QPlace/qplace.cpp:157 把 Y 方向的边界引脚判定写成了
//     pin2==Ymin || pin1==Ymax || pin2==Ymin || pin2==Ymax
// 第三项与第一项完全重复，而 **pin1 == boundPinYmin 整个丢失**。
// 后果是约一半的「Ymin ↔ 内部引脚」配对被跳过——不会崩溃、不会报错，
// 只表现为 Y 方向布局质量偏差，极难通过观察结果发现。
//
// 本测试直接调用生产代码的 forEachB2BPair（b2b_model.h 是"哪些对参与"的唯一真源），
// 验证 Σ w_pq·(Δ)² 是否严格等于该 net 的 HPWL 分量。
// 一旦边界判定漏项，这个恒等式立刻不成立。
//
// 数学依据：设引脚按坐标排序为 x_1 <= ... <= x_P，边界即 1 与 P。
//   参与的对 = {(1,j) : j=2..P} ∪ {(j,P) : j=2..P-1}，共 2P-3 对
//   每对贡献 w·Δ² = (1/(P-1))·|Δ|
//   Σ|Δ| = Σ_{j≥2}(x_j - x_1) + Σ_{2≤j≤P-1}(x_P - x_j) = (P-1)(x_P - x_1)
//   故总和 = x_P - x_1 = x_max - x_min
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "init/b2b_model.h"
#include "test_util.h"

namespace {

/// 构造一个单 net、P 个节点的库，节点坐标由调用者给定。
/// 引脚偏移全为 0，这样 pin 坐标即节点坐标，便于核对恒等式。
sp::PlaceDB makeSingleNetDb(const std::vector<float>& xs, const std::vector<float>& ys) {
    const int P = static_cast<int>(xs.size());
    sp::PlaceDB db;
    for (int i = 0; i < P; ++i) db.addNode("n" + std::to_string(i), 2.f, 2.f, 0);
    db.numNodes = P;
    db.numMovable = P;
    db.node_x = xs;
    db.node_y = ys;

    db.pin2node.resize(static_cast<size_t>(P));
    db.pin_offset_x.assign(static_cast<size_t>(P), 0.f);
    db.pin_offset_y.assign(static_cast<size_t>(P), 0.f);
    for (int i = 0; i < P; ++i) db.pin2node[static_cast<size_t>(i)] = i;
    db.numPins = P;

    db.net_name = {"n"};
    db.net_weight = {1.f};
    db.numNets = 1;
    db.finalizeCSR(std::vector<int>(static_cast<size_t>(P), 0));

    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 1000.f, 0.f, 1.f, 1000});
    db.computeRegions();
    return db;
}

/// 用生产代码枚举 B2B 配对，累加 Σ w·Δ²，并统计对数
template <sp::Axis A>
double quadraticForm(const sp::PlaceDB& db, int net, int bmin, int bmax, int* pairCount) {
    double sum = 0.0;
    int cnt = 0;
    // minDistance 取极小值，避免下限钳制干扰恒等式（真实布局中该下限是必要的）
    sp::forEachB2BPair<A>(db, net, bmin, bmax, 1e-9f, [&](int p, int q, float w) {
        const double d = static_cast<double>(sp::AxisTraits<A>::pinPos(db, p)) -
                         static_cast<double>(sp::AxisTraits<A>::pinPos(db, q));
        sum += static_cast<double>(w) * d * d;
        ++cnt;
    });
    if (pairCount) *pairCount = cnt;
    return sum;
}

/// 对给定坐标验证 x/y 两个方向的恒等式与对数
void checkIdentity(const std::vector<float>& xs, const std::vector<float>& ys, const char* label) {
    sp::PlaceDB db = makeSingleNetDb(xs, ys);
    std::vector<int> bMinX, bMaxX, bMinY, bMaxY;
    sp::computeNetBoundPins(db, bMinX, bMaxX, bMinY, bMaxY);

    const int P = static_cast<int>(xs.size());
    const int expectPairs = 2 * P - 3;

    int cntX = 0, cntY = 0;
    const double sx = quadraticForm<sp::Axis::X>(db, 0, bMinX[0], bMaxX[0], &cntX);
    const double sy = quadraticForm<sp::Axis::Y>(db, 0, bMinY[0], bMaxY[0], &cntY);

    const double wantX = *std::max_element(xs.begin(), xs.end()) -
                         *std::min_element(xs.begin(), xs.end());
    const double wantY = *std::max_element(ys.begin(), ys.end()) -
                         *std::min_element(ys.begin(), ys.end());

    std::printf("  %-14s P=%2d  pairs x/y = %2d/%2d (want %2d)  sum x/y = %.6f/%.6f"
                "  want %.6f/%.6f\n",
                label, P, cntX, cntY, expectPairs, sx, sy, wantX, wantY);

    // 每个 net 恰好 2P-3 对——少一对就说明边界判定漏项
    CHECK_EQ(cntX, expectPairs);
    CHECK_EQ(cntY, expectPairs);
    // 伸缩相消：二次型严格等于该方向的 HPWL 分量
    CHECK_NEAR(sx, wantX, 1e-4 * std::max(1.0, wantX));
    CHECK_NEAR(sy, wantY, 1e-4 * std::max(1.0, wantY));
}

void testSmallNets() {
    checkIdentity({10.f, 50.f}, {3.f, 90.f}, "P=2");
    checkIdentity({10.f, 30.f, 50.f}, {90.f, 3.f, 40.f}, "P=3");
    checkIdentity({5.f, 12.f, 40.f, 7.f, 33.f}, {1.f, 80.f, 22.f, 55.f, 9.f}, "P=5");
}

/// x 与 y 的极值落在不同引脚上——这是最容易暴露"照抄 X 逻辑"的场景：
/// 若 Y 方向沿用了 X 的边界引脚，恒等式会立刻不成立。
void testDecoupledExtremes() {
    // x 最小在 idx0、最大在 idx3；y 最小在 idx2、最大在 idx1
    checkIdentity({1.f, 20.f, 15.f, 40.f, 25.f}, {50.f, 99.f, 2.f, 70.f, 60.f}, "decoupled");
}

/// 随机用例：覆盖各种极值位置组合
void testRandomized() {
    std::mt19937 rng(20260916);
    std::uniform_real_distribution<float> dist(0.f, 500.f);
    for (int trial = 0; trial < 40; ++trial) {
        const int P = 2 + static_cast<int>(rng() % 9);
        std::vector<float> xs(static_cast<size_t>(P)), ys(static_cast<size_t>(P));
        for (int i = 0; i < P; ++i) {
            xs[static_cast<size_t>(i)] = dist(rng);
            ys[static_cast<size_t>(i)] = dist(rng);
        }
        sp::PlaceDB db = makeSingleNetDb(xs, ys);
        std::vector<int> bMinX, bMaxX, bMinY, bMaxY;
        sp::computeNetBoundPins(db, bMinX, bMaxX, bMinY, bMaxY);

        int cntX = 0, cntY = 0;
        const double sx = quadraticForm<sp::Axis::X>(db, 0, bMinX[0], bMaxX[0], &cntX);
        const double sy = quadraticForm<sp::Axis::Y>(db, 0, bMinY[0], bMaxY[0], &cntY);
        const double wantX = *std::max_element(xs.begin(), xs.end()) -
                             *std::min_element(xs.begin(), xs.end());
        const double wantY = *std::max_element(ys.begin(), ys.end()) -
                             *std::min_element(ys.begin(), ys.end());

        CHECK_EQ(cntX, 2 * P - 3);
        CHECK_EQ(cntY, 2 * P - 3);
        CHECK_NEAR(sx, wantX, 1e-3 * std::max(1.0, wantX));
        CHECK_NEAR(sy, wantY, 1e-3 * std::max(1.0, wantY));
    }
}

/// 反向验证：故意模拟 easyPlace 的漏项（丢掉 "p == bmin" 这一条），
/// 恒等式必须被破坏——否则说明本测试根本没有鉴别力。
void testBuggyVariantIsDetected() {
    const std::vector<float> xs = {1.f, 20.f, 15.f, 40.f, 25.f};
    const std::vector<float> ys = {50.f, 99.f, 2.f, 70.f, 60.f};
    sp::PlaceDB db = makeSingleNetDb(xs, ys);
    std::vector<int> bMinX, bMaxX, bMinY, bMaxY;
    sp::computeNetBoundPins(db, bMinX, bMaxX, bMinY, bMaxY);

    const int bmin = bMinX[0], bmax = bMaxX[0];
    const int b = db.net2pin_start[0], e = db.net2pin_start[1];
    const int deg = e - b;
    const float coef = 1.0f / static_cast<float>(deg - 1);

    double buggySum = 0.0;
    int buggyPairs = 0;
    for (int s = b; s < e; ++s) {
        const int p = db.flat_net2pin[s];
        for (int t = s + 1; t < e; ++t) {
            const int q = db.flat_net2pin[t];
            if (db.pin2node[p] == db.pin2node[q]) continue;
            // 故意漏掉 (p == bmin)，复现 easyPlace 的缺陷
            const bool pIsBound = (p == bmax);
            const bool qIsBound = (q == bmin || q == bmax);
            if (!pIsBound && !qIsBound) continue;
            const double d = static_cast<double>(db.pinX(p)) - static_cast<double>(db.pinX(q));
            buggySum += static_cast<double>(coef / std::max(std::fabs(d), 1e-9)) * d * d;
            ++buggyPairs;
        }
    }
    const double want = *std::max_element(xs.begin(), xs.end()) -
                        *std::min_element(xs.begin(), xs.end());
    std::printf("  buggy variant: pairs=%d (correct %d), sum=%.6f (correct %.6f)\n", buggyPairs,
                2 * static_cast<int>(xs.size()) - 3, buggySum, want);
    // 漏项后对数必然变少、恒等式必然被破坏
    CHECK_TRUE(buggyPairs < 2 * static_cast<int>(xs.size()) - 3);
    CHECK_TRUE(std::fabs(buggySum - want) > 1e-3);
}

}  // namespace

int main() {
    testSmallNets();
    testDecoupledExtremes();
    testRandomized();
    testBuggyVariantIsDetected();
    return sptest::summary("test_b2b");
}
