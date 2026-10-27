// Nesterov 优化器验证。
//
// 在解析可验证的凸函数上测试——优化器与布局语义完全解耦（铁律 6），
// 所以可以脱离 PlaceDB 单独验证，这正是决策 8 契约设计的收益。
#include <cmath>
#include <cstdio>
#include <vector>

#include "numeric/optimizer.h"
#include "test_util.h"

namespace {

/// f(x) = 0.5 · Σ c_i (x_i − t_i)²，最优解 x = t，梯度 c_i(x_i − t_i)
struct Quadratic {
    std::vector<float> coef, target;

    void operator()(const float* pos, float* grad, double* obj) const {
        double o = 0.0;
        for (size_t i = 0; i < coef.size(); ++i) {
            const double d = static_cast<double>(pos[i]) - target[i];
            o += 0.5 * coef[i] * d * d;
            grad[i] = static_cast<float>(coef[i] * d);
        }
        *obj = o;
    }
};

double maxErr(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(double(a[i]) - double(b[i])));
    return m;
}

/// 良态：所有方向曲率相同
void testIsotropic() {
    const int n = 64;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 2.0f);
    q.target.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) q.target[static_cast<size_t>(i)] = 10.f + 0.5f * i;

    std::vector<float> pos(static_cast<size_t>(n), 0.f);
    sp::OptConfig cfg;
    cfg.use_bb = false;
    auto opt = sp::makeNesterov(q, nullptr, cfg);
    for (int k = 0; k < 200; ++k) opt->step(pos.data(), n);

    const double err = maxErr(pos, q.target);
    std::printf("  [isotropic]  max|x - x*| = %.3e   obj = %.3e\n", err, opt->objective());
    CHECK_TRUE(err < 1e-3);
}

/// 病态：曲率跨越三个数量级，考验步长自适应
void testIllConditioned() {
    const int n = 50;
    Quadratic q;
    q.coef.resize(static_cast<size_t>(n));
    q.target.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        // 曲率从 0.01 到 10
        q.coef[static_cast<size_t>(i)] = 0.01f * std::pow(10.f, 3.f * i / (n - 1));
        q.target[static_cast<size_t>(i)] = -5.f + 0.3f * i;
    }

    std::vector<float> pos(static_cast<size_t>(n), 100.f);
    sp::OptConfig cfg;
    cfg.use_bb = true;
    auto opt = sp::makeNesterov(q, nullptr, cfg);

    double obj0 = 0.0;
    {
        std::vector<float> g(static_cast<size_t>(n));
        q(pos.data(), g.data(), &obj0);
    }
    for (int k = 0; k < 2000; ++k) opt->step(pos.data(), n);

    std::printf("  [ill-cond]   obj %.4e -> %.4e   (下降 %.1f 个数量级)\n", obj0, opt->objective(),
                std::log10(obj0 / std::max(opt->objective(), 1e-30)));
    // 病态问题不要求逐点收敛，但目标值必须显著下降
    CHECK_TRUE(opt->objective() < obj0 * 1e-6);
}

/// 约束函数必须被真正应用：把解夹在 [lo, hi] 内，最优点在界外时应停在边界
void testConstraint() {
    const int n = 16;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 1.0f);
    q.target.assign(static_cast<size_t>(n), 1000.f);   // 远在可行域之外

    constexpr float kHi = 5.f;
    auto clamp = [&](float* p) {
        for (int i = 0; i < n; ++i) p[i] = std::min(p[i], kHi);
    };

    std::vector<float> pos(static_cast<size_t>(n), 0.f);
    sp::OptConfig cfg;
    auto opt = sp::makeNesterov(q, clamp, cfg);
    for (int k = 0; k < 100; ++k) opt->step(pos.data(), n);

    float maxPos = 0.f;
    for (float v : pos) maxPos = std::max(maxPos, v);
    std::printf("  [constraint] max x = %.4f  (上界 %.1f)\n", maxPos, kHi);
    CHECK_TRUE(maxPos <= kHi + 1e-4f);
    // 且应当顶到边界，而不是停在半路
    CHECK_TRUE(maxPos > kHi - 1e-2f);
}

/// 确定性：同输入两次运行必须逐位一致（铁律 7）
void testDeterminism() {
    const int n = 128;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 1.5f);
    q.target.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) q.target[static_cast<size_t>(i)] = std::sin(0.1f * i) * 20.f;

    auto run = [&]() {
        std::vector<float> pos(static_cast<size_t>(n), 3.f);
        auto opt = sp::makeNesterov(q, nullptr, sp::OptConfig{});
        for (int k = 0; k < 150; ++k) opt->step(pos.data(), n);
        return pos;
    };
    CHECK_TRUE(run() == run());
}

/// 目标值单调性：凸问题上不应出现持续上升
void testMonotoneTrend() {
    const int n = 32;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 1.0f);
    q.target.assign(static_cast<size_t>(n), 7.f);

    std::vector<float> pos(static_cast<size_t>(n), -20.f);
    auto opt = sp::makeNesterov(q, nullptr, sp::OptConfig{});

    double prev = 1e300;
    int rises = 0;
    for (int k = 0; k < 100; ++k) {
        opt->step(pos.data(), n);
        const double o = opt->objective();
        if (o > prev) ++rises;
        prev = o;
    }
    std::printf("  [monotone]   100 轮中目标值上升 %d 次（Nesterov 允许少量非单调）\n", rises);
    // Nesterov 带动量，允许少量非单调，但不应持续上升
    CHECK_TRUE(rises < 30);
    CHECK_TRUE(opt->objective() < 1e-4);
}

/// 回溯路径：良态与病态问题都要收敛，且结果逐位确定
void testBacktrackConverges() {
    const int n = 50;
    Quadratic q;
    q.coef.resize(static_cast<size_t>(n));
    q.target.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        q.coef[static_cast<size_t>(i)] = 0.01f * std::pow(10.f, 3.f * i / (n - 1));
        q.target[static_cast<size_t>(i)] = -5.f + 0.3f * i;
    }
    sp::OptConfig cfg;
    cfg.backtrack = true;

    double obj0 = 0.0;
    auto run = [&](std::vector<float>& pos, long* evals) {
        pos.assign(static_cast<size_t>(n), 100.f);
        std::vector<float> g(static_cast<size_t>(n));
        q(pos.data(), g.data(), &obj0);
        auto opt = sp::makeNesterov(q, nullptr, cfg);
        for (int k = 0; k < 2000; ++k) opt->step(pos.data(), n);
        *evals = opt->evaluations();
        return opt->objective();
    };
    std::vector<float> a, b;
    long evA = 0, evB = 0;
    const double objA = run(a, &evA);
    run(b, &evB);
    std::printf("  [backtrack]  ill-cond obj %.4e -> %.4e，2000 步求值 %ld 次\n", obj0, objA, evA);
    CHECK_TRUE(objA < obj0 * 1e-6);
    CHECK_TRUE(a == b);
    CHECK_TRUE(evA == evB);
}

/// 回溯必须能把过大的步长拉回来：首步故意给到 1/L 的 50 倍。
///
/// 常曲率二次函数上 BB 步长恰是精确的 1/L，所以不开回溯时第二步也能把当前解 v
/// 拉回原点——区别在动量：坏步的位移被带进了动量，前瞻点 u 还会被甩出去好几轮，
/// 而梯度正是在 u 上求的。回溯重走了首步，动量里不留坏步的痕迹。
/// 故比较第 3 步时前瞻点上的目标值。
void testBacktrackRecoversFromHugeStep() {
    const int n = 32;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 4.0f);   // L = 4，安全步长 0.25
    q.target.assign(static_cast<size_t>(n), 3.f);

    auto threeSteps = [&](bool backtrack, long* evals) {
        sp::OptConfig cfg;
        cfg.init_step = 50.f / 4.f;
        cfg.backtrack = backtrack;
        std::vector<float> pos(static_cast<size_t>(n), 0.f);
        auto opt = sp::makeNesterov(q, nullptr, cfg);
        for (int k = 0; k < 3; ++k) opt->step(pos.data(), n);
        *evals = opt->evaluations();
        return opt->objective();
    };
    long evBt = 0, evPlain = 0;
    const double objBt = threeSteps(true, &evBt);
    const double objPlain = threeSteps(false, &evPlain);
    std::printf("  [backtrack]  首步给 50/L：第 3 步前瞻点目标值 回溯 %.3e（求值 %ld 次），"
                "不回溯 %.3e（求值 %ld 次）\n", objBt, evBt, objPlain, evPlain);
    CHECK_TRUE(objBt < 1e-6);
    CHECK_TRUE(objPlain > 1e3);
    CHECK_EQ(evBt, evPlain + 1);   // 恰好重走了一次
}

/// 校验通过时不得改变轨迹：良态凸问题上步长从不超标，开不开回溯结果应逐位相同
void testBacktrackIsNoopWhenStepIsSafe() {
    const int n = 64;
    Quadratic q;
    q.coef.assign(static_cast<size_t>(n), 2.0f);
    q.target.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) q.target[static_cast<size_t>(i)] = 10.f + 0.5f * i;
    auto run = [&](bool backtrack) {
        sp::OptConfig cfg;
        cfg.backtrack = backtrack;
        std::vector<float> pos(static_cast<size_t>(n), 0.f);
        auto opt = sp::makeNesterov(q, nullptr, cfg);
        for (int k = 0; k < 100; ++k) opt->step(pos.data(), n);
        return std::make_pair(pos, opt->evaluations());
    };
    const auto a = run(true), b = run(false);
    std::printf("  [backtrack]  安全步长下：求值 %ld vs %ld 次，坐标逐位%s\n", a.second, b.second,
                a.first == b.first ? "相同" : "不同");
    CHECK_TRUE(a.first == b.first);
    CHECK_EQ(a.second, b.second);
}

}  // namespace

int main() {
    testIsotropic();
    testIllConditioned();
    testConstraint();
    testDeterminism();
    testMonotoneTrend();
    testBacktrackConverges();
    testBacktrackRecoversFromHugeStep();
    testBacktrackIsNoopWhenStepIsSafe();
    return sptest::summary("test_nesterov");
}
