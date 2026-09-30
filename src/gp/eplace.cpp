#include "gp/eplace.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "gp/bin_grid.h"
#include "gp/filler.h"
#include "gp/wa_wirelength.h"
#include "numeric/optimizer.h"
#include "numeric/reduction.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {

const char* toString(GpStage s) {
    switch (s) {
        case GpStage::kMGP:        return "mGP";
        case GpStage::kFillerOnly: return "FILLERONLY";
        case GpStage::kCGP:        return "cGP";
    }
    return "?";
}

namespace {

/// 夹回 core 时留的余量。取 0 会让单元贴着边界，密度场在边界 bin 上出现
/// 半个单元宽的硬断层，优化器在那里来回抖。
constexpr float kBoundaryEps = 1e-3f;

}  // namespace

struct EPlace::Impl {
    PlaceDB&      db;
    const Config& cfg;
    MetricsSink&  sink;

    BinGrid      grid;
    WaWirelength wl;

    std::vector<int>   optIdx;   ///< pos 下标 k -> db 节点下标
    std::vector<float> pos;      ///< 2*nOpt：前半 x、后半 y
    std::vector<float> gradWl;   ///< 2*numMovable，由 WaWirelength 填
    std::vector<float> force;    ///< 2*totalNodes，密度力 qξ（**不是**梯度）

    // 调度状态。objAndGrad 只读它们，由主循环每轮更新（05 §5.5.9）
    float  gamma  = 1.f;
    float  lambda = 1.f;

    // objAndGrad 在求值点上顺带算出、供主循环读取的量
    float  tau      = 0.f;
    double hpwl     = 0.0;
    double wlObj    = 0.0;
    double sumAbsWlGrad  = 0.0;
    double sumAbsDenForce = 0.0;

    EPlace::IterCallback onIter;

    Impl(PlaceDB& d, const Config& c, MetricsSink& s) : db(d), cfg(c), sink(s) {}

    int nOpt() const { return static_cast<int>(optIdx.size()); }

    /// 选出该阶段参与优化的节点。
    ///
    /// 三个阶段的差异**全部**体现在这个函数和停止阈值上——这正是把阶段做成
    /// 参数而不是三份拷贝的理由。注意不能假设"下标 < numMovable 即可移动"：
    /// mLG 之后宏会被置 F_FIXED，但它仍留在可移动分段内（分段是位置约定，
    /// 重排代价太大），所以这里一律以 isFixed() 为准。
    void buildOptSet(GpStage stage) {
        optIdx.clear();
        optIdx.reserve(static_cast<size_t>(db.numMovable + db.numFillers));

        if (stage != GpStage::kFillerOnly) {
            for (int i = 0; i < db.numMovable; ++i) {
                if (db.isFixed(i) || db.isNI(i)) continue;
                if (stage == GpStage::kCGP && db.isMacro(i)) continue;
                optIdx.push_back(i);
            }
        }
        for (int i = db.numNodes; i < db.totalNodes(); ++i) optIdx.push_back(i);
    }

    /// 把 pos 写回 db。注意 db 存的是【中心坐标】（铁律 2），pos 同此约定。
    void scatter(const float* p) {
        const int n = nOpt();
#pragma omp parallel for schedule(static)
        for (int k = 0; k < n; ++k) {
            const int i = optIdx[static_cast<size_t>(k)];
            db.node_x[static_cast<size_t>(i)] = p[k];
            db.node_y[static_cast<size_t>(i)] = p[n + k];
        }
    }

    void gather(float* p) const {
        const int n = nOpt();
        for (int k = 0; k < n; ++k) {
            const int i = optIdx[static_cast<size_t>(k)];
            p[k] = db.node_x[static_cast<size_t>(i)];
            p[n + k] = db.node_y[static_cast<size_t>(i)];
        }
    }

    /// 投影回 coreRegion（05 §5.4.5）。作用在整个节点包围盒上，不只是中心。
    void clamp(float* p) const {
        const int n = nOpt();
        const Rect& core = db.coreRegion;
#pragma omp parallel for schedule(static)
        for (int k = 0; k < n; ++k) {
            const int i = optIdx[static_cast<size_t>(k)];
            const float hw = 0.5f * db.node_w[static_cast<size_t>(i)];
            const float hh = 0.5f * db.node_h[static_cast<size_t>(i)];
            // 单元比 core 还大时上下界会交叉，用 max 兜住，结果是居中放置
            const float xlo = core.lx + hw + kBoundaryEps;
            const float xhi = std::max(xlo, core.hx - hw - kBoundaryEps);
            const float ylo = core.ly + hh + kBoundaryEps;
            const float yhi = std::max(ylo, core.hy - hh - kBoundaryEps);
            p[k] = std::min(std::max(p[k], xlo), xhi);
            p[n + k] = std::min(std::max(p[n + k], ylo), yhi);
        }
    }

    /// 目标与梯度（05 §5.5.7）。这是全项目符号最易错的一个函数。
    void objAndGrad(const float* p, float* grad, double* obj) {
        const int n = nOpt();
        const int total = db.totalNodes();

        scatter(p);

        // ---- 密度场：先统计电荷，再解泊松
        grid.accumulate(db);
        tau = grid.overflow();
        grid.solveField();
        grid.gatherForce(db, force.data());

        // ---- 线长
        wl.compute(db, gamma, cfg.ignore_net_degree, &wlObj, gradWl.data());
        hpwl = computeHPWL(db);

        // ---- 总梯度组装
        //
        // densityForce 的语义是密度力 qξ，它指向低密度方向；而数学梯度
        // ∇D = −qξ。优化器执行 pos −= α·grad，所以这里密度项前必须是【减号】。
        // 写成加号的现象是单元越跑越挤，极易被误判成 λ 给小了。
        const int nMov = db.numMovable;
        // 固定分块规约（铁律 7）。这两个和决定 λ₀，次序漂移会让整条收敛轨迹跟着变。
        // 用 WithWork 版本把"写梯度"和"统计范数"合在一趟里，不额外扫一遍。
        double sumDen = 0.0;
        const double sumWl = deterministicSumWithWork(n, [&](int64_t kk, double& accWl) {
            const int k = static_cast<int>(kk);
            const int i = optIdx[static_cast<size_t>(k)];
            const bool hasWl = (i < nMov);

            const float wlx = hasWl ? gradWl[static_cast<size_t>(i)] : 0.f;
            const float wly = hasWl ? gradWl[static_cast<size_t>(nMov + i)] : 0.f;
            const float fx = force[static_cast<size_t>(i)];
            const float fy = force[static_cast<size_t>(total + i)];

            const double degree = static_cast<double>(db.nodeDegree(i));
            const float precond = static_cast<float>(
                1.0 / std::max(1.0, degree + static_cast<double>(lambda) * db.area(i)));

            grad[k] = precond * (wlx - lambda * fx);
            grad[n + k] = precond * (wly - lambda * fy);

            if (hasWl) accWl += std::fabs(wlx) + std::fabs(wly);
        });
        sumDen = deterministicSum(n, [&](int64_t kk) -> double {
            const int i = optIdx[static_cast<size_t>(kk)];
            return std::fabs(force[static_cast<size_t>(i)]) +
                   std::fabs(force[static_cast<size_t>(total + i)]);
        });
        sumAbsWlGrad = sumWl;
        sumAbsDenForce = sumDen;

        // 报告用的目标值取 WA 线长。密度项的能量 ½Σqφ 与线长不同量纲，
        // 相加得到的数字既不可比也不可解释；Nesterov 的步长只依赖梯度，
        // obj 不参与任何决策，所以这里报告更有意义的那一个。
        *obj = wlObj;
    }
};

EPlace::EPlace(PlaceDB& db, const Config& cfg, MetricsSink& sink)
    : impl_(std::make_unique<Impl>(db, cfg, sink)) {
    impl_->grid.initialize(db, cfg.target_density, cfg.gp_bin_dim, cfg.density_chunks);
    impl_->wl.prepare(db);
    impl_->gradWl.assign(static_cast<size_t>(2 * db.numMovable), 0.f);
    impl_->force.assign(static_cast<size_t>(2 * db.totalNodes()), 0.f);
}

EPlace::~EPlace() = default;

void EPlace::setIterCallback(IterCallback cb) { impl_->onIter = std::move(cb); }

GpResult EPlace::run(GpStage stage) {
    Impl& s = *impl_;
    PlaceDB& db = s.db;
    const Config& cfg = s.cfg;

    s.buildOptSet(stage);
    const int n = s.nOpt();
    GpResult result;
    if (n == 0) {
        SP_WARN("%s: 无可优化节点，跳过", toString(stage));
        return result;
    }

    s.pos.assign(static_cast<size_t>(2 * n), 0.f);
    s.gather(s.pos.data());
    // 必须先夹进 core：落在 core 外的节点与所有 bin 都不相交，密度贡献为 0，
    // 也就感受不到任何密度力，会永远留在外面。QP 之后通常已在 core 内，
    // 但 --stage=gp 直接从 .pl 初值跑时（adaptec1 的初值全是 0,0）就会踩到。
    s.clamp(s.pos.data());
    s.scatter(s.pos.data());

    // ---- λ 初始化（05 §5.5.9）：让线长与密度两股力初始量级相当
    s.gamma = computeGamma(1.f, s.grid.stepX());
    s.lambda = 1.f;
    {
        std::vector<float> probe(static_cast<size_t>(2 * n), 0.f);
        double dummy = 0.0;
        s.objAndGrad(s.pos.data(), probe.data(), &dummy);
        s.gamma = computeGamma(s.tau, s.grid.stepX());
        s.lambda = (s.sumAbsDenForce > 0.0)
                       ? static_cast<float>(s.sumAbsWlGrad / s.sumAbsDenForce)
                       : 1.f;
        if (!std::isfinite(s.lambda) || s.lambda <= 0.f) s.lambda = 1.f;
    }

    const float targetTau =
        (stage == GpStage::kCGP) ? cfg.cgp_target_overflow : cfg.target_overflow;
    const int maxIter =
        (stage == GpStage::kFillerOnly) ? cfg.filler_only_iters : cfg.gp_max_iter;

    SP_INFO("%s: %d 个节点参与优化 (bin %dx%d, τ0=%.4f, λ0=%.4g, γ0=%.4g)",
            toString(stage), n, s.grid.dim(), s.grid.dim(), s.tau, s.lambda, s.gamma);

    // 初态就已达标时必须【一步都不走】。停止判据若只写在 step() 之后，
    // 哪怕已经收敛也会先迈出一步——Nesterov 的首步步长按扰动法估计，量级很大，
    // 这一步足以把一个本来很好的布局推坏（thin1 上实测 HPWL 29 -> 643）。
    if (stage != GpStage::kFillerOnly && s.tau < targetTau) {
        s.grid.accumulate(db);
        result.overflow = s.grid.overflow();
        result.hpwl = computeHPWL(db);
        result.converged = true;
        SP_INFO("%s: τ0=%.4f 已低于阈值 %.4f，无需优化", toString(stage), result.overflow,
                targetTau);
        return result;
    }

    OptConfig oc;
    oc.max_iter = maxIter;
    oc.use_bb = cfg.use_bb;
    auto opt = makeNesterov(
        [&s](const float* p, float* g, double* o) { s.objAndGrad(p, g, o); },
        [&s](float* p) { s.clamp(p); }, oc);

    s.sink.beginStage(toString(stage));
    Timer stageTimer;
    double prevHpwl = 0.0;
    bool havePrev = false;

    // 停滞保护。λ 调度（05 §5.5.9）是 ΔHPWL 的【速率】控制器，它会把 HPWL 的
    // 增速稳定在 delta_hpwl_ref 上——但这条规则里没有任何一项写着"τ 不再改善就收手"。
    // 在密排混合尺寸设计上，τ 会锁死在某个高于阈值的值，而 λ 继续上涨、HPWL
    // 线性膨胀，优化器空转到迭代上限。
    //
    // 实测 MMS adaptec4（1329 个可移动宏）：第 700 轮起 τ 就钉在 0.1396 不动，
    // 跑到第 3000 轮 τ 仍是 0.1398，HPWL 却从 1.83e8 涨到 9.80e8。跑得越久结果越差。
    // 故记录 τ 最优时的解，停滞则回到它——与 mLG 保留最优解是同一个道理。
    std::vector<float> bestPos = s.pos;
    float  bestTau = std::numeric_limits<float>::infinity();
    double bestHpwl = 0.0;
    int    bestIter = -1;
    int    sinceImprove = 0;
    bool   stagnated = false;

    for (int iter = 0; iter < maxIter; ++iter) {
        opt->step(s.pos.data(), 2 * n);

        // τ / HPWL 取自本轮求值点（Nesterov 的前瞻点 u_k），这是标准做法：
        // 梯度在哪里求的，指标就在哪里报，二者一致才好判读收敛曲线。
        if (!std::isfinite(s.hpwl) || !std::isfinite(s.tau)) {
            SP_ERROR("%s: 第 %d 轮出现 NaN/Inf，中止", toString(stage), iter);
            break;
        }

        // λ 调度（05 §5.5.9）。HPWL 涨得越猛，说明密度压得过狠，μ 越小；
        // HPWL 下降时取上界 1.05，继续加大密度权重把单元铺开。
        if (havePrev) {
            const double dHpwl = s.hpwl - prevHpwl;
            const double expo = -dHpwl / static_cast<double>(cfg.delta_hpwl_ref) + 1.0;
            const double mu = std::pow(1.1, expo);
            s.lambda *= static_cast<float>(std::min(1.05, std::max(0.95, mu)));
        }
        prevHpwl = s.hpwl;
        havePrev = true;

        s.gamma = computeGamma(s.tau, s.grid.stepX());

        IterMetrics m;
        m.iter = iter;
        m.hpwl = s.hpwl;
        m.overflow = s.tau;
        m.lambda = s.lambda;
        m.gamma = s.gamma;
        m.step_size = opt->stepSize();
        m.grad_norm_wl = static_cast<float>(s.sumAbsWlGrad);
        m.grad_norm_den = static_cast<float>(s.sumAbsDenForce);
        m.elapsed_ms = stageTimer.elapsedMs();
        s.sink.push(m);

        if (cfg.verbose || iter % 20 == 0) {
            SP_INFO("  [%s %4d] HPWL=%.5g  τ=%.4f  λ=%.4g  γ=%.4g  α=%.4g",
                    toString(stage), iter, s.hpwl, s.tau, s.lambda, s.gamma, opt->stepSize());
        }

        // 回调看到的是本轮求值点上的 db，与刚推给 MetricsSink 的那行指标同一位置
        if (s.onIter) s.onIter(stage, iter, db);

        result.iterations = iter + 1;
        if (stage != GpStage::kFillerOnly && s.tau < targetTau) {
            result.converged = true;
            break;
        }

        if (stage != GpStage::kFillerOnly && cfg.gp_stagnation_window > 0) {
            // 用相对量判"有改善"：τ 在 1e-4 级别的抖动不算进展
            if (s.tau < bestTau * 0.999f) {
                bestTau = s.tau;
                bestHpwl = s.hpwl;
                bestIter = iter;
                bestPos = s.pos;
                sinceImprove = 0;
            } else if (++sinceImprove >= cfg.gp_stagnation_window) {
                stagnated = true;
                SP_WARN("%s: τ 连续 %d 轮无改善（停在 %.4f，最好 %.4f @ 第 %d 轮），"
                        "判定停滞并回退到最优解", toString(stage), sinceImprove, s.tau, bestTau,
                        bestIter);
                SP_WARN("%s: 此刻 HPWL=%.6g，而最优解处 HPWL=%.6g —— 继续跑只会让线长"
                        "单调膨胀而 τ 毫无改善", toString(stage), s.hpwl, bestHpwl);
                s.pos = bestPos;
                break;
            }
        }
    }

    // 优化器对外暴露的是"当前解" v，而求值点是前瞻点 u；循环里 db 停在 u 上，
    // 这里必须把 v 写回去，否则交付的不是优化器认定的解。
    s.clamp(s.pos.data());
    s.scatter(s.pos.data());
    s.grid.accumulate(db);
    result.overflow = s.grid.overflow();
    result.hpwl = computeHPWL(db);

    s.sink.endStage(stageTimer.elapsedMs());
    // FILLERONLY 是固定轮数，报"耗尽迭代上限"会误导；且它的 τ 必然不变——
    // τ 按定义就不含 fillerDensity（05 §5.5.8），只动 filler 改不了它。
    // 这个阶段的作用体现在它给 cGP 交付的密度场上，不在 τ 上。
    const char* verdict = (stage == GpStage::kFillerOnly) ? "（固定轮数，τ 不含 filler 故不变）"
                          : result.converged             ? "（达标）"
                          : stagnated                    ? "（τ 停滞，已回退到最优解）"
                                                         : "（耗尽迭代上限）";
    SP_INFO("%s 结束：%d 轮，HPWL=%.6g，τ=%.4f%s", toString(stage), result.iterations, result.hpwl,
            result.overflow, verdict);
    return result;
}

}  // namespace sp
