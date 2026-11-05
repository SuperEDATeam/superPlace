// 基于 cuFFT 的泊松求解器。数学与 cpu/cpu_poisson.cpp 完全一致，先读那里的推导。
//
// 思路参考 DREAMPlace 的 dreamplace/ops/dct/src/dct2_fft2_cuda_kernel.cu
// （Zixuan Jiang, Jiaqi Gu；BSD-3-Clause，Copyright (c) 2019），即 Makhoul 的
// "重排 → 二维实数 FFT → 旋转因子后处理"。DREAMPlace 的归一化约定与 FFTW 不同
// （其 dct2d 带 2/(MN)、4/(MN) 系数，idct 输出乘 MN），本文件不沿用它的系数，
// 而是直接按 FFTW 的定义重新推导，逐项对齐 CPU 版；test_poisson_parity 钉死这一点。
//
// ============================ 推导（与 FFTW 系数逐项对应） ============================
//
// 记 M = nx（慢轴 x）、N = ny（快轴 y），数据 row-major：a[x*N + y]。
// A_j(x) = πj(2x+1)/(2M)，B_k(y) = πk(2y+1)/(2N)。
//
// 重排：v[n] = a[2n]，v[M-1-n] = a[2n+1]（每维各做一次）。反过来 x 落在
//   n(x) = x/2（x 偶）或 M-(x+1)/2（x 奇）。
// 关键恒等式：e^{iπj/(2M)} · e^{2πi·n(x)·j/M} = e^{i·s_x·A_j(x)}，s_x = +1（x 偶）/ -1（x 奇）。
//
// 1) 正变换 REDFT10_2D：F[j][k] = 4 ΣΣ a cos A cos B。
//    V = R2C_2D(v)（cuFFT 正变换不归一化），令
//      H  = e^{-iπj/(2M)} e^{-iπk/(2N)} V[j][k]      → Re H  = ΣΣ a (cosA cosB - s_x s_y sinA sinB)
//      H' = e^{-iπj/(2M)} e^{+iπk/(2N)} V[j][N-k]    → Re H' = ΣΣ a (cosA cosB + s_x s_y sinA sinB)
//    故 F = 2(Re H + Re H')。二维不能像一维那样只取一项，交叉项要靠 H' 抵消。
//    k > N/2 的 V 不在 R2C 输出里，用共轭对称 V[j][k] = conj(V[-j][-k]) 取。
//
// 2) 谱除与 CPU 同式：P = F·invNorm/(u_j²+v_k²)，invNorm = 1/(4MN)，P[0][0] = 0。
//
// 3) 逆变换 REDFT01_2D：S[x][y] = ΣΣ c_j c_k X[j][k] cos A cos B（c_0 = 1，其余 2）。
//    一维时令 G[j] = e^{iπj/(2M)} (X[j] - i·X[M-j])（X[M] := 0），则 C2R(G)[n(x)] 恰为
//    REDFT01(X)[x]——X[M-j] 那一项正好补上 e^{-iA} 的分量，c_j 的 2 倍自动出现。
//    二维是两个一维算子的张量积：
//      G[j][k] = e^{iπj/(2M)} e^{iπk/(2N)} · [ (X[j][k] - X[M-j][N-k]) - i(X[M-j][k] + X[j][N-k]) ]
//    结果是实数，所以 G 厄米对称，只需给出 k ≤ N/2 的半谱交给 C2R（cuFFT 逆变换同样不归一化）。
//
// 4) 正弦方向用 sin A_j(x) = (-1)^x cos A_{M-j}(x)：
//      Σ_j c_j Q[j] sin A_j(x) = (-1)^x · REDFT01(X')[x]，X'[j] = Q[M-j]（j ≥ 1），X'[0] = 0。
//    这与 CPU 版 RODFT01 的"下移一位、末行补零"在数学上等价（j=0 项因 sin0=0 本就消失）。
//    于是 ξ_x：X'[j][k] = P[M-j][k]·u_{M-j}，输出乘 (-1)^x；ξ_y 同理作用在 y 维。
//
// 三个逆变换共用同一套前/后处理，按 mode 区分，C2R 一次批量做 3 个。
// 全程无原子操作，cuFFT plan 固定，同一 GPU 上多次运行逐位一致。
// ============================================================================
#include "numeric/cuda/cuda_poisson.h"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace sp {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void check(cudaError_t e, const char* what) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CudaPoissonBackend: ") + what + ": " +
                                 cudaGetErrorString(e));
}

void check(cufftResult r, const char* what) {
    if (r != CUFFT_SUCCESS)
        throw std::runtime_error(std::string("CudaPoissonBackend: ") + what +
                                 " 失败（cufftResult=" + std::to_string(static_cast<int>(r)) + "）");
}

__device__ __forceinline__ float2 cmul(float2 a, float2 b) {
    return make_float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

/// 线程 (k, j) 写 v[j][k]，读 a[x(j)][y(k)]：写连续、读跨 2，比反过来散写更友好。
__global__ void fwdReorder(const float* __restrict__ a, float* __restrict__ v, int M, int N) {
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (j >= M || k >= N) return;
    const int x = (j < M / 2) ? 2 * j : 2 * M - 1 - 2 * j;
    const int y = (k < N / 2) ? 2 * k : 2 * N - 1 - 2 * k;
    v[static_cast<size_t>(j) * N + k] = a[static_cast<size_t>(x) * N + y];
}

/// 半谱 V（M×(N/2+1)）上取任意 (j, k)，k > N/2 时走共轭对称
__device__ __forceinline__ float2 fetchSpec(const float2* __restrict__ V, int M, int N, int j,
                                            int k) {
    const int W = N / 2 + 1;
    if (k <= N / 2) return V[static_cast<size_t>(j) * W + k];
    const float2 c = V[static_cast<size_t>((M - j) % M) * W + (N - k)];
    return make_float2(c.x, -c.y);
}

/// F = 2(Re H + Re H')，随即谱除得 P。tw*[t] = e^{iπt/(2·dim)}。
__global__ void fwdPostDivide(const float2* __restrict__ V, float* __restrict__ P, int M, int N,
                              const float2* __restrict__ twM, const float2* __restrict__ twN,
                              const float* __restrict__ u, const float* __restrict__ v,
                              float invNorm) {
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (j >= M || k >= N) return;

    const float2 a = fetchSpec(V, M, N, j, k);
    const float2 b = fetchSpec(V, M, N, j, (N - k) % N);
    const float2 tm = twM[j], tn = twN[k];
    // Re(conj(tm·tn)·a) 与 Re(conj(tm)·tn·b)
    const float2 w = cmul(tm, tn);
    const float2 w2 = cmul(make_float2(tm.x, -tm.y), tn);
    const float reH = w.x * a.x + w.y * a.y;
    const float reH2 = w2.x * b.x - w2.y * b.y;
    const float F = 2.f * (reH + reH2);

    const float denom = u[j] * u[j] + v[k] * v[k];
    P[static_cast<size_t>(j) * N + k] = (denom > 0.f) ? (F * invNorm / denom) : 0.f;
}

/// 第 mode 个逆变换的系数表 X(i, l)。i == M 或 l == N 处按定义为 0。
///   mode 0（φ）：P[i][l]
///   mode 1（ξ_x）：P[M-i][l]·u_{M-i}，i = 0 处为 0
///   mode 2（ξ_y）：P[i][N-l]·v_{N-l}，l = 0 处为 0
__device__ __forceinline__ float coefOf(const float* __restrict__ P, const float* __restrict__ u,
                                        const float* __restrict__ v, int M, int N, int mode,
                                        int i, int l) {
    if (i >= M || l >= N) return 0.f;
    if (mode == 0) return P[static_cast<size_t>(i) * N + l];
    if (mode == 1) {
        if (i == 0) return 0.f;
        return P[static_cast<size_t>(M - i) * N + l] * u[M - i];
    }
    if (l == 0) return 0.f;
    return P[static_cast<size_t>(i) * N + (N - l)] * v[N - l];
}

/// 线程 (k, j, mode) 写 G_mode[j][k]，k ≤ N/2
__global__ void invPre(const float* __restrict__ P, float2* __restrict__ G, int M, int N,
                       const float2* __restrict__ twM, const float2* __restrict__ twN,
                       const float* __restrict__ u, const float* __restrict__ v) {
    const int W = N / 2 + 1;
    const int k = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    const int mode = blockIdx.z;
    if (j >= M || k >= W) return;

    const float re = coefOf(P, u, v, M, N, mode, j, k) - coefOf(P, u, v, M, N, mode, M - j, N - k);
    const float im = -(coefOf(P, u, v, M, N, mode, M - j, k) + coefOf(P, u, v, M, N, mode, j, N - k));
    const float2 g = cmul(cmul(twM[j], twN[k]), make_float2(re, im));
    G[static_cast<size_t>(mode) * M * W + static_cast<size_t>(j) * W + k] = g;
}

/// 逆重排，并给正弦方向补上 (-1)^x 或 (-1)^y
__global__ void invPost(const float* __restrict__ g, float* __restrict__ out, int M, int N) {
    const int y = blockIdx.x * blockDim.x + threadIdx.x;
    const int x = blockIdx.y * blockDim.y + threadIdx.y;
    const int mode = blockIdx.z;
    if (x >= M || y >= N) return;
    const int n = (x & 1) ? M - (x + 1) / 2 : x / 2;
    const int m = (y & 1) ? N - (y + 1) / 2 : y / 2;
    const size_t plane = static_cast<size_t>(M) * N;
    float val = g[mode * plane + static_cast<size_t>(n) * N + m];
    if ((mode == 1 && (x & 1)) || (mode == 2 && (y & 1))) val = -val;
    out[mode * plane + static_cast<size_t>(x) * N + y] = val;
}

class CudaPoissonBackend final : public PoissonBackend {
public:
    explicit CudaPoissonBackend(const BackendConfig& cfg)
        : M_(cfg.nx), N_(cfg.ny), step_x_(cfg.step_x), step_y_(cfg.step_y) {
        if (M_ < 2 || N_ < 2 || (M_ & 1) || (N_ & 1))
            throw std::runtime_error("CudaPoissonBackend: grid dims must be even and >= 2");
        if (!(step_x_ > 0.f) || !(step_y_ > 0.f))
            throw std::runtime_error("CudaPoissonBackend: bin step must be positive");
        try {
            init();
        } catch (...) {
            release();
            throw;
        }
    }

    ~CudaPoissonBackend() override { release(); }

    void solve(BinGridSpan density, BinGridSpan phi, BinGridSpan field_x,
               BinGridSpan field_y) override {
        checkSpan(density);
        checkSpan(phi);
        checkSpan(field_x);
        checkSpan(field_y);

        const size_t n = static_cast<size_t>(M_) * static_cast<size_t>(N_);
        const dim3 block(32, 8);
        const dim3 gridFull((N_ + 31) / 32, (M_ + 7) / 8);
        const dim3 gridHalf((N_ / 2 + 1 + 31) / 32, (M_ + 7) / 8, 3);
        const dim3 gridOut((N_ + 31) / 32, (M_ + 7) / 8, 3);

        std::memcpy(h_in_, density.data, n * sizeof(float));
        check(cudaMemcpyAsync(d_in_, h_in_, n * sizeof(float), cudaMemcpyHostToDevice, stream_),
              "H2D");

        fwdReorder<<<gridFull, block, 0, stream_>>>(d_in_, d_v_, M_, N_);
        check(cufftExecR2C(plan_fwd_, d_v_, d_V_), "cufftExecR2C");
        fwdPostDivide<<<gridFull, block, 0, stream_>>>(d_V_, d_P_, M_, N_, d_twM_, d_twN_,
                                                       d_u_, d_vf_, invNorm_);
        invPre<<<gridHalf, block, 0, stream_>>>(d_P_, d_G_, M_, N_, d_twM_, d_twN_, d_u_, d_vf_);
        check(cufftExecC2R(plan_inv_, d_G_, d_g_), "cufftExecC2R");
        invPost<<<gridOut, block, 0, stream_>>>(d_g_, d_out_, M_, N_);
        check(cudaGetLastError(), "kernel launch");

        check(cudaMemcpyAsync(h_out_, d_out_, 3 * n * sizeof(float), cudaMemcpyDeviceToHost,
                              stream_),
              "D2H");
        check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize");

        std::memcpy(phi.data, h_out_, n * sizeof(float));
        std::memcpy(field_x.data, h_out_ + n, n * sizeof(float));
        std::memcpy(field_y.data, h_out_ + 2 * n, n * sizeof(float));
    }

    const char* name() const override { return "cuda-cufft"; }

private:
    void init() {
        const size_t n = static_cast<size_t>(M_) * static_cast<size_t>(N_);
        const size_t half = static_cast<size_t>(M_) * static_cast<size_t>(N_ / 2 + 1);

        // 与 CPU 版同一表达式算频率，两边的 u、v 逐位相同
        std::vector<float> fu(static_cast<size_t>(M_)), fv(static_cast<size_t>(N_));
        for (int j = 0; j < M_; ++j)
            fu[static_cast<size_t>(j)] = kPi * static_cast<float>(j) / (static_cast<float>(M_) * step_x_);
        for (int k = 0; k < N_; ++k)
            fv[static_cast<size_t>(k)] = kPi * static_cast<float>(k) / (static_cast<float>(N_) * step_y_);
        invNorm_ = 1.0f / (4.0f * static_cast<float>(M_) * static_cast<float>(N_));

        // 旋转因子在双精度下算好再截断，免得大下标处的角度误差累积
        auto twiddle = [](int dim) {
            std::vector<float2> t(static_cast<size_t>(dim));
            for (int i = 0; i < dim; ++i) {
                const double th = M_PI * static_cast<double>(i) / (2.0 * dim);
                t[static_cast<size_t>(i)] =
                    make_float2(static_cast<float>(std::cos(th)), static_cast<float>(std::sin(th)));
            }
            return t;
        };
        const std::vector<float2> twM = twiddle(M_), twN = twiddle(N_);

        check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
        check(cudaMalloc(&d_in_, n * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_v_, n * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_V_, half * sizeof(cufftComplex)), "cudaMalloc");
        check(cudaMalloc(&d_P_, n * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_G_, 3 * half * sizeof(cufftComplex)), "cudaMalloc");
        check(cudaMalloc(&d_g_, 3 * n * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_out_, 3 * n * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_u_, static_cast<size_t>(M_) * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_vf_, static_cast<size_t>(N_) * sizeof(float)), "cudaMalloc");
        check(cudaMalloc(&d_twM_, static_cast<size_t>(M_) * sizeof(float2)), "cudaMalloc");
        check(cudaMalloc(&d_twN_, static_cast<size_t>(N_) * sizeof(float2)), "cudaMalloc");
        check(cudaMallocHost(&h_in_, n * sizeof(float)), "cudaMallocHost");
        check(cudaMallocHost(&h_out_, 3 * n * sizeof(float)), "cudaMallocHost");

        check(cudaMemcpy(d_u_, fu.data(), fu.size() * sizeof(float), cudaMemcpyHostToDevice), "H2D");
        check(cudaMemcpy(d_vf_, fv.data(), fv.size() * sizeof(float), cudaMemcpyHostToDevice), "H2D");
        check(cudaMemcpy(d_twM_, twM.data(), twM.size() * sizeof(float2), cudaMemcpyHostToDevice), "H2D");
        check(cudaMemcpy(d_twN_, twN.data(), twN.size() * sizeof(float2), cudaMemcpyHostToDevice), "H2D");

        // plan 创建时即分配好工作区，solve() 内不再分配
        check(cufftPlan2d(&plan_fwd_, M_, N_, CUFFT_R2C), "cufftPlan2d");
        plan_fwd_ok_ = true;
        int dims[2] = {M_, N_};
        check(cufftPlanMany(&plan_inv_, 2, dims, nullptr, 1, 0, nullptr, 1, 0, CUFFT_C2R, 3),
              "cufftPlanMany");
        plan_inv_ok_ = true;
        check(cufftSetStream(plan_fwd_, stream_), "cufftSetStream");
        check(cufftSetStream(plan_inv_, stream_), "cufftSetStream");
    }

    // 析构路径不抛异常：释放失败只能忽略
    void release() noexcept {
        if (plan_fwd_ok_) cufftDestroy(plan_fwd_);
        if (plan_inv_ok_) cufftDestroy(plan_inv_);
        plan_fwd_ok_ = plan_inv_ok_ = false;
        for (void* p : {static_cast<void*>(d_in_), static_cast<void*>(d_v_),
                        static_cast<void*>(d_V_), static_cast<void*>(d_P_),
                        static_cast<void*>(d_G_), static_cast<void*>(d_g_),
                        static_cast<void*>(d_out_), static_cast<void*>(d_u_),
                        static_cast<void*>(d_vf_), static_cast<void*>(d_twM_),
                        static_cast<void*>(d_twN_)})
            if (p) cudaFree(p);
        d_in_ = d_v_ = d_P_ = d_g_ = d_out_ = d_u_ = d_vf_ = nullptr;
        d_V_ = d_G_ = nullptr;
        d_twM_ = d_twN_ = nullptr;
        if (h_in_) cudaFreeHost(h_in_);
        if (h_out_) cudaFreeHost(h_out_);
        h_in_ = h_out_ = nullptr;
        if (stream_) cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }

    void checkSpan(const BinGridSpan& s) const {
        if (!s.valid() || s.nx != M_ || s.ny != N_)
            throw std::runtime_error("CudaPoissonBackend: grid span dimension mismatch");
    }

    int   M_, N_;
    float step_x_, step_y_;
    float invNorm_ = 0.f;

    cudaStream_t stream_ = nullptr;
    cufftHandle  plan_fwd_{}, plan_inv_{};
    bool         plan_fwd_ok_ = false, plan_inv_ok_ = false;

    float*        d_in_ = nullptr;    // 密度
    float*        d_v_ = nullptr;     // 重排后的密度
    cufftComplex* d_V_ = nullptr;     // 正变换半谱
    float*        d_P_ = nullptr;     // 谱除后的系数 P
    cufftComplex* d_G_ = nullptr;     // 3 个逆变换的半谱输入
    float*        d_g_ = nullptr;     // 3 个 C2R 输出（重排态）
    float*        d_out_ = nullptr;   // φ | ξ_x | ξ_y，连续存放以便一次 D2H
    float*        d_u_ = nullptr;
    float*        d_vf_ = nullptr;
    float2*       d_twM_ = nullptr;
    float2*       d_twN_ = nullptr;

    float* h_in_ = nullptr;           // 锁页内存
    float* h_out_ = nullptr;
};

}  // namespace

std::unique_ptr<PoissonBackend> makeCudaPoissonBackend(const BackendConfig& cfg) {
    if (cfg.nx < 2 || cfg.ny < 2 || (cfg.nx & 1) || (cfg.ny & 1)) return nullptr;
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
        cudaGetLastError();   // 清掉"无设备"留下的错误状态，免得后续误报
        return nullptr;
    }
    return std::make_unique<CudaPoissonBackend>(cfg);
}

}  // namespace sp
