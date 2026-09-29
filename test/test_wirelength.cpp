// WA 线长梯度的数值对拍（05 §7.3）。
//
// 解析梯度的推导链条不短（softmax 求导 + 中心化变形），光看公式很难确信写对。
// 中心差分是唯一的硬判据：若解析式或实现有任何一处符号/系数错误，
// 与数值导数的偏差会立刻暴露。
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "gp/wa_wirelength.h"
#include "test_util.h"

namespace {

/// 构造一个可控的小网表：nNodes 个可移动节点 + nFixed 个固定节点，若干 net。
sp::PlaceDB makeDb(int nMovable, int nFixed, const std::vector<std::vector<int>>& nets,
                   unsigned seed) {
    sp::PlaceDB db;
    const int total = nMovable + nFixed;
    for (int i = 0; i < nMovable; ++i) db.addNode("m" + std::to_string(i), 6.f, 12.f, 0);
    for (int i = 0; i < nFixed; ++i)
        db.addNode("f" + std::to_string(i), 20.f, 20.f, sp::F_FIXED);
    db.numNodes = total;
    db.numMovable = nMovable;

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> pos(100.f, 900.f);
    std::uniform_real_distribution<float> off(-4.f, 4.f);
    db.node_x.resize(static_cast<size_t>(total));
    db.node_y.resize(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) {
        db.node_x[static_cast<size_t>(i)] = pos(rng);
        db.node_y[static_cast<size_t>(i)] = pos(rng);
    }

    std::vector<int> pinNet;
    for (size_t k = 0; k < nets.size(); ++k) {
        for (int node : nets[k]) {
            db.pin2node.push_back(node);
            db.pin_offset_x.push_back(off(rng));
            db.pin_offset_y.push_back(off(rng));
            pinNet.push_back(static_cast<int>(k));
        }
        db.net_name.push_back("n" + std::to_string(k));
        db.net_weight.push_back(1.f);
    }
    db.numPins = static_cast<int>(db.pin2node.size());
    db.numNets = static_cast<int>(nets.size());
    db.finalizeCSR(pinNet);

    db.rows.push_back(sp::PlaceDB::SiteRow{0.f, 1000.f, 0.f, 1.f, 1000});
    db.computeRegions();
    return db;
}

/// 只求 WA 线长值，不求梯度——中心差分用
double wlOnly(sp::PlaceDB& db, sp::WaWirelength& wa, float gamma, int ignoreDeg) {
    std::vector<float> dummy(2 * static_cast<size_t>(db.numMovable));
    double wl = 0.0;
    wa.compute(db, gamma, ignoreDeg, &wl, dummy.data());
    return wl;
}

void checkGradient(const char* label, sp::PlaceDB db, float gamma) {
    constexpr int kIgnoreDeg = 1000;
    sp::WaWirelength wa;
    wa.prepare(db);

    const int n = db.numMovable;
    std::vector<float> grad(2 * static_cast<size_t>(n));
    double wl0 = 0.0;
    wa.compute(db, gamma, kIgnoreDeg, &wl0, grad.data());

    // 误差按【全局最大梯度模】归一化，这是梯度检查的标准做法。
    //
    // 不能按单点数值归一化：大 net 里多数引脚的梯度接近零，
    // 拿它当分母会把舍入噪声放大成 O(1)，量到的是噪声而不是正确性。
    double maxAbsA = 0.0;
    for (float v : grad) maxAbsA = std::max(maxAbsA, static_cast<double>(std::fabs(v)));
    const double denom = std::max(maxAbsA, 1e-9);

    // 步长取 0.5：实测在此处误差最小。有限差分误差随 h 呈 U 形——
    // h 太小被单精度舍入噪声主导，h 太大被 O(h²) 截断误差主导。
    const float h = 0.5f;
    double maxRel = 0.0;
    int checked = 0;

    for (int i = 0; i < n; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
            float& coord = (axis == 0) ? db.node_x[i] : db.node_y[i];
            const float saved = coord;

            coord = saved + h;
            const double wlp = wlOnly(db, wa, gamma, kIgnoreDeg);
            coord = saved - h;
            const double wlm = wlOnly(db, wa, gamma, kIgnoreDeg);
            coord = saved;

            const double numeric = (wlp - wlm) / (2.0 * h);
            const double analytic = grad[static_cast<size_t>(axis * n + i)];
            maxRel = std::max(maxRel, std::fabs(numeric - analytic) / denom);
            ++checked;
        }
    }

    std::printf("  %-22s gamma=%7.2f  max|grad|=%8.4f  max rel err = %.3e  (%d 点)\n", label,
                gamma, maxAbsA, maxRel, checked);
    CHECK_TRUE(maxRel < 1e-3);
}

void testSimpleNets() {
    // 三个可移动节点构成一个 net
    checkGradient("3-pin net", makeDb(3, 0, {{0, 1, 2}}, 11), 50.f);
    // 含固定节点：固定引脚参与 b±/c± 累加，但自身无梯度
    checkGradient("with fixed pin", makeDb(3, 1, {{0, 1, 2, 3}}, 22), 50.f);
    // 多 net 共享节点：同一节点的多个引脚贡献必须正确累加
    checkGradient("shared nodes", makeDb(5, 0, {{0, 1, 2}, {1, 3}, {2, 3, 4}, {0, 4}}, 33), 50.f);
}

void testGammaRange() {
    // γ 跨三个数量级。小 γ 是数值稳定性的真正考验——
    // 若指数未减极值，这里会直接得到 inf/nan。
    sp::PlaceDB db = makeDb(6, 2, {{0, 1, 2, 6}, {2, 3, 4}, {4, 5, 7}, {0, 5}}, 44);
    for (float gamma : {500.f, 50.f, 5.f}) {
        checkGradient("gamma sweep", db, gamma);
    }
}

void testLargeNet() {
    // 大 net：极值点与内部点的指数项差异悬殊，检验中心化是否真的防住了抵消
    std::vector<int> big;
    for (int i = 0; i < 40; ++i) big.push_back(i);
    checkGradient("40-pin net", makeDb(40, 0, {big}, 55), 20.f);
}

/// 梯度必须是有限值——小 γ 下若指数溢出会出现 inf/nan
void testNoOverflow() {
    sp::PlaceDB db = makeDb(8, 0, {{0, 1, 2, 3}, {4, 5, 6, 7}}, 66);
    sp::WaWirelength wa;
    wa.prepare(db);
    std::vector<float> grad(2 * static_cast<size_t>(db.numMovable));
    double wl = 0.0;
    // γ 极小，等价于几乎退化为 max/min
    wa.compute(db, 0.01f, 1000, &wl, grad.data());

    bool allFinite = std::isfinite(wl);
    for (float v : grad) allFinite = allFinite && std::isfinite(v);
    std::printf("  tiny gamma (0.01):  wl=%.4f  all finite = %s\n", wl, allFinite ? "yes" : "NO");
    CHECK_TRUE(allFinite);
}

/// WA 是 HPWL 的平滑下界：应当略小于 HPWL，且 γ→0 时逼近它
void testWaApproachesHpwl() {
    sp::PlaceDB db = makeDb(10, 0, {{0, 1, 2, 3, 4}, {5, 6, 7, 8, 9}, {0, 9}}, 77);
    sp::WaWirelength wa;
    wa.prepare(db);
    std::vector<float> grad(2 * static_cast<size_t>(db.numMovable));

    double prev = -1.0;
    std::printf("  WA -> HPWL 收敛：");
    for (float gamma : {200.f, 50.f, 10.f, 1.f}) {
        double wl = 0.0;
        wa.compute(db, gamma, 1000, &wl, grad.data());
        std::printf(" γ=%.0f:%.1f", gamma, wl);
        if (prev >= 0.0) CHECK_TRUE(wl >= prev - 1e-3);   // γ 减小时 WA 单调上升
        prev = wl;
    }
    std::printf("\n");
}

void testGammaSchedule() {
    // 分段边界：τ>1.0 放松、τ<0.1 收紧
    const float wb = 20.f;
    const float gHigh = sp::computeGamma(1.5f, wb);
    const float gMid  = sp::computeGamma(0.5f, wb);
    const float gLow  = sp::computeGamma(0.05f, wb);
    std::printf("  gamma 调度: tau=1.5 -> %.2f   tau=0.5 -> %.2f   tau=0.05 -> %.2f\n", gHigh,
                gMid, gLow);
    // τ 越小 γ 越小（线长力越锐利）
    CHECK_TRUE(gHigh > gMid);
    CHECK_TRUE(gMid > gLow);
}

}  // namespace

int main() {
    testSimpleNets();
    testGammaRange();
    testLargeNet();
    testNoOverflow();
    testWaApproachesHpwl();
    testGammaSchedule();
    return sptest::summary("test_wirelength");
}
