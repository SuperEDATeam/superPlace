// CPU（FFTW）与 CUDA（cuFFT）泊松后端的逐点对拍（05 §7.4）。仅 ENABLE_CUDA 构建。
//
// 判据：对 φ、ξ_x、ξ_y 各自取 max|Δ| / max|CPU 值| < 1e-5。
// 不用逐元素相对误差：场在大片区域穿过零点，那里的相对误差没有意义；
// 按场的量级归一化后，1e-5 约是 float 下两次独立 FFT 舍入误差之和的上界。
//
// 顺带打印 512²、1024² 下两者的 solve() 耗时（CUDA 含 H2D/D2H 传输），只输出不断言。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "numeric/cuda/cuda_poisson.h"
#include "numeric/poisson_backend.h"
#include "test_util.h"
#include "util/rng.h"
#include "util/timer.h"

namespace {

constexpr double kTol = 1e-5;

struct Grid {
    int nx, ny;
    std::vector<float> rho, phi, ex, ey;
    Grid(int x, int y)
        : nx(x), ny(y),
          rho(size_t(x) * y), phi(size_t(x) * y), ex(size_t(x) * y), ey(size_t(x) * y) {}
    sp::BinGridSpan span(std::vector<float>& v) { return {v.data(), nx, ny}; }
};

void fillRandom(Grid& g, uint64_t seed) {
    sp::Rng rng(seed);
    for (float& v : g.rho) v = rng.uniform(0.f, 1.f);
}

/// 几个不同宽度的高斯峰叠加，再加一个常数底——真实密度并不去 DC
void fillGaussians(Grid& g) {
    const struct { float cx, cy, s, a; } peaks[] = {
        {0.3f, 0.6f, 0.05f, 1.0f}, {0.7f, 0.2f, 0.12f, 0.6f}, {0.55f, 0.85f, 0.02f, 2.0f}};
    for (int x = 0; x < g.nx; ++x)
        for (int y = 0; y < g.ny; ++y) {
            const float fx = (x + 0.5f) / g.nx, fy = (y + 0.5f) / g.ny;
            float v = 0.2f;
            for (const auto& p : peaks) {
                const float dx = fx - p.cx, dy = fy - p.cy;
                v += p.a * std::exp(-(dx * dx + dy * dy) / (2.f * p.s * p.s));
            }
            g.rho[size_t(x) * g.ny + y] = v;
        }
}

double relErr(const std::vector<float>& ref, const std::vector<float>& got) {
    double md = 0.0, mr = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        md = std::max(md, std::fabs(double(ref[i]) - double(got[i])));
        mr = std::max(mr, std::fabs(double(ref[i])));
    }
    return mr > 0.0 ? md / mr : md;
}

void compare(int nx, int ny, float sx, float sy, bool gaussian) {
    const sp::BackendConfig cfg{nx, ny, sx, sy, false};
    auto cpu = sp::makePoissonBackend(cfg);
    auto gpu = sp::makeCudaPoissonBackend(cfg);
    CHECK_TRUE(gpu != nullptr);
    if (!gpu) return;

    Grid a(nx, ny), b(nx, ny);
    if (gaussian) fillGaussians(a);
    else fillRandom(a, 20261105u + static_cast<uint64_t>(nx) * 7919u + static_cast<uint64_t>(ny));
    b.rho = a.rho;

    cpu->solve(a.span(a.rho), a.span(a.phi), a.span(a.ex), a.span(a.ey));
    gpu->solve(b.span(b.rho), b.span(b.phi), b.span(b.ex), b.span(b.ey));

    const double ep = relErr(a.phi, b.phi), ex = relErr(a.ex, b.ex), ey = relErr(a.ey, b.ey);
    std::printf("  %4dx%-4d %-6s  φ %.2e  ξx %.2e  ξy %.2e\n", nx, ny,
                gaussian ? "gauss" : "random", ep, ex, ey);
    CHECK_TRUE(ep < kTol);
    CHECK_TRUE(ex < kTol);
    CHECK_TRUE(ey < kTol);

    // 同一 GPU 上重复求解逐位一致
    Grid c(nx, ny);
    c.rho = a.rho;
    gpu->solve(c.span(c.rho), c.span(c.phi), c.span(c.ex), c.span(c.ey));
    CHECK_TRUE(c.phi == b.phi && c.ex == b.ex && c.ey == b.ey);
}

double timeSolve(sp::PoissonBackend& be, Grid& g, int reps) {
    be.solve(g.span(g.rho), g.span(g.phi), g.span(g.ex), g.span(g.ey));   // 预热
    sp::Timer t;
    for (int i = 0; i < reps; ++i)
        be.solve(g.span(g.rho), g.span(g.phi), g.span(g.ex), g.span(g.ey));
    return t.elapsedMs() / reps;
}

void reportTiming(int n) {
    const sp::BackendConfig cfg{n, n, 1.f, 1.f, false};
    auto cpu = sp::makePoissonBackend(cfg);
    auto gpu = sp::makeCudaPoissonBackend(cfg);
    if (!gpu) return;
    Grid g(n, n);
    fillRandom(g, 7);
    const int reps = n >= 1024 ? 20 : 50;
    const double tc = timeSolve(*cpu, g, reps);
    const double tg = timeSolve(*gpu, g, reps);
    std::printf("  [time] %4dx%-4d  %s %.3f ms   %s %.3f ms（含传输）  %.2fx\n", n, n, cpu->name(),
                tc, gpu->name(), tg, tc / tg);
}

}  // namespace

int main() {
    if (!sp::makeCudaPoissonBackend({64, 64, 1.f, 1.f, true})) {
        std::printf("skip: 没有可用的 CUDA 设备\n");
        return sptest::summary("test_poisson_parity");
    }
    std::printf("max|Δ| / max|CPU|，判据 < %.0e\n", kTol);
    const struct { int nx, ny; float sx, sy; } grids[] = {
        {64, 64, 2.5f, 2.5f},
        {512, 512, 20.88f, 20.86f},
        {1024, 1024, 10.44f, 10.43f},
        {256, 512, 3.0f, 1.7f},    // 非方形，且两维步长不同
        {512, 256, 1.7f, 3.0f},
    };
    for (const auto& g : grids) {
        compare(g.nx, g.ny, g.sx, g.sy, false);
        compare(g.nx, g.ny, g.sx, g.sy, true);
    }
    reportTiming(512);
    reportTiming(1024);
    return sptest::summary("test_poisson_parity");
}
