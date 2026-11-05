// CudaPoissonBackend：基于 cuFFT 的泊松求解器（仅 ENABLE_CUDA 构建提供定义）。
//
// 本头文件是纯 C++，不含任何 CUDA 头（铁律 5）——只有 poisson_factory.cpp
// 在 ENABLE_CUDA 下引用它，实现全部在 cuda_poisson.cu 里。
#pragma once

#include "numeric/poisson_backend.h"

namespace sp {

/// 工厂内部使用；外部一律经由 makePoissonBackend() 构造。
///
/// 没有可用的 CUDA 设备、或网格任一维不是偶数（Makhoul 重排按奇偶对折）时
/// 返回 nullptr，由工厂回退 CPU。自动推算的维度都是 2 的幂，只有 gp_bin_dim
/// 手工覆盖时才可能出现奇数。设备存在但初始化失败（显存不足等）抛 std::runtime_error。
std::unique_ptr<PoissonBackend> makeCudaPoissonBackend(const BackendConfig& cfg);

}  // namespace sp
