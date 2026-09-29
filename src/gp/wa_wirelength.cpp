#include "gp/wa_wirelength.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "db/place_db.h"

namespace sp {
namespace {

constexpr float kMinGamma = 1e-4f;

}  // namespace

float computeGamma(float tau, float binStep) {
    // 05 §5.5.6：γ = 8·w_b·10^{(20/9)(τ−0.1)−1.0}
    float gamma = 8.0f * binStep;
    if (tau > 1.0f) {
        gamma *= 10.0f;            // 太拥挤：放松线长力，让密度力主导
    } else if (tau < 0.1f) {
        gamma *= 0.1f;             // 已经很散：收紧线长力，逼近真实 HPWL
    } else {
        gamma *= std::pow(10.0f, (20.0f / 9.0f) * (tau - 0.1f) - 1.0f);
    }
    return std::max(gamma, kMinGamma);
}

void WaWirelength::prepare(const PlaceDB& db) {
    const size_t np = static_cast<size_t>(db.numPins);
    const size_t nn = static_cast<size_t>(db.numNets);
    expPosX_.assign(np, 0.f);
    expNegX_.assign(np, 0.f);
    expPosY_.assign(np, 0.f);
    expNegY_.assign(np, 0.f);
    bPosX_.assign(nn, 0.f);  bNegX_.assign(nn, 0.f);
    cPosX_.assign(nn, 0.f);  cNegX_.assign(nn, 0.f);
    bPosY_.assign(nn, 0.f);  bNegY_.assign(nn, 0.f);
    cPosY_.assign(nn, 0.f);  cNegY_.assign(nn, 0.f);
    maxX_.assign(nn, 0.f);   minX_.assign(nn, 0.f);
    maxY_.assign(nn, 0.f);   minY_.assign(nn, 0.f);
}

void WaWirelength::compute(const PlaceDB& db, float gamma, int ignoreNetDegree, double* outWl,
                           float* grad) {
    const int numNets = db.numNets;
    const int numMov  = db.numMovable;
    const float g = std::max(gamma, kMinGamma);
    const float invG = 1.0f / g;

    std::fill(grad, grad + 2 * static_cast<size_t>(numMov), 0.f);

    // ========================================================================
    // 第一趟：逐 net 求极值与 b±/c±
    //
    // 两处数值处理都不是可选优化，而是正确性要求：
    //
    // 1) 指数里减去该 net 的极值，使 a± ≤ 1。否则 γ 较小时 exp 直接溢出成 inf。
    //
    // 2) c± 采用**以极值为参考点的中心化累加** c' = Σ(x_j − x_ref)·a_j。
    //    若按原式存 c = Σ x_j·a_j，梯度里的 (1 + x_i/γ)b − c/γ 两项都是大数
    //    且几乎相消（x 可达 1e4 量级，除以小 γ 后更大），单精度下灾难性抵消。
    //    中心化后各项都是小量，精度得以保持。
    //
    //    代入 c = x_ref·b + c' 可得等价的稳定形式：
    //      (1 + x_i/γ)b⁺ − c⁺/γ  =  b⁺ + [(x_i − x_max)·b⁺ − c'⁺]/γ
    //      (1 − x_i/γ)b⁻ + c⁻/γ  =  b⁻ + [c'⁻ − (x_i − x_min)·b⁻]/γ
    // ========================================================================
    double wlTotal = 0.0;
#pragma omp parallel for schedule(dynamic, 256) reduction(+ : wlTotal)
    for (int k = 0; k < numNets; ++k) {
        const int b = db.net2pin_start[k], e = db.net2pin_start[k + 1];
        const int deg = e - b;
        if (deg < 2 || deg > ignoreNetDegree) {
            // b⁺ = 0 作为"该 net 不参与"的标记，第二趟据此跳过
            bPosX_[static_cast<size_t>(k)] = 0.f;
            bPosY_[static_cast<size_t>(k)] = 0.f;
            continue;
        }

        float mxX = -std::numeric_limits<float>::max(), mnX = std::numeric_limits<float>::max();
        float mxY = -std::numeric_limits<float>::max(), mnY = std::numeric_limits<float>::max();
        for (int t = b; t < e; ++t) {
            const int p = db.flat_net2pin[t];
            const float px = db.pinX(p), py = db.pinY(p);
            mxX = std::max(mxX, px);  mnX = std::min(mnX, px);
            mxY = std::max(mxY, py);  mnY = std::min(mnY, py);
        }
        maxX_[static_cast<size_t>(k)] = mxX;  minX_[static_cast<size_t>(k)] = mnX;
        maxY_[static_cast<size_t>(k)] = mxY;  minY_[static_cast<size_t>(k)] = mnY;

        float bpx = 0.f, bnx = 0.f, cpx = 0.f, cnx = 0.f;
        float bpy = 0.f, bny = 0.f, cpy = 0.f, cny = 0.f;
        for (int t = b; t < e; ++t) {
            const int p = db.flat_net2pin[t];
            const float dxp = db.pinX(p) - mxX;      // ≤ 0
            const float dxn = db.pinX(p) - mnX;      // ≥ 0
            const float ap = std::exp(dxp * invG);
            const float an = std::exp(-dxn * invG);
            expPosX_[static_cast<size_t>(p)] = ap;
            expNegX_[static_cast<size_t>(p)] = an;
            bpx += ap;  cpx += dxp * ap;
            bnx += an;  cnx += dxn * an;

            const float dyp = db.pinY(p) - mxY;
            const float dyn = db.pinY(p) - mnY;
            const float bp = std::exp(dyp * invG);
            const float bn = std::exp(-dyn * invG);
            expPosY_[static_cast<size_t>(p)] = bp;
            expNegY_[static_cast<size_t>(p)] = bn;
            bpy += bp;  cpy += dyp * bp;
            bny += bn;  cny += dyn * bn;
        }

        bPosX_[static_cast<size_t>(k)] = bpx;  cPosX_[static_cast<size_t>(k)] = cpx;
        bNegX_[static_cast<size_t>(k)] = bnx;  cNegX_[static_cast<size_t>(k)] = cnx;
        bPosY_[static_cast<size_t>(k)] = bpy;  cPosY_[static_cast<size_t>(k)] = cpy;
        bNegY_[static_cast<size_t>(k)] = bny;  cNegY_[static_cast<size_t>(k)] = cny;

        // WA 线长：WA_x = (x_max − x_min) + c'⁺/b⁺ − c'⁻/b⁻
        // 因 c'⁺ ≤ 0、c'⁻ ≥ 0，WA 略小于 HPWL——它是 HPWL 的平滑下界，符合预期。
        wlTotal += static_cast<double>((mxX - mnX) + cpx / bpx - cnx / bnx);
        wlTotal += static_cast<double>((mxY - mnY) + cpy / bpy - cny / bny);
    }
    if (outWl) *outWl = wlTotal;

    // ========================================================================
    // 第二趟：逐【可移动节点】汇总其全部引脚的梯度贡献。
    //
    // 按节点而非按 net 并行——一个节点可能有多个引脚、分属不同 net，
    // 按 net 并行会导致多线程写同一个 grad[i]。按节点划分天然无竞争，
    // 因此不需要原子加（铁律 7：原子加既慢又破坏逐位可复现）。
    // ========================================================================
#pragma omp parallel for schedule(static)
    for (int i = 0; i < numMov; ++i) {
        float gx = 0.f, gy = 0.f;
        for (int t = db.node2pin_start[i]; t < db.node2pin_start[i + 1]; ++t) {
            const int p = db.flat_node2pin[t];
            const int k = db.pin2net[static_cast<size_t>(p)];
            if (k < 0) continue;
            if (bPosX_[static_cast<size_t>(k)] <= 0.f) continue;   // 该 net 被跳过

            // ---- x 方向
            {
                const float bp = bPosX_[static_cast<size_t>(k)];
                const float bn = bNegX_[static_cast<size_t>(k)];
                const float cp = cPosX_[static_cast<size_t>(k)];
                const float cn = cNegX_[static_cast<size_t>(k)];
                const float ap = expPosX_[static_cast<size_t>(p)];
                const float an = expNegX_[static_cast<size_t>(p)];
                const float dp = db.pinX(p) - maxX_[static_cast<size_t>(k)];
                const float dn = db.pinX(p) - minX_[static_cast<size_t>(k)];

                const float termMax = ap * (bp + (dp * bp - cp) * invG) / (bp * bp);
                const float termMin = an * (bn + (cn - dn * bn) * invG) / (bn * bn);
                gx += termMax - termMin;
            }
            // ---- y 方向
            {
                const float bp = bPosY_[static_cast<size_t>(k)];
                const float bn = bNegY_[static_cast<size_t>(k)];
                const float cp = cPosY_[static_cast<size_t>(k)];
                const float cn = cNegY_[static_cast<size_t>(k)];
                const float ap = expPosY_[static_cast<size_t>(p)];
                const float an = expNegY_[static_cast<size_t>(p)];
                const float dp = db.pinY(p) - maxY_[static_cast<size_t>(k)];
                const float dn = db.pinY(p) - minY_[static_cast<size_t>(k)];

                const float termMax = ap * (bp + (dp * bp - cp) * invG) / (bp * bp);
                const float termMin = an * (bn + (cn - dn * bn) * invG) / (bn * bn);
                gy += termMax - termMin;
            }
        }
        grad[i] = gx;
        grad[numMov + i] = gy;
    }
}

}  // namespace sp
