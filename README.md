# superPlace

面向 EDA 物理设计的混合尺寸全局布局器。输入 BookShelf 格式电路网表，
在密度约束下最小化 HPWL 线长。

算法主线：BookShelf 解析 → Kraftwerk2 二次布局（初始解）→
ePlace 静电场模型 + Nesterov 加速梯度（全局布局）→ Abacus 合法化 → 可视化。

设计文档见 [`work_report/`](work_report/)，其中
[05-开发执行规范](work_report/05-开发执行规范.md) 是编码时的唯一真源。

## 当前进度

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| **M1** | 地基：数据层 / BookShelf 解析 / 可视化 / CLI | ✅ 完成 |
| M2 | 三种初始布局与对比分析（任务 4） | 未开始 |
| M2′ | 泊松求解器（可与 M2 并行） | 未开始 |
| M3 | ePlace 全局布局（任务 5） | 未开始 |
| M4 | GDSII 分层导出（任务 6） | 未开始 |
| M5 | Abacus 合法化（任务 7） | 未开始 |
| M6 | 大规模 benchmark（MMS / ISPD2005） | 未开始 |
| M7 | CUDA 泊松求解器 | 未开始 |

## 构建

CImg 与 nlohmann/json 已 vendor 在 `third_party/`，无需安装。系统依赖：

```bash
sudo apt install -y pkgconf libpng-dev libeigen3-dev libfftw3-dev ninja-build ccache
```

| 依赖 | 用途 |
| --- | --- |
| OpenMP | 并行（编译器自带） |
| libpng | 可视化输出 |
| Eigen3 | 稀疏矩阵 + BiCGSTAB（M2 二次布局） |
| FFTW3 | DCT / 泊松求解（M2′、M3） |
| ninja / ccache | 可选，加速构建；CMake 检测到 ccache 会自动启用 |

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

可选开关：`-DENABLE_CUDA=ON`（M7 起）、`-DENABLE_GUI=ON`、`-DENABLE_NATIVE=OFF`。

分析脚本另需 Python 包：`sudo apt install -y python3-numpy python3-matplotlib python3-pil`。

## 运行

```bash
./build/superplace-cli --aux test_data/adaptec1/adaptec1.aux \
                       --config configs/default.json \
                       --out results
```

产出 `results/<benchmark>/`：

```
adaptec1.parse.pl     布局结果（BookShelf .pl）
metrics.csv           逐迭代指标
summary.json          统计总览与各阶段耗时
plots/parse.png       可视化
```

主要参数：`--stage <list>` 选择阶段、`--seed <n>` 随机种子、
`--threads <n>` 线程数、`--no-plot` 关闭出图、`--full-plot` 逐迭代出图。
完整用法 `--help`。

## 测试

```bash
cd build && ctest --output-on-failure
```

| 测试 | 内容 |
| --- | --- |
| `test_parser` | adaptec1 的 21 项统计数字与课程基准逐项对拍 |
| `test_csr` | CSR 双向索引与朴素重建的参考结果比对 |
| `test_roundtrip` | `.pl` 写出/读回位置逐位一致，两次写出逐字节一致 |

## 目录

```
src/db/       PlaceDB（纯 SoA + CSR）、ModuleRef 句柄、统计
src/io/       BookShelf 读写
src/numeric/  数值内核（接口 / cpu / cuda 三段式，见其 README）
src/init/     初始布局（M2）
src/gp/       全局布局（M3）
src/lg/       合法化（M5）
src/plot/     CImg 渲染 + libpng 输出
src/util/     配置 / 日志 / 指标 / 计时 / 随机源
src/stages/   PlacementStage 实现
test/         单元测试
work_report/  设计文档
```
