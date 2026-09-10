# numeric —— 数值内核层

铁律 1：本目录下的代码**不得认识"布局"**。
不得 include `db/` 下任何头文件，不得出现 `PlaceDB` / `Module` / `Net` / `macro` / `filler` 等概念。
所有接口只接收裸数组（`const float*` / `BinGridSpan`）与标量参数。

铁律 5：每个内核采用 Backend Strategy 模式——
接口头文件是纯 C++（无 CUDA 头、无预处理分支），具体实现放在 `cpu/` 与 `cuda/`，
由工厂函数在运行时选择。

目录约定：
- `*_backend.h`  —— 抽象接口 + 工厂声明（纯 C++）
- `cpu/`         —— CPU 实现（Ooura DCT / FFTW + OpenMP）
- `cuda/`        —— CUDA 实现（M7 阶段，由 ENABLE_CUDA 门控）
- `third_party/` —— 第三方数值代码（如 fftsg）

当前状态：M1 阶段为空，M2′ 起开始填充（首个是 poisson_backend）。

检查命令（应无输出）：

    grep -rE "PlaceDB|Module|Net\b|macro|filler" src/numeric/ --include=*.h --include=*.cpp
