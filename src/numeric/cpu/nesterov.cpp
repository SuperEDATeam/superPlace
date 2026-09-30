// Nesterov 加速梯度法（ePlace 变体，05 §5.5.10）。
//
// 迭代式：
//   a_0 = 1;  u_0 = v_0 = pos_init
//   g     = ∇f(u_k)
//   α_k   = 1 / L_k,  L_k = ‖∇f(u_k) − ∇f(u_{k−1})‖ / ‖u_k − u_{k−1}‖
//   a_{k+1} = (1 + sqrt(4a_k² + 1)) / 2
//   v_{k+1} = u_k − α_k·g
//   u_{k+1} = v_{k+1} + (a_k − 1)/a_{k+1} · (v_{k+1} − v_k)
//
// 其中 u 是"前瞻点"（参考解）、v 是"当前解"。位置更新发生在 u 上——
// 这正是 Nesterov 比经典动量法收敛更快的原因：梯度在预判位置处求取。
#include <algorithm>
#include <cmath>
#include <vector>

#include "numeric/optimizer.h"
#include "numeric/reduction.h"

namespace sp {
namespace {

// 三个归约全部走固定分块（铁律 7）。它们决定步长 α，而 α 直接决定下一步走到哪里——
// 求和次序若随线程数漂移，整条优化轨迹就会跟着漂。
double l2Norm(const float* a, int n) {
    return std::sqrt(deterministicSum(n, [&](int64_t i) {
        return static_cast<double>(a[i]) * static_cast<double>(a[i]);
    }));
}

double l2Diff(const float* a, const float* b, int n) {
    return std::sqrt(deterministicSum(n, [&](int64_t i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        return d * d;
    }));
}

double dotDiff(const float* s1, const float* s2, const float* y1, const float* y2, int n) {
    return deterministicSum(n, [&](int64_t i) {
        const double sv = static_cast<double>(s1[i]) - static_cast<double>(s2[i]);
        const double yv = static_cast<double>(y1[i]) - static_cast<double>(y2[i]);
        return sv * yv;
    });
}

class NesterovOptimizer final : public Optimizer {
public:
    NesterovOptimizer(ObjAndGradFn f, ConstraintFn c, const OptConfig& cfg)
        : f_(std::move(f)), c_(std::move(c)), cfg_(cfg) {}

    void step(float* pos, int n) override {
        if (n <= 0) return;
        if (!inited_) init(pos, n);

        // ---- 1. 在前瞻点 u_k 处求梯度
        f_(u_.data(), g_.data(), &obj_);
        gradNorm_ = static_cast<float>(l2Norm(g_.data(), n));

        // ---- 2. 步长：首步用预估值，之后用 Lipschitz 常数的倒数
        alpha_ = (iter_ == 0) ? alpha0_ : estimateStep(n);

        // ---- 3. Nesterov 更新
        const float aNext = 0.5f * (1.f + std::sqrt(4.f * a_ * a_ + 1.f));
        const float momentum = (a_ - 1.f) / aNext;

        vNew_.resize(static_cast<size_t>(n));
        uNew_.resize(static_cast<size_t>(n));
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) {
            vNew_[static_cast<size_t>(i)] = u_[static_cast<size_t>(i)] - alpha_ * g_[static_cast<size_t>(i)];
            uNew_[static_cast<size_t>(i)] =
                vNew_[static_cast<size_t>(i)] +
                momentum * (vNew_[static_cast<size_t>(i)] - v_[static_cast<size_t>(i)]);
        }
        if (c_) {
            c_(vNew_.data());
            c_(uNew_.data());
        }

        // ---- 4. 滚动状态：上一轮的 u 与 g 是下一轮估 Lipschitz 常数的依据
        uPrev_.swap(u_);
        gPrev_.swap(gPrevTmp_);
        gPrevTmp_ = g_;
        v_.swap(vNew_);
        u_.swap(uNew_);
        a_ = aNext;
        ++iter_;

        // 对外暴露的始终是"当前解" v，而非前瞻点 u
        std::copy(v_.begin(), v_.end(), pos);
    }

    int    iteration() const override { return iter_; }
    double objective() const override { return obj_; }
    float  stepSize() const override { return alpha_; }
    float  gradNorm() const override { return gradNorm_; }

private:
    void init(const float* pos, int n) {
        const size_t sz = static_cast<size_t>(n);
        u_.assign(pos, pos + n);
        v_ = u_;
        uPrev_ = u_;
        g_.assign(sz, 0.f);
        gPrev_.assign(sz, 0.f);
        gPrevTmp_.assign(sz, 0.f);
        a_ = 1.f;
        alpha0_ = (cfg_.init_step > 0.f) ? cfg_.init_step : estimateInitialStep(n);
        inited_ = true;
    }

    /// 首步步长估计。
    ///
    /// α_0 = 1 在真实量纲下可能差几个数量级——布局问题的梯度量级由面积与线长决定，
    /// 与"1"没有任何关系。这里按 DREAMPlace 的 initialize_learning_rate 做法：
    /// 对初值施加一个小扰动，用两点差分估一次 Lipschitz 常数，取其倒数。
    float estimateInitialStep(int n) {
        std::vector<float> probe(u_);
        std::vector<float> gA(static_cast<size_t>(n)), gB(static_cast<size_t>(n));
        double dummy = 0.0;

        f_(u_.data(), gA.data(), &dummy);

        // 扰动幅度取位置本身量级的一个小比例，避免绝对值假设
        const double posNorm = l2Norm(u_.data(), n);
        const double gNorm = l2Norm(gA.data(), n);
        if (!(gNorm > 0.0) || !std::isfinite(gNorm)) return 1.f;

        const double scale =
            cfg_.init_perturb * (posNorm > 0.0 ? posNorm : 1.0) / gNorm;
        for (int i = 0; i < n; ++i)
            probe[static_cast<size_t>(i)] =
                u_[static_cast<size_t>(i)] - static_cast<float>(scale) * gA[static_cast<size_t>(i)];
        if (c_) c_(probe.data());

        f_(probe.data(), gB.data(), &dummy);

        const double dx = l2Diff(probe.data(), u_.data(), n);
        const double dg = l2Diff(gB.data(), gA.data(), n);
        if (!(dx > 0.0) || !(dg > 0.0) || !std::isfinite(dg)) return 1.f;

        const double L = dg / dx;
        const double step = 1.0 / L;
        return (std::isfinite(step) && step > 0.0) ? static_cast<float>(step) : 1.f;
    }

    /// 后续步长：Lipschitz 倒数，可选 Barzilai-Borwein
    float estimateStep(int n) {
        const double dx = l2Diff(u_.data(), uPrev_.data(), n);
        const double dg = l2Diff(g_.data(), gPrevTmp_.data(), n);
        if (!(dx > 0.0) || !(dg > 0.0)) return alpha_;

        const double lipStep = dx / dg;   // = 1/L

        if (cfg_.use_bb) {
            // BB 短步：s·y / ‖y‖²，其中 s = u_k − u_{k−1}, y = g_k − g_{k−1}
            const double sy = dotDiff(u_.data(), uPrev_.data(), g_.data(), gPrevTmp_.data(), n);
            const double yy = dg * dg;
            if (yy > 0.0) {
                const double bb = sy / yy;
                // sy <= 0 说明曲率信息不可靠（非凸区域），回退到 Lipschitz 步长
                if (bb > 0.0 && std::isfinite(bb)) return static_cast<float>(bb);
            }
            return static_cast<float>(std::min(dx / dg, lipStep));
        }
        return static_cast<float>(lipStep);
    }

    ObjAndGradFn f_;
    ConstraintFn c_;
    OptConfig    cfg_;

    std::vector<float> u_, v_, uPrev_, uNew_, vNew_;
    std::vector<float> g_, gPrev_, gPrevTmp_;

    float  a_ = 1.f;
    float  alpha_ = 1.f;
    float  alpha0_ = 1.f;
    float  gradNorm_ = 0.f;
    double obj_ = 0.0;
    int    iter_ = 0;
    bool   inited_ = false;
};

}  // namespace

std::unique_ptr<Optimizer> makeNesterov(ObjAndGradFn f, ConstraintFn c, const OptConfig& cfg) {
    return std::make_unique<NesterovOptimizer>(std::move(f), std::move(c), cfg);
}

}  // namespace sp
