// 泊松后端工厂。整个项目里只有这个实现文件按 ENABLE_CUDA 分派（铁律 5）：
// 接口头 poisson_backend.h 与各后端的头文件都保持纯 C++、无预处理分支。
#include "numeric/poisson_backend.h"

#include "numeric/cpu/cpu_poisson.h"
#ifdef ENABLE_CUDA
#include "numeric/cuda/cuda_poisson.h"
#endif

namespace sp {

// 请求 GPU 但拿不到（非 CUDA 构建、无设备、维度为奇数）时静默回退 CPU。
// 是否回退由调用方对照 name() 判断并告警——numeric 层不认识日志。
std::unique_ptr<PoissonBackend> makePoissonBackend(const BackendConfig& cfg) {
#ifdef ENABLE_CUDA
    if (cfg.prefer_gpu) {
        if (auto be = makeCudaPoissonBackend(cfg)) return be;
    }
#endif
    return makeCpuPoissonBackend(cfg);
}

}  // namespace sp
