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

}  // namespace

int main() {
    testIsotropic();
    testIllConditioned();
    testConstraint();
    testDeterminism();
    testMonotoneTrend();
    return sptest::summary("test_nesterov");
}
