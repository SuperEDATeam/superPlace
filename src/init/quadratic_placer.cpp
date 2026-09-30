#include "init/quadratic_placer.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <algorithm>
#include <cmath>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {

namespace {

using SpMat   = Eigen::SparseMatrix<float, Eigen::RowMajor>;
using Triplet = Eigen::Triplet<float>;
using VecXf   = Eigen::VectorXf;

/// 装配某一方向的 A 与 rhs。
///
/// B2B 模型：只有「至少一端是该 net 边界引脚」的 pin 对才有权重。
/// 三种情况按 05 §5.4.2：
///   两端可移动   A[i][i]+=w  A[j][j]+=w  A[i][j]-=w  A[j][i]-=w
///                rhs[i] += -w(off_i - off_j)，rhs[j] 对称
///   一端固定     只在可移动端的对角线加 w，rhs 收固定端的绝对坐标
template <Axis A>
/// 装配是【串行】的，这是有意的选择。
///
/// 曾经并行过：按线程分桶，后果是 QP 的解随核数变（adaptec1 上 1/4/32 线程
/// 分别给出 4.10215e7 / 4.10362e7 / 4.09846e7）。改成固定分桶能消除这一点，
/// 但桶数是个没有原理依据的魔数，而 MMS 的最终结果对 QP 的数值细节高度敏感——
/// 16 桶与 32 桶在 MMS adaptec1 上相差 5%，且前者有一个种子直接炸到 8.5e7。
///
/// 权衡下来退回串行：它天然与线程数无关、代码简单，代价只是装配从 168 ms 回到
/// 216 ms（占 QP 总耗时约 9%）。真要提速，瓶颈其实在 setFromTriplets（占六成），
/// 那需要自己建 CSR，不是靠并行三元组生成。
void assemble(const PlaceDB& db, const Config& cfg, const std::vector<int>& boundMin,
              const std::vector<int>& boundMax, SpMat& mat, VecXf& rhs,
              std::vector<Triplet>& triplets, size_t nnzHint) {
    const int nMov = db.numMovable;
    const float minDist = std::max(1e-6f, cfg.qp_min_distance);

    triplets.clear();
    triplets.reserve(nnzHint);
    rhs.setZero();

    {
        std::vector<Triplet>& myTri = triplets;
        float* myRhs = rhs.data();
        for (int k = 0; k < db.numNets; ++k) {
            const int deg = db.netDegree(k);
            if (deg < 2 || deg > cfg.ignore_net_degree) continue;   // 超大 net 跳过，避免 O(deg²) 退化

            // 参与哪些 pin 对由 b2b_model.h 统一定义（与 test_b2b 共用同一份判定）
            forEachB2BPair<A>(
                db, k, boundMin[static_cast<size_t>(k)], boundMax[static_cast<size_t>(k)], minDist,
                [&](int p, int q, float w) {
                    const int ni = db.pin2node[p];
                    const int nj = db.pin2node[q];
                    const float offI = AxisTraits<A>::offset(db, p);
                    const float offJ = AxisTraits<A>::offset(db, q);
                    const bool movI = (ni < nMov);
                    const bool movJ = (nj < nMov);

                    if (movI && movJ) {
                        myTri.emplace_back(ni, ni, w);
                        myTri.emplace_back(nj, nj, w);
                        myTri.emplace_back(ni, nj, -w);
                        myTri.emplace_back(nj, ni, -w);
                        myRhs[ni] += -w * (offI - offJ);
                        myRhs[nj] += -w * (offJ - offI);
                    } else if (movI && !movJ) {
                        myTri.emplace_back(ni, ni, w);
                        myRhs[ni] += w * (AxisTraits<A>::pinPos(db, q) - offI);
                    } else if (!movI && movJ) {
                        myTri.emplace_back(nj, nj, w);
                        myRhs[nj] += w * (AxisTraits<A>::pinPos(db, p) - offJ);
                    }
                    // 两端都固定：对未知量无贡献
                });
        }
    }

    mat.setZero();
    mat.setFromTriplets(triplets.begin(), triplets.end());   // 重复项自动累加

    // 完全没有连接的可移动节点会让对角线为 0，矩阵奇异。补一个极小的对角项，
    // 效果等价于"把该节点轻轻拉向它当前的位置"，不影响有连接节点的解。
    for (int i = 0; i < nMov; ++i) {
        if (mat.coeff(i, i) == 0.f) {
            mat.coeffRef(i, i) = 1e-6f;
            rhs[i] += 1e-6f * AxisTraits<A>::nodePos(db, i);
        }
    }
}

/// 解一个方向并写回位置，返回求解器残差
template <Axis A>
float solveAxis(PlaceDB& db, const Config& cfg, const std::vector<int>& boundMin,
                const std::vector<int>& boundMax, SpMat& mat, VecXf& rhs, VecXf& sol,
                std::vector<Triplet>& triplets, size_t nnzHint, double* tAssemble,
                double* tSolve) {
    Timer tA;
    assemble<A>(db, cfg, boundMin, boundMax, mat, rhs, triplets, nnzHint);
    *tAssemble += tA.elapsedMs();
    Timer tS;

    Eigen::BiCGSTAB<SpMat, Eigen::DiagonalPreconditioner<float>> solver;
    solver.setMaxIterations(cfg.qp_solver_max_iter);
    solver.setTolerance(cfg.qp_tol);
    solver.compute(mat);
    sol = solver.solveWithGuess(rhs, sol);   // 上一轮位置是最好的初值

    const int nMov = db.numMovable;
    for (int i = 0; i < nMov; ++i) {
        float v = sol[i];
        if (!std::isfinite(v)) v = AxisTraits<A>::nodePos(db, i);
        // 夹回 core，保证整个单元在区域内
        const float half = AxisTraits<A>::halfSize(db, i);
        const float lo = AxisTraits<A>::coreLo(db) + half;
        const float hi = AxisTraits<A>::coreHi(db) - half;
        v = (lo <= hi) ? std::clamp(v, lo, hi) : 0.5f * (lo + hi);
        AxisTraits<A>::setNodePos(db, i, v);
        sol[i] = v;
    }
    *tSolve += tS.elapsedMs();
    const float err = solver.error();
    return std::isfinite(err) ? err : 0.f;
}

}  // namespace

// ----------------------------------------------------------------------------
void QuadraticPlacer::place(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    Timer total;
    const int nMov = db.numMovable;
    if (nMov == 0) return;

    // 第二步（05 §5.4.3）：所有可移动单元中心先统一移到 core 中心。
    // 这是 BiCGSTAB 的初值，也避免了全部堆在原点导致的权重奇异。
    const float cx = db.coreRegion.cx();
    const float cy = db.coreRegion.cy();
    for (int i = 0; i < nMov; ++i) {
        db.node_x[i] = cx;
        db.node_y[i] = cy;
    }

    SpMat matX(nMov, nMov), matY(nMov, nMov);
    VecXf rhsX(nMov), rhsY(nMov), solX(nMov), solY(nMov);
    for (int i = 0; i < nMov; ++i) {
        solX[i] = db.node_x[i];
        solY[i] = db.node_y[i];
    }

    // 预估非零元：每个 net 贡献 2·deg-3 个 pin 对，每对最多 4 个三元组
    size_t nnz = 0;
    for (int k = 0; k < db.numNets; ++k) {
        const int deg = db.netDegree(k);
        if (deg < 2 || deg > cfg.ignore_net_degree) continue;
        nnz += static_cast<size_t>(4 * (2 * deg - 3));
    }
    std::vector<Triplet> triplets;
    triplets.reserve(nnz);

    std::vector<int> bMinX, bMaxX, bMinY, bMaxY;

    double lastHpwl = 0.0;
    for (int iter = 0;; ++iter) {
        Timer iterTimer;

        // 权重依赖当前位置，故每轮都要重算边界引脚
        Timer tB;
        computeNetBoundPins(db, bMinX, bMaxX, bMinY, bMaxY);
        const double tBounds = tB.elapsedMs();

        double tAsm = 0.0, tSol = 0.0;
        const float errX = solveAxis<Axis::X>(db, cfg, bMinX, bMaxX, matX, rhsX, solX, triplets,
                                              nnz, &tAsm, &tSol);
        const float errY = solveAxis<Axis::Y>(db, cfg, bMinY, bMaxY, matY, rhsY, solY, triplets,
                                              nnz, &tAsm, &tSol);

        Timer tH;
        const double hpwl = computeHPWL(db);
        const double tHpwl = tH.elapsedMs();

        IterMetrics m;
        m.iter = iter;
        m.hpwl = hpwl;
        m.grad_norm_wl = std::max(errX, errY);
        m.elapsed_ms = iterTimer.elapsedMs();
        sink.push(m);
        SP_INFO("  qp iter %3d: err %.3e  HPWL %.6g  (%.0f ms = bounds %.0f + assemble %.0f + solve %.0f + hpwl %.0f)",
                iter, std::max(errX, errY), hpwl, m.elapsed_ms, tBounds, tAsm, tSol, tHpwl);

        // 外层收敛只看【布局本身】是否还在动，不看线性求解器的残差。
        //
        // 早期版本写的是 `errX < cfg.qp_tol && errY < cfg.qp_tol`，与内层 BiCGSTAB
        // 共用同一个 qp_tol。而 BiCGSTAB 恰恰是在 err < tol 时停的，于是外层一看
        // err < tol 就宣布收敛——把内层容差从 1e-6 放松到 1e-3，外层会在第 5 轮
        // 就退出，adaptec1 的 HPWL 从 4.10e7 劣化到 4.58e7。内层精度与外层不动点
        // 是两件事，必须分开。
        const bool stalled = (iter > 4 && lastHpwl > 0.0 &&
                              std::fabs(hpwl - lastHpwl) < cfg.qp_outer_tol * lastHpwl);
        lastHpwl = hpwl;
        if (stalled || iter + 1 >= cfg.qp_max_iter) break;
    }

    SP_INFO("quadratic: final HPWL = %.6g  (%.1f ms)", lastHpwl, total.elapsedMs());
}

}  // namespace sp
