// Bound2Bound 网络模型的核心定义（Kraftwerk2）。
//
// 本文件是「哪些 pin 对参与 B2B、权重是多少」的**唯一真源**：
// 二次布局的矩阵装配与 test_b2b 的恒等式验证都走这里，
// 因此测试真的在检验生产代码的判定逻辑，而不是各写一遍。
//
// 铁律 8：X/Y 方向的差异全部收敛到 AxisTraits，禁止出现两段结构相同的代码。
// easyPlace 正是在这里复制粘贴后漏改（qplace.cpp:157 少了 pin1 == boundPinYmin
// 且有一项重复），导致约一半的「Ymin ↔ 内部引脚」配对被跳过，Y 方向 B2B 失效。
#pragma once

#include <algorithm>
#include <cmath>

#include "db/place_db.h"

namespace sp {

enum class Axis { X, Y };

template <Axis A>
struct AxisTraits;

template <>
struct AxisTraits<Axis::X> {
    static float pinPos(const PlaceDB& db, int p) { return db.pinX(p); }
    static float offset(const PlaceDB& db, int p) { return db.pin_offset_x[static_cast<size_t>(p)]; }
    static float nodePos(const PlaceDB& db, int i) { return db.node_x[i]; }
    static void  setNodePos(PlaceDB& db, int i, float v) { db.node_x[i] = v; }
    static float halfSize(const PlaceDB& db, int i) { return 0.5f * db.node_w[i]; }
    static float coreLo(const PlaceDB& db) { return db.coreRegion.lx; }
    static float coreHi(const PlaceDB& db) { return db.coreRegion.hx; }
    static const char* label() { return "x"; }
};

template <>
struct AxisTraits<Axis::Y> {
    static float pinPos(const PlaceDB& db, int p) { return db.pinY(p); }
    static float offset(const PlaceDB& db, int p) { return db.pin_offset_y[static_cast<size_t>(p)]; }
    static float nodePos(const PlaceDB& db, int i) { return db.node_y[i]; }
    static void  setNodePos(PlaceDB& db, int i, float v) { db.node_y[i] = v; }
    static float halfSize(const PlaceDB& db, int i) { return 0.5f * db.node_h[i]; }
    static float coreLo(const PlaceDB& db) { return db.coreRegion.ly; }
    static float coreHi(const PlaceDB& db) { return db.coreRegion.hy; }
    static const char* label() { return "y"; }
};

/// 枚举 net k 中参与 B2B 的 pin 对，对每对调用 fn(p, q, weight)。
///
/// 参与条件：p 与 q 中**至少一个**是该 net 在 A 方向上的边界引脚
/// （即 x/y 取到最小或最大值的那个）。两端都是内部引脚时权重为 0，跳过。
/// 这样每个 net 恰好产生 2P-3 对，求和后伸缩相消，二次型严格等于 HPWL 分量。
///
/// 权重 w = net_weight / (P-1) / max(|Δ|, minDistance)。
/// 分母下限不可省：引脚重合时权重会爆炸，导致 BiCGSTAB 震荡不收敛。
template <Axis A, typename F>
void forEachB2BPair(const PlaceDB& db, int net, int bmin, int bmax, float minDistance, F&& fn) {
    const int b = db.net2pin_start[net], e = db.net2pin_start[net + 1];
    const int deg = e - b;
    if (deg < 2) return;

    const float coef = db.net_weight[static_cast<size_t>(net)] / static_cast<float>(deg - 1);
    const float minD = std::max(1e-6f, minDistance);

    for (int s = b; s < e; ++s) {
        const int p = db.flat_net2pin[s];
        for (int t = s + 1; t < e; ++t) {
            const int q = db.flat_net2pin[t];

            // 同一节点上的两个引脚之间没有独立自由度
            if (db.pin2node[p] == db.pin2node[q]) continue;

            // 注意：p 与 q 都要各自对 bmin 和 bmax 做判断，四个条件缺一不可
            const bool pIsBound = (p == bmin || p == bmax);
            const bool qIsBound = (q == bmin || q == bmax);
            if (!pIsBound && !qIsBound) continue;

            const float dist = std::fabs(AxisTraits<A>::pinPos(db, p) - AxisTraits<A>::pinPos(db, q));
            fn(p, q, coef / std::max(dist, minD));
        }
    }
}

}  // namespace sp
