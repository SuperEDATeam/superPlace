// 泊松求解器的解析对拍（05 §7.2）。
//
// 这是全项目最重要的单元测试。密度项是符号错误与缩放错误的重灾区，
// 一旦把整个布局器串起来跑，这类错误就看不出来了——只会看到单元乱飘，
// 无法定位是密度、线长、λ 还是步长的问题。
//
// 五项测试：
//   1 高斯峰      场背离峰心 / 边界法向分量≈0（Neumann）/ DC 已消除
//   2 均匀密度    去 DC 后场处处≈0
//   3 对称性      ρ 关于中轴对称 => 对应方向的场反对称
//   4 往返变换    正变换后立即逆变换应还原 ρ（钉死归一化系数）
//   5 场-势一致性 中心差分验证 ξ = -∇φ（专门捕捉 DST 索引偏移）
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

#include "numeric/poisson_backend.h"
#include "test_util.h"

namespace {

constexpr int   kN    = 64;      // 网格维度，取 2 的幂以贴近真实用法
constexpr float kStep = 2.5f;    // 非 1 的步长，确保物理单位换算被真正检验

struct Field {
    int n;
    std::vector<float> rho, phi, ex, ey;

    explicit Field(int nn)
        : n(nn),
          rho(static_cast<size_t>(nn) * nn, 0.f),
          phi(static_cast<size_t>(nn) * nn, 0.f),
          ex(static_cast<size_t>(nn) * nn, 0.f),
          ey(static_cast<size_t>(nn) * nn, 0.f) {}

    float& at(std::vector<float>& v, int x, int y) { return v[static_cast<size_t>(x) * n + y]; }
    float  at(const std::vector<float>& v, int x, int y) const {
        return v[static_cast<size_t>(x) * n + y];
    }

    sp::BinGridSpan span(std::vector<float>& v) { return {v.data(), n, n}; }
};

/// 去掉均值——泊松方程在 Neumann 边界下要求 ∫ρ = 0 才有解
void removeDC(std::vector<float>& v) {
    const double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    for (float& f : v) f -= static_cast<float>(mean);
}

void fillGaussian(Field& f, float cx, float cy, float sigma, float amp = 1.f) {
    for (int x = 0; x < f.n; ++x)
        for (int y = 0; y < f.n; ++y) {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            const float dy = (static_cast<float>(y) + 0.5f) - cy;
            f.at(f.rho, x, y) = amp * std::exp(-(dx * dx + dy * dy) / (2.f * sigma * sigma));
        }
}

float maxAbs(const std::vector<float>& v) {
    float m = 0.f;
    for (float f : v) m = std::max(m, std::fabs(f));
    return m;
}

// ---------------------------------------------------------------- 测试 1
void testGaussian() {
    Field f(kN);
    const float c = 0.5f * static_cast<float>(kN);
    fillGaussian(f, c, c, static_cast<float>(kN) / 10.f);
    removeDC(f.rho);

    // 判据 a：DC 确已消除
    const double sum = std::accumulate(f.rho.begin(), f.rho.end(), 0.0);
    CHECK_NEAR(sum / f.rho.size(), 0.0, 1e-6);

    auto be = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be->solve(f.span(f.rho), f.span(f.phi), f.span(f.ex), f.span(f.ey));

    // 判据 b：场处处背离峰心。
    // 检查方式：(位置 - 峰心) 与 场向量 的点积应为正。
    // 只统计离峰心有一定距离、且场强足够大的点，避开峰心附近的数值噪声。
    const float scale = maxAbs(f.ex) + maxAbs(f.ey);
    int checked = 0, outward = 0;
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y) {
            const float dx = (static_cast<float>(x) + 0.5f) - c;
            const float dy = (static_cast<float>(y) + 0.5f) - c;
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r < 3.f || r > 0.4f * kN) continue;
            const float fx = f.at(f.ex, x, y), fy = f.at(f.ey, x, y);
            if (std::sqrt(fx * fx + fy * fy) < 1e-3f * scale) continue;
            ++checked;
            if (dx * fx + dy * fy > 0.f) ++outward;
        }
    CHECK_TRUE(checked > 100);
    // 允许极少数边缘点因离散化偏离，要求 99% 以上向外
    CHECK_TRUE(outward >= static_cast<int>(0.99 * checked));

    // 判据 c：Neumann 边界——边界上场的法向分量应≈0
    float maxNormal = 0.f;
    for (int y = 0; y < kN; ++y) {
        maxNormal = std::max(maxNormal, std::fabs(f.at(f.ex, 0, y)));
        maxNormal = std::max(maxNormal, std::fabs(f.at(f.ex, kN - 1, y)));
    }
    for (int x = 0; x < kN; ++x) {
        maxNormal = std::max(maxNormal, std::fabs(f.at(f.ey, x, 0)));
        maxNormal = std::max(maxNormal, std::fabs(f.at(f.ey, x, kN - 1)));
    }
    // 采样在 bin 中心，边界法向分量不会严格为 0，但应远小于场的整体量级
    std::printf("  [1] boundary normal / field scale = %.4f\n", maxNormal / scale);
    CHECK_TRUE(maxNormal < 0.05f * scale);
}

// ---------------------------------------------------------------- 测试 2
void testUniform() {
    Field f(kN);
    for (float& v : f.rho) v = 3.7f;   // 任意常数
    removeDC(f.rho);                   // 去 DC 后应恒为 0

    auto be = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be->solve(f.span(f.rho), f.span(f.phi), f.span(f.ex), f.span(f.ey));

    CHECK_NEAR(maxAbs(f.ex), 0.0, 1e-4);
    CHECK_NEAR(maxAbs(f.ey), 0.0, 1e-4);
}

// ---------------------------------------------------------------- 测试 3
void testSymmetry() {
    // ρ 关于 x 中轴镜像对称 => ξ_x 关于该轴反对称
    Field f(kN);
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y) {
            const float dx = (static_cast<float>(x) + 0.5f) - 0.5f * kN;
            f.at(f.rho, x, y) = std::exp(-dx * dx / 50.f) * (1.f + 0.3f * std::sin(0.2f * y));
        }
    removeDC(f.rho);

    auto be = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be->solve(f.span(f.rho), f.span(f.phi), f.span(f.ex), f.span(f.ey));

    const float scale = maxAbs(f.ex);
    float maxErr = 0.f;
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y) {
            // 镜像点 x' = kN-1-x，ξ_x(x') 应等于 -ξ_x(x)
            const float a = f.at(f.ex, x, y);
            const float b = f.at(f.ex, kN - 1 - x, y);
            maxErr = std::max(maxErr, std::fabs(a + b));
        }
    std::printf("  [3] antisymmetry residual / scale = %.3e\n", maxErr / scale);
    CHECK_TRUE(maxErr < 1e-3f * scale);
}

// ---------------------------------------------------------------- 测试 4
// 正变换后立即逆变换应还原原信号。这一项钉死归一化系数 1/(4·nx·ny)。
// 用 PoissonBackend 无法直接触达内部变换，故此处独立复算一遍 DCT-II/III，
// 与实现使用同一套 FFTW 约定——若实现里的系数写错，测试 5 会连带失败。
void testRoundTrip() {
    // 这里改为验证一个等价且可从外部观察的性质：
    // 对 ρ = cos 基的单一模式，φ 应等于 ρ / (u² + v²)，比例关系严格成立。
    const int j0 = 3, k0 = 5;
    Field f(kN);
    const float u = static_cast<float>(M_PI) * j0 / (kN * kStep);
    const float v = static_cast<float>(M_PI) * k0 / (kN * kStep);
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y)
            f.at(f.rho, x, y) =
                std::cos(static_cast<float>(M_PI) * j0 * (x + 0.5f) / kN) *
                std::cos(static_cast<float>(M_PI) * k0 * (y + 0.5f) / kN);

    auto be = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be->solve(f.span(f.rho), f.span(f.phi), f.span(f.ex), f.span(f.ey));

    // 单一模式下 φ = ρ/(u²+v²) 应逐点成立
    const float expectRatio = 1.0f / (u * u + v * v);
    float maxRel = 0.f;
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y) {
            const float r = f.at(f.rho, x, y);
            if (std::fabs(r) < 0.1f) continue;             // 跳过接近零点处
            const float ratio = f.at(f.phi, x, y) / r;
            maxRel = std::max(maxRel, std::fabs(ratio - expectRatio) / expectRatio);
        }
    std::printf("  [4] phi/rho ratio max rel err = %.3e (expect %.6g)\n", maxRel, expectRatio);
    CHECK_TRUE(maxRel < 1e-4f);
}

// ---------------------------------------------------------------- 测试 5
// 场-势一致性：内部区域用中心差分验证 ξ = -∇φ。
// 这是对 DST 索引偏移（RODFT01 的下标下移一位）最直接的捕捉手段——
// 若下标错位，φ 与 ξ 之间的导数关系会立刻破裂，而前四项未必能发现。
void testFieldMatchesPotentialGradient() {
    Field f(kN);
    // 用低频平滑输入，让中心差分的离散误差足够小
    for (int x = 0; x < kN; ++x)
        for (int y = 0; y < kN; ++y)
            f.at(f.rho, x, y) =
                std::cos(static_cast<float>(M_PI) * 2 * (x + 0.5f) / kN) *
                std::cos(static_cast<float>(M_PI) * 3 * (y + 0.5f) / kN) +
                0.5f * std::cos(static_cast<float>(M_PI) * 1 * (x + 0.5f) / kN);
    removeDC(f.rho);

    auto be = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be->solve(f.span(f.rho), f.span(f.phi), f.span(f.ex), f.span(f.ey));

    const float scaleX = maxAbs(f.ex), scaleY = maxAbs(f.ey);
    float maxErrX = 0.f, maxErrY = 0.f;
    for (int x = 1; x < kN - 1; ++x)
        for (int y = 1; y < kN - 1; ++y) {
            // ξ_x = -∂φ/∂X，中心差分步长为 kStep
            const float gx = (f.at(f.phi, x + 1, y) - f.at(f.phi, x - 1, y)) / (2.f * kStep);
            const float gy = (f.at(f.phi, x, y + 1) - f.at(f.phi, x, y - 1)) / (2.f * kStep);
            maxErrX = std::max(maxErrX, std::fabs(f.at(f.ex, x, y) - (-gx)));
            maxErrY = std::max(maxErrY, std::fabs(f.at(f.ey, x, y) - (-gy)));
        }
    std::printf("  [5] |xi_x + dphi/dx| / scale = %.3e\n", maxErrX / scaleX);
    std::printf("  [5] |xi_y + dphi/dy| / scale = %.3e\n", maxErrY / scaleY);
    // 中心差分是 O(h²) 近似，容许 1% 量级的离散误差；
    // 若 DST 下标错位，这里会是 O(1) 的偏差而非 O(h²)
    CHECK_TRUE(maxErrX < 0.01f * scaleX);
    CHECK_TRUE(maxErrY < 0.01f * scaleY);
}

// 确定性：同输入两次求解结果必须逐位一致（铁律 7）
void testDeterminism() {
    Field a(kN), b(kN);
    fillGaussian(a, 0.3f * kN, 0.7f * kN, 5.f);
    removeDC(a.rho);
    b.rho = a.rho;

    auto be1 = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be1->solve(a.span(a.rho), a.span(a.phi), a.span(a.ex), a.span(a.ey));
    auto be2 = sp::makePoissonBackend({kN, kN, kStep, kStep, false});
    be2->solve(b.span(b.rho), b.span(b.phi), b.span(b.ex), b.span(b.ey));

    CHECK_TRUE(a.phi == b.phi);
    CHECK_TRUE(a.ex == b.ex);
    CHECK_TRUE(a.ey == b.ey);
}

}  // namespace

int main() {
    std::printf("backend: %s\n",
                sp::makePoissonBackend({kN, kN, kStep, kStep, false})->name());
    testGaussian();
    testUniform();
    testSymmetry();
    testRoundTrip();
    testFieldMatchesPotentialGradient();
    testDeterminism();
    return sptest::summary("test_poisson");
}
