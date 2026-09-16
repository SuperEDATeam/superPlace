// 泊松求解器的抽象接口（决策 7 的 Backend Strategy）。
//
// 铁律 5：本头文件必须是**纯 C++**——不含 FFTW 头、不含 CUDA 头、不含任何
// 预处理分支。这样上层可以直接持有 std::unique_ptr<PoissonBackend> 成员，
// 而完全不需要知道是否存在 GPU 构建。
//
// 铁律 1：不得出现 PlaceDB / Module / macro / filler 等布局概念。
// 本层只解「给定 m×n 网格上的密度，求电势与电场」这一纯数学问题。
//
// 参考范式：OpenROAD gpl 的 src/gpl/src/fftBackend.h。
#pragma once

#include <memory>

#include "numeric/bin_grid_span.h"

namespace sp {

/// 在 Neumann 边界条件下求解 ∇²φ = -ρ，并给出电场 ξ = -∇φ。
class PoissonBackend {
public:
    virtual ~PoissonBackend() = default;

    PoissonBackend(const PoissonBackend&) = delete;
    PoissonBackend& operator=(const PoissonBackend&) = delete;
    PoissonBackend(PoissonBackend&&) = delete;
    PoissonBackend& operator=(PoissonBackend&&) = delete;

    /// 读 density，写 phi / field_x / field_y。四者必须共享同一网格维度。
    ///
    /// **符号约定（务必锁死）**：输出的 field 是【电场 ξ = -∇φ】，
    /// 不是数学梯度 ∇N。由 ePlace 式 (16) 有 ∇N = -qξ，
    /// 因此上层组装总梯度时密度项前是减号（见 05 §5.5.7）。
    ///
    /// 实现约定：solve() 内部不得进行堆分配——所有工作缓冲在构造期一次性分配。
    virtual void solve(BinGridSpan density, BinGridSpan phi, BinGridSpan field_x,
                       BinGridSpan field_y) = 0;

    /// 供日志标注实际选中的后端。
    virtual const char* name() const = 0;

protected:
    PoissonBackend() = default;
};

struct BackendConfig {
    int   nx = 0;
    int   ny = 0;
    float step_x = 1.f;   // bin 的物理宽度，用于把频域导数换算到物理单位
    float step_y = 1.f;
    bool  prefer_gpu = false;   // M7 起生效；当前恒走 CPU
};

/// 工厂：当前恒返回 CpuPoissonBackend。
/// ENABLE_CUDA 构建且 cfg.prefer_gpu 为真时，M7 起可返回 CudaPoissonBackend——
/// 届时调用方无需任何改动。
std::unique_ptr<PoissonBackend> makePoissonBackend(const BackendConfig& cfg);

}  // namespace sp
