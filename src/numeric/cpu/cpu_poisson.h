// CpuPoissonBackend：基于 FFTW r2r 变换的泊松求解器。
//
// 本文件是 PoissonBackend 的具体实现，允许 include FFTW。
// 接口头 poisson_backend.h 则保持纯 C++（铁律 5）。
#pragma once

#include "numeric/poisson_backend.h"

namespace sp {

/// 工厂内部使用；外部一律经由 makePoissonBackend() 构造。
std::unique_ptr<PoissonBackend> makeCpuPoissonBackend(const BackendConfig& cfg);

}  // namespace sp
