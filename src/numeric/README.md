# numeric —— 数值内核层

铁律 1：本目录下的代码**不得认识"布局"**。
不得 include `db/` 下任何头文件，不得出现 `PlaceDB` / `Module` / `Net` / `macro` / `filler` 等概念。
所有接口只接收裸数组（`const float*` / `BinGridSpan`）与标量参数。

铁律 5：每个内核采用 Backend Strategy 模式——
接口头文件是纯 C++（无 CUDA 头、无预处理分支），具体实现放在 `cpu/` 与 `cuda/`，
由工厂函数在运行时选择。

目录约定：
- `*_backend.h`  —— 抽象接口 + 工厂声明（纯 C++）
- `*_factory.cpp` —— 工厂实现，**唯一**允许出现 `#ifdef ENABLE_CUDA` 的地方
- `cpu/`         —— CPU 实现（FFTW + OpenMP）
- `cuda/`        —— CUDA 实现（由 ENABLE_CUDA 门控；`.h` 仍是纯 C++，实现在 `.cu`）
- `third_party/` —— 第三方数值代码（当前为空）

当前状态（2026-11-05）：

| 内核 | 接口 | CPU | CUDA |
| --- | --- | --- | --- |
| 泊松求解 | `poisson_backend.h`，工厂在 `poisson_factory.cpp` | `cpu/cpu_poisson.cpp`（FFTW r2r，`cpu-fftw`） | `cuda/cuda_poisson.cu`（cuFFT，`cuda-cufft`） |
| Nesterov 优化器 | `optimizer.h` | `cpu/nesterov.cpp` | 无（向量运算抽象推迟到 GPU 常驻方案一并设计） |
| 确定性归约 | `reduction.h`（头文件内联） | — | — |

泊松后端由配置项 `poisson_backend`（`"cpu"` | `"cuda"`，默认 `"cpu"`）选择；CUDA 构建用
`cmake -B build_cuda -DENABLE_CUDA=ON`。请求 CUDA 而拿不到（非 CUDA 构建、无设备、网格维度为奇数）
时工厂回退 CPU，由调用方（`BinGrid`）告警。两个后端的系数约定逐项对齐 FFTW，推导分别写在
`cpu_poisson.cpp` 与 `cuda_poisson.cu` 开头；对拍判据见 05 §7.4，测试是 `test_poisson`
（每个可用后端各跑一遍解析测试）与 `test_poisson_parity`（仅 CUDA 构建）。

检查命令（应无输出）：

    grep -rE "PlaceDB|Module|Net\b|macro|filler" src/numeric/ --include=*.h --include=*.cpp
