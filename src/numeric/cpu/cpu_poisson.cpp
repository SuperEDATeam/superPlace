// 基于 FFTW r2r 的泊松求解器。
//
// ============================ 数学推导（务必读完再改） ============================
//
// 问题：Neumann 边界下求 ∇²φ = -ρ，并给出电场 ξ = -∇φ。
//
// 密度采样在 bin 中心：X = (x + 0.5)·step_x，因此天然对应 DCT-II/III 基
//   cos(π j (x+0.5) / nx)
// 该基的导数在区域两端为零，正是 Neumann 条件（这也是不能用普通 FFT 的原因）。
//
// 物理频率（域长 L_x = nx·step_x）：
//   u_j = π j / (nx·step_x),   v_k = π k / (ny·step_y)
//
// 1) 正变换。FFTW REDFT10 的定义为 Y_k = 2 Σ_j X_j cos(π(j+0.5)k/n)，二维带来 4 倍：
//      F[j][k] = 4 ΣΣ ρ cos(π j (x+0.5)/nx) cos(π k (y+0.5)/ny)
//    而 REDFT01∘REDFT10 = 2n·I（每维），二维往返为 4·nx·ny·I，故令
//      G = F / (4·nx·ny)
//    则 REDFT01_2D(G) 恰好还原 ρ。（测试 4 钉死这一系数。）
//
// 2) 记 REDFT01 的展开为 Σ_j c_j (...) cos(...)，其中 c_0 = 1、c_{j>0} = 2。
//    于是 ρ 的真实展开系数是 c_j c_k G[j][k]。
//
// 3) 设 φ = Σ c_j c_k P[j][k] cos(u_j X) cos(v_k Y)，代入 ∇²φ = -ρ 得
//      P[j][k] = G[j][k] / (u_j² + v_k²),   P[0][0] = 0（DC 消除，否则无解）
//    则 φ = REDFT01_2D(P)。
//
// 4) 电场：
//      ξ_x = -∂φ/∂X = Σ c_j c_k · P[j][k]·u_j · sin(u_j X) cos(v_k Y)
//      ξ_y = -∂φ/∂Y = Σ c_j c_k · P[j][k]·v_k · cos(u_j X) sin(v_k Y)
//
//    x 方向需要 Σ_j c_j Q[j] sin(π j (x+0.5)/nx)（j=0 项因 sin0=0 自动消失）。
//    FFTW RODFT01 的定义为
//      Y_x = (-1)^x I[n-1] + 2 Σ_{j'=0}^{n-2} I[j'] sin(π (j'+1)(x+0.5)/n)
//    对比可知必须做 **索引下移一位**：
//      I[j'] = Q[j'+1]  (j' = 0..n-2),   I[n-1] = 0
//    其中 I[n-1] 置零是为了消掉那个 (-1)^x 项。
//    这就是本实现最容易写错的地方，test_poisson 的测试 5（场-势一致性）
//    专门用中心差分来抓它。
//
// ============================================================================
#include "numeric/cpu/cpu_poisson.h"

#include <fftw3.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sp {
namespace {

constexpr float kPi = 3.14159265358979323846f;

class CpuPoissonBackend final : public PoissonBackend {
public:
    explicit CpuPoissonBackend(const BackendConfig& cfg)
        : nx_(cfg.nx), ny_(cfg.ny), step_x_(cfg.step_x), step_y_(cfg.step_y) {
        if (nx_ < 2 || ny_ < 2)
            throw std::runtime_error("CpuPoissonBackend: grid must be at least 2x2");
        if (!(step_x_ > 0.f) || !(step_y_ > 0.f))
            throw std::runtime_error("CpuPoissonBackend: bin step must be positive");

        const size_t n = static_cast<size_t>(nx_) * static_cast<size_t>(ny_);
        buf_in_   = alloc(n);
        buf_spec_ = alloc(n);
        buf_work_ = alloc(n);
        buf_out_  = alloc(n);

        // 频率表预计算，避免每次 solve 重复算三角函数参数
        freq_u_.resize(static_cast<size_t>(nx_));
        freq_v_.resize(static_cast<size_t>(ny_));
        for (int j = 0; j < nx_; ++j)
            freq_u_[static_cast<size_t>(j)] = kPi * static_cast<float>(j) / (static_cast<float>(nx_) * step_x_);
        for (int k = 0; k < ny_; ++k)
            freq_v_[static_cast<size_t>(k)] = kPi * static_cast<float>(k) / (static_cast<float>(ny_) * step_y_);

        // 铁律 7：必须用 FFTW_ESTIMATE。FFTW_MEASURE 会实测计时来挑算法，
        // 不同运行可能选到不同代码路径、浮点舍入随之变化，破坏逐位可复现。
        constexpr unsigned kFlags = FFTW_ESTIMATE | FFTW_UNALIGNED;

        plan_fwd_ = fftwf_plan_r2r_2d(nx_, ny_, buf_in_, buf_spec_,
                                      FFTW_REDFT10, FFTW_REDFT10, kFlags);
        plan_cc_  = fftwf_plan_r2r_2d(nx_, ny_, buf_work_, buf_out_,
                                      FFTW_REDFT01, FFTW_REDFT01, kFlags);   // φ
        plan_sc_  = fftwf_plan_r2r_2d(nx_, ny_, buf_work_, buf_out_,
                                      FFTW_RODFT01, FFTW_REDFT01, kFlags);   // ξ_x
        plan_cs_  = fftwf_plan_r2r_2d(nx_, ny_, buf_work_, buf_out_,
                                      FFTW_REDFT01, FFTW_RODFT01, kFlags);   // ξ_y

        if (!plan_fwd_ || !plan_cc_ || !plan_sc_ || !plan_cs_)
            throw std::runtime_error("CpuPoissonBackend: FFTW plan creation failed");
    }

    ~CpuPoissonBackend() override {
        if (plan_fwd_) fftwf_destroy_plan(plan_fwd_);
        if (plan_cc_)  fftwf_destroy_plan(plan_cc_);
        if (plan_sc_)  fftwf_destroy_plan(plan_sc_);
        if (plan_cs_)  fftwf_destroy_plan(plan_cs_);
        fftwf_free(buf_in_);
        fftwf_free(buf_spec_);
        fftwf_free(buf_work_);
        fftwf_free(buf_out_);
    }

    void solve(BinGridSpan density, BinGridSpan phi, BinGridSpan field_x,
               BinGridSpan field_y) override {
        checkSpan(density);
        checkSpan(phi);
        checkSpan(field_x);
        checkSpan(field_y);

        const size_t n = static_cast<size_t>(nx_) * static_cast<size_t>(ny_);

        // ---- 1. 正变换 ρ → F
        std::memcpy(buf_in_, density.data, n * sizeof(float));
        fftwf_execute(plan_fwd_);

        // ---- 2/3. 归一化得 G，再除以 (u²+v²) 得 P；DC 置零
        const float invNorm = 1.0f / (4.0f * static_cast<float>(nx_) * static_cast<float>(ny_));
#pragma omp parallel for schedule(static)
        for (int j = 0; j < nx_; ++j) {
            const float u2 = freq_u_[static_cast<size_t>(j)] * freq_u_[static_cast<size_t>(j)];
            float* row = buf_spec_ + static_cast<size_t>(j) * static_cast<size_t>(ny_);
            for (int k = 0; k < ny_; ++k) {
                const float v2 = freq_v_[static_cast<size_t>(k)] * freq_v_[static_cast<size_t>(k)];
                const float denom = u2 + v2;
                row[k] = (denom > 0.f) ? (row[k] * invNorm / denom) : 0.f;   // P[0][0] = 0
            }
        }
        // buf_spec_ 现在保存 P

        // ---- 5. φ = REDFT01_2D(P)
        std::memcpy(buf_work_, buf_spec_, n * sizeof(float));
        fftwf_execute(plan_cc_);
        std::memcpy(phi.data, buf_out_, n * sizeof(float));

        // ---- 6. ξ_x：x 维系数乘 u_j 后【下移一位】，末行补零，再做 RODFT01×REDFT01
#pragma omp parallel for schedule(static)
        for (int j = 0; j < nx_; ++j) {
            float* dst = buf_work_ + static_cast<size_t>(j) * static_cast<size_t>(ny_);
            if (j == nx_ - 1) {
                std::memset(dst, 0, static_cast<size_t>(ny_) * sizeof(float));   // I[nx-1] = 0
            } else {
                const int src_j = j + 1;                       // I[j'] = P[j'+1] · u_{j'+1}
                const float u = freq_u_[static_cast<size_t>(src_j)];
                const float* src = buf_spec_ + static_cast<size_t>(src_j) * static_cast<size_t>(ny_);
                for (int k = 0; k < ny_; ++k) dst[k] = src[k] * u;
            }
        }
        fftwf_execute(plan_sc_);
        std::memcpy(field_x.data, buf_out_, n * sizeof(float));

        // ---- 7. ξ_y：y 维同理下移一位，末列补零，再做 REDFT01×RODFT01
#pragma omp parallel for schedule(static)
        for (int j = 0; j < nx_; ++j) {
            float* dst = buf_work_ + static_cast<size_t>(j) * static_cast<size_t>(ny_);
            const float* src = buf_spec_ + static_cast<size_t>(j) * static_cast<size_t>(ny_);
            for (int k = 0; k < ny_ - 1; ++k) dst[k] = src[k + 1] * freq_v_[static_cast<size_t>(k + 1)];
            dst[ny_ - 1] = 0.f;
        }
        fftwf_execute(plan_cs_);
        std::memcpy(field_y.data, buf_out_, n * sizeof(float));
    }

    const char* name() const override { return "cpu-fftw"; }

private:
    static float* alloc(size_t n) {
        float* p = static_cast<float*>(fftwf_malloc(sizeof(float) * n));
        if (!p) throw std::runtime_error("CpuPoissonBackend: fftwf_malloc failed");
        std::memset(p, 0, sizeof(float) * n);
        return p;
    }

    void checkSpan(const BinGridSpan& s) const {
        if (!s.valid() || s.nx != nx_ || s.ny != ny_)
            throw std::runtime_error("CpuPoissonBackend: grid span dimension mismatch");
    }

    int   nx_, ny_;
    float step_x_, step_y_;

    std::vector<float> freq_u_, freq_v_;

    float* buf_in_   = nullptr;
    float* buf_spec_ = nullptr;
    float* buf_work_ = nullptr;
    float* buf_out_  = nullptr;

    fftwf_plan plan_fwd_ = nullptr;
    fftwf_plan plan_cc_  = nullptr;
    fftwf_plan plan_sc_  = nullptr;
    fftwf_plan plan_cs_  = nullptr;
};

}  // namespace

std::unique_ptr<PoissonBackend> makeCpuPoissonBackend(const BackendConfig& cfg) {
    return std::make_unique<CpuPoissonBackend>(cfg);
}

// 工厂：当前恒走 CPU。M7 加入 CUDA 后端后，此处按 cfg.prefer_gpu 分派即可，
// 调用方无需任何改动（决策 7）。
std::unique_ptr<PoissonBackend> makePoissonBackend(const BackendConfig& cfg) {
    return makeCpuPoissonBackend(cfg);
}

}  // namespace sp
