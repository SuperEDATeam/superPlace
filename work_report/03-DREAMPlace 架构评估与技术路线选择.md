# DREAMPlace 架构评估与技术路线选择

> 评估对象：
> - **DREAMPlace** — `/home/friedrichc/DREAMPlace`（limbo018/DREAMPlace，commit `6627f33`）
> - **OpenROAD `gpl`** — `/home/friedrichc/OpenROAD/src/gpl`（commit `3435623`）
>
> 评估目的：判断"tensor + kernel + CUDA"架构是否适合本课程设计项目，
> 并识别与既有设计文档（[01-架构设计与技术选型](01-架构设计与技术选型.md)）的冲突点。
>
> 撰写日期：2026-09-09

---

## 0. 结论摘要

**采纳 DREAMPlace 的数据布局（纯 SoA + CSR）与算子划分思想，不采纳其 Python/PyTorch 技术栈；
CUDA 作为规划内的第二阶段目标，但必须在 CPU 版本验证正确之后。**

理由一句话概括：**DREAMPlace 最有价值的部分（SoA 布局、算子化解耦）与 CUDA 无关，
在纯 C++ 里就能完整获得；而这套布局同时也是将来上 GPU 的物理前提——
所以它既是当下的性能收益，也是未来的入场券。**

| 问题 | 结论 |
| --- | --- |
| 与现有设计文档有冲突吗？ | **有 4 处**，其中 2 处硬冲突（技术栈、GUI 接入）保持原设计，2 处（数据布局、可复现性）需修订 |
| 能采用 tensor/kernel 架构吗？ | ✅ **能，且改为强制要求**——用 C++ 扁平数组实现，不用 PyTorch |
| 数据布局采用哪种？ | ✅ **纯 SoA + CSR，外加零成本句柄视图**；不保留 AoS 副本（见 §4.2 冲突 3 修订） |
| 能采用 CUDA 吗？ | ✅ **规划内的第二阶段目标**（环境已就绪：CUDA 12.6 + RTX 4060）。但先 CPU 做对，再上 GPU；首个 CUDA 算子建议选 DCT/泊松求解 |

> **本节于 2026-09-09 修订。** 初版结论为"不建议 CUDA 作为主线"，其依据是"只需跑通
> `test_data/` 中的小规模样例"。经确认项目目标包含**工业级大规模 benchmark**
> （ISPD2005/2006 最大 2.5M 单元、MMS 混合尺寸套件），CUDA 的价值显著上升，故上调结论。
> 详见新增的 [§6 Benchmark 调研](#6-benchmark-调研与规模规划)。

---

## 1. 三方定位

在动手评估前，先厘清三个参考实现的关系，避免混用。

| 项目 | 语言/栈 | 规模 | 数据布局 | 并行 | 定位 |
| --- | --- | --- | --- | --- | --- |
| **easyPlace** | 纯 C++ | 10.7k 行自研 | AoS（`vector<Module*>`） | 无 | 教学可读性优先 |
| **OpenROAD `gpl`** | C++ + Kokkos | 24.2k 行 | 混合（连续存储 + 指针索引） | OpenMP + 可选 GPU | 工业生产级 |
| **DREAMPlace** | Python + C++/CUDA | 20.7k Py / 39.7k C++ / 15.2k CUDA | 纯 SoA 张量 | 多线程 + CUDA | 研究/性能标杆 |

**谱系关系**：ePlace（论文，无源码）→ RePlAce（UCSD 同组开源）→ OpenROAD `gpl`（重构整合）
与 DREAMPlace（GPU 重写）并列为两条演进路径。easyPlace 是照着 RePlAce 写的教学版。

> `OpenROAD/src/gpl/src/nesterovBase.h` 中大量出现 `Please check the equation (4) in the ePlace-MS paper.`
> 这类注释，说明 `gpl` 至今仍以 ePlace-MS 为算法基准，与我们的任务 5 完全同源。

---

## 2. DREAMPlace 架构剖析

### 2.1 五层结构

```
┌─ 编排层 (Python) ──────────────────────────────────────┐
│  Placer.py → NonLinearPlace.py                        │
│  三层嵌套调度：Lgamma → Llambda → Lsub                  │
└───────────────────────────────────────────────────────┘
        ↓
┌─ 目标函数层 (Python) ──────────────────────────────────┐
│  PlaceObj.py：obj = wirelength + density_weight·density │
│  唯一对外契约：obj_and_grad_fn(pos) → (obj, grad)       │
└───────────────────────────────────────────────────────┘
        ↓
┌─ 优化器层 (Python) ────────────────────────────────────┐
│  NesterovAcceleratedGradientOptimizer(torch.optim)     │
│  + Adam / AggMo / RAdam / 非线性 CG ⋯ 全部可插拔         │
└───────────────────────────────────────────────────────┘
        ↓
┌─ 算子层 (Python 壳 + C++/CUDA 核) ─────────────────────┐
│  ops/ 下 30 个算子，每个是独立的 torch.autograd.Function │
│  electric_potential / weighted_average_wirelength /    │
│  dct / hpwl / abacus_legalize / legality_check / ⋯      │
└───────────────────────────────────────────────────────┘
        ↓
┌─ 数据层 (Python + C++) ────────────────────────────────┐
│  PlaceDB.py：全部 1D 数组 + CSR 扁平化超图               │
└───────────────────────────────────────────────────────┘
```

### 2.2 数据布局：单一 `pos` 张量 + SoA + CSR

这是 DREAMPlace 最核心、也最值得我们借鉴的部分。

**（1）位置是一个张量，不是一堆对象**

所有可移动单元的坐标压进**一个** 1D 张量 `pos`，长度 `2N`（前 N 个是全部 x，后 N 个是全部 y）。
优化器直接把它当作待优化参数（`torch.optim` 的 `params`）。

**（2）几何属性全部 SoA**（`PlaceDB.py:44-54`）

```python
self.node_x         = None  # 1D array, cell position x
self.node_y         = None  # 1D array, cell position y
self.node_size_x    = None  # 1D array, cell width
self.node_size_y    = None  # 1D array, cell height
self.pin_offset_x   = None  # 1D array, pin offset x to its node
```

**没有 `Module` 类。** 一个单元就是若干并行数组里的同一个下标。

**（3）超图用 CSR 扁平化**（`PlaceDB.py:61-69`）

```python
self.flat_net2pin_map        = None  # flatten version of net2pin_map
self.flat_net2pin_start_map  = None  # starting index of each net
self.flat_node2pin_map       = None  # flatten version of node2pin_map
self.flat_node2pin_start_map = None  # starting index of each node
self.pin2node_map            = None  # 1D array, parent node id of each pin
```

这正是我们设计文档在讲稀疏矩阵时提到的 CSR 格式，DREAMPlace 把它用在了**整个超图的表示**上，
而不只是线性方程组。net→pin 与 node→pin 两个方向都建了索引，因为线长梯度需要按 net 聚合、
按 node 分发。

> **对比**：easyPlace 的 `Net::netPins` 是 `vector<Pin*>`，`Module::modulePins` 也是
> `vector<Pin*>`——同样的双向索引，但用了 40 万次零散堆分配和无数次指针追逐来实现。

### 2.3 算子化：一个算子 = 一个目录 = 四份实现

`ops/` 下 30 个算子，每个目录的结构高度统一。以 `electric_potential`（对应我们任务 5 的密度项）为例：

```
ops/electric_potential/
├── __init__.py
├── electric_potential.py              ← Python 壳：autograd.Function + nn.Module
├── electric_overflow.py               ← 密度溢出率 τ
├── CMakeLists.txt
└── src/
    ├── density_function.h             ← CPU/GPU 共享的数学核心
    ├── electric_density_map.cpp       ← CPU 前向
    ├── electric_density_map_cuda.cpp  ← CUDA 主机侧绑定
    ├── electric_density_map_cuda_kernel.cu  ← CUDA kernel
    ├── electric_force.cpp             ← CPU 反向
    ├── electric_force_cuda.cpp
    └── electric_force_cuda_kernel.cu
```

**运行时按张量所在设备分派**（`electric_potential.py` backward）：

```python
if grad_pos.is_cuda:
    output = -electric_potential_cuda.electric_force(...)
else:
    output = -electric_potential_cpp.electric_force(...)
```

**编译期按 CUDA 可用性门控**：

```python
if configure.compile_configurations["CUDA_FOUND"] == "TRUE":
    import dreamplace.ops.electric_potential.electric_potential_cuda as electric_potential_cuda
```

所以 **DREAMPlace 天然支持 CPU-only 构建**——README 明确写着
*"If it is installed on a machine without GPU, only CPU support will be enabled with multi-threading."*
`CMakeLists.txt:86` 的 `if(CUDA_FOUND)` 是全局门控。

C++ 侧通过 pybind11 暴露（`ops/hpwl/src/hpwl.cpp:86`）：

```cpp
PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("forward", &DREAMPLACE_NAMESPACE::hpwl_forward, "HPWL forward");
}
```

### 2.4 手写 backward，不用自动微分

这一点很关键，容易被误解。DREAMPlace 虽然跑在 PyTorch 上，但**并不依赖 autograd 求导**——
每个算子的 `backward()` 都是**手工推导并实现的解析梯度**。

`ElectricPotentialFunction.backward()` 直接返回 `-electric_force(...)`，也就是**密度力取负**。
这恰好把我们在
[01 附录 A 第 5 条](01-架构设计与技术选型.md)
里标注的那个符号约定坑，用 autograd 的契约固定死了：
框架规定 `backward` 必须返回 \(\partial\text{obj}/\partial\text{pos}\)，
而 \(\nabla N = -q\xi\)，所以代码里写 `-electric_force` 才对。

**启示**：符号约定不能靠注释，要靠**接口契约**强制。我们的 `PoissonSolver` 应当在文档和函数名上
明确区分"返回的是电场 \(\xi\)"还是"返回的是梯度 \(\nabla N\)"。

### 2.5 `obj_and_grad_fn` 是优化器与目标函数的唯一契约

`NesterovAcceleratedGradientOptimizer.__init__` 的签名（`NesterovAcceleratedGradientOptimizer.py:23`）：

```python
def __init__(self, params, lr=required, obj_and_grad_fn=required,
             constraint_fn=None, use_bb=True):
```

优化器**只知道**三件事：待优化参数、一个 `(obj, grad) = f(pos)` 的回调、一个把解投影回可行域的
`constraint_fn`。它完全不知道什么是线长、什么是密度、什么是 filler。

这带来的直接好处是**优化器可以随便换**——`NonLinearPlace.py:173-213` 里支持
Nesterov、Adam、AggMo、QHAdam、Yogi、RAdam、AdaBelief、AdaBound、Adafactor、DiffGrad、
NovoGrad，以及 FR/PRP/HS/CD/LS/DY/HZ 七种非线性共轭梯度。做优化器对比实验的成本接近于零。

> 与 easyPlace 对比：easyPlace 的优化器接口是 `getPosition()` / `setPosition()` /
> `getGradient()` 三个按值传递巨型 vector 的方法（见
> [02 §3.3](02-baseline%20代码评估.md)），
> 每轮迭代白白拷贝约 50 MB。**同样是"解耦优化器"，接口设计的差别直接决定了性能。**

### 2.6 三层嵌套调度

`NonLinearPlace.py` 的停止判据分三层（`:286`、`:329`、`:358`）：

```
Lgamma_stop_criterion    ← 外层：γ（线长平滑度）调度
  └ Llambda_stop_criterion   ← 中层：density_weight（λ）调度
      └ Lsub_stop_criterion      ← 内层：实际的梯度下降步
```

比 easyPlace 的单层循环（`nesterov.hpp:56` 只看 overflow 和 iter_count）精细得多，
收敛更稳。这是我们文档 §8 演进节奏里没有考虑到的层次。

### 2.7 GPU 的确定性问题（我们文档遗漏的点）

`electric_potential.py` 的参数列表里有 `deterministic_flag`，`ops/` 下还专门有
`weighted_average_wirelength_atomic` / `hpwl_atomic` 这类"原子加法版本"的独立实现。

原因是：**GPU 上多线程原子浮点加法的累加顺序不确定，浮点加法不满足结合律，
因此同样的输入两次运行可能得到不同的结果。** DREAMPlace 为此提供了确定性模式（牺牲性能换可复现）。

这与我们设计文档 §2.2 强调的"**可脚本化、可复现、可对比**"直接相关——
如果将来上 GPU，可复现性不是免费的，必须显式设计。

---

## 3. OpenROAD `gpl` 的 Backend 抽象（最值得直接照搬的部分）

OpenROAD 走了一条和 DREAMPlace 不同、但对我们**更有参考价值**的路：**纯 C++，
通过 Strategy 模式把 CPU 与 GPU 实现隔离在接口之后。**

`src/gpl/src/` 下有四个 Backend 抽象头文件：

```
fftBackend.h                  ← FFT / 泊松求解
densityGradientBackend.h      ← 密度梯度
wirelengthGradientBackend.h   ← 线长梯度
hpwlBackend.h                 ← HPWL
```

以及一个 `gpu/` 子目录存放对应的 GPU 实现（用 **Kokkos** 而非裸 CUDA，以获得可移植性）。

### 3.1 接口长什么样

```cpp
// OpenROAD/src/gpl/src/fftBackend.h

// POD view over a 2D bin grid laid out as a single row-major float buffer
struct BinGridSpan
{
  float* data = nullptr;
  int bin_cnt_x = 0;
  int bin_cnt_y = 0;
  float& operator()(int x, int y) { return data[x * bin_cnt_y + y]; }
};

class FftBackend
{
 public:
  virtual void solve(BinGridSpan density,
                     BinGridSpan phi,
                     BinGridSpan field_x,
                     BinGridSpan field_y) = 0;
  virtual const char* name() const = 0;
};

// 工厂：ENABLE_GPU 构建且运行时选择 GPU 时返回 GpuFftBackend，否则 CpuFftBackend
std::unique_ptr<FftBackend> makeFftBackend(const BackendContext& ctx);
```

### 3.2 为什么这对我们极其重要

**第一，它几乎逐字印证了我们设计文档的决策 1。** 我们写的是：

```cpp
class PoissonSolver {
    void solve(const float* density, float* ex, float* ey, float* potential);
};
```

OpenROAD 写的是 `solve(density, phi, field_x, field_y)`，全部是**扁平 row-major float 缓冲**。
两者形状完全一致。这说明"数值内核只吃裸数组、不认识单元"这个决策是被工业级实现独立验证过的。

**第二，它示范了如何"零成本预留 CUDA 后端"。** 该头文件的注释把设计意图写得很清楚：

> *"This header is plain C++ — no Kokkos, no preprocessor branches — so `fft.h` can hold a
> `std::unique_ptr<FftBackend>` member without learning anything about the GPU build."*

也就是说：**调用方永远只看到纯 C++ 接口，GPU 的存在与否完全不泄漏。**
先写 `CpuFftBackend`，将来加 `GpuFftBackend` 只是新增一个 `.cpp`，
调用方一行都不用改。

**第三，扁平缓冲本身就是 GPU-ready 的。** `BinGridSpan` 这种 `float* + 维度` 的形式，
既能给 CPU 循环用，也能直接 `cudaMemcpy` 上显存。相反，easyPlace 的
`vector<vector<Bin_2D*>>` 是**根本没法传给 GPU 的**——26 万个散落的堆对象无法一次拷贝。

> **这条是本次评估最重要的收获**：我们不需要在"现在上 CUDA"和"永远不上 CUDA"之间二选一。
> 只要数据布局是扁平的、内核接口是裸指针的，CUDA 的门就一直开着。

---

## 4. 与现有设计文档的对照

### 4.1 被验证的决策（无需修改）

| 我们的决策 | DREAMPlace / OpenROAD 的对应 | 结论 |
| --- | --- | --- |
| **决策 1** 数值内核不认识"单元" | OpenROAD `FftBackend::solve(BinGridSpan...)`；DREAMPlace 算子只吃 tensor | ✅ 强验证，两方独立一致 |
| **决策 3** SoA 扁平数组 | DREAMPlace 纯 SoA + CSR；OpenROAD `BinGridSpan` | ✅ 强验证 |
| **决策 4** 可观测性作为子系统 | `EvalMetrics.py` 逐迭代记录 | ✅ 验证 |
| **决策 5** 配置文件化 | `params.json` 完全配置驱动 | ✅ 验证 |
| **决策 6** 检查器独立 | DREAMPlace 有独立的 `ops/legality_check` 算子 | ✅ 验证（easyPlace 恰恰没有） |
| 阶段可独立启停 | DREAMPlace 分阶段配置；OpenROAD 分 pass | ✅ 验证 |

### 4.2 冲突点（需要决策）

#### 冲突 1【硬】技术栈：C++ 单体 vs Python 编排 + C++ 算子

我们文档 §2.3 规定交付物是 `libsuperplace`（静态库）+ `superplace`（C++ CLI）。
若采用 DREAMPlace 架构，形态将变成 **Python 包 + pybind11 扩展模块**，
`libsuperplace` 不复存在，CLI 变成 Python 脚本。

**影响面**：§2.3 交付物、§5.1 目录结构、§5.2 `PlacementStage` 接口、§9 依赖清单全部要重写。

**判断**：**不采纳 Python 栈**。理由见 §5.3。

#### 冲突 2【硬】GUI 接入方式

我们文档 §7.5 规划的是 ImGui/Qt 直接链接 `libsuperplace`，布局器跑在 worker 线程，
通过**双缓冲位置数组 + 原子指针交换**给 GUI 提供快照。

若核心是 Python/PyTorch，这套同进程共享内存的方案不再适用——GUI 要么改用
PySide6/PyQtGraph（Python 内），要么走进程间通信。

**判断**：随冲突 1 一并解决。保持 C++ 核心，§7.5 方案继续有效。

#### 冲突 3【硬，已修订】决策 3 的"AoS + SoA 双轨"应改为纯 SoA + 句柄视图

我们文档决策 3 原本写的是"对象模型给人看，扁平数组给循环跑，进入优化循环前扁平化"。
DREAMPlace 的做法则是**从解析阶段就直接构造扁平数组，全程没有 AoS**。

**判断（修订后）：采纳纯 SoA，废弃双轨折中。**

原方案的问题在于**维护两份数据**——需要同步、需要转换代码、需要约定"哪份是权威"，
这三样都是真实的复杂度与 bug 来源，而换来的只是"对象写起来顺手"。

正确的解法是第三条路：**存储 100% 是 SoA，另外提供一层零成本的句柄视图（handle / view）。**

```cpp
// 唯一存储，全部扁平数组
struct PlaceDB {
    std::vector<float>   node_x, node_y, node_w, node_h;
    std::vector<uint8_t> node_flags;          // isMacro / isFixed / isFiller 位标志
    std::vector<std::string> node_name;       // 仅解析与输出使用，不进热路径
    // 超图 CSR
    std::vector<int>   flat_net2pin,  net2pin_start;
    std::vector<int>   flat_node2pin, node2pin_start;
    std::vector<int>   pin2node;
    std::vector<float> pin_offset_x, pin_offset_y;
};

// 句柄视图：一个索引 + 一个引用，无存储、无堆分配、全 inline
class ModuleRef {
    const PlaceDB& db_;
    int i_;
public:
    ModuleRef(const PlaceDB& db, int i) : db_(db), i_(i) {}
    float x()       const { return db_.node_x[i_]; }
    float y()       const { return db_.node_y[i_]; }
    float width()   const { return db_.node_w[i_]; }
    float height()  const { return db_.node_h[i_]; }
    bool  isMacro() const { return db_.node_flags[i_] & F_MACRO; }
    PinRange pins() const { return db_.pinsOf(i_); }   // CSR 切片
};
```

`db.node(i).width()` 与 `db.node_w[i]` **访问同一块内存**，没有副本、没有同步问题。
解析、绘图、调试、答辩演示用视图写得可读；热循环直接吃 `node_w.data()`，零指针间接。

**纯 SoA 的三个代价，均可接受：**

| 代价 | 说明 | 缓解 |
| --- | --- | --- |
| 解析器需两趟构建 CSR | 先统计每个 net 的 pin 数填 `start` 数组做前缀和，再填 `flat` 数组 | 约 50 行一次性代码；DREAMPlace 与 OpenROAD 均如此 |
| 调试器无法展开"一个完整对象" | gdb 里悬停下标不会显示全部属性 | 句柄视图 + `dumpNode(i)` 打印函数，十余行 |
| 与课程资料参考结构不一致 | 资料 §1 给出的是 AoS 风格 | 资料四处均写"**参考**属性定义"，全篇无强制要求；且这一偏离可作为设计分析写入报告（见下） |

**关于课程合规**：材料 3 的 `参考属性定义` 出现 4 次，全篇未出现任何对数据结构的强制性措辞
（`必须` 的四处分别针对 site 高度、溢出取正部、Y 轴翻转、单元入行，均为算法/格式约束）。
真正可验证的是 §2 数据对照的统计数字。

进一步说，**这一偏离本身是加分项**：报告中写明"资料给出的 AoS 参考结构在 21 万单元规模下
cache 有效利用率仅约 12.5%（`Module` 约 144 字节，热路径只需 16 字节，且 `w/h` 与 `x/y`
分属不同 cache line），故改用 SoA + CSR，实测提速 N 倍"——这是一段有分量的设计分析，
比照抄参考结构更能体现设计能力。

**额外收益**：纯 SoA 是**上 GPU 的物理前提**。`vector<vector<Bin_2D*>>` 那种结构
根本无法传给 GPU（26 万个散落堆对象无法一次拷贝），而扁平数组可以直接 `cudaMemcpy`。
所以这条决策同时服务于 §5 的 CUDA 路线。

#### 冲突 4【新增】可复现性与 GPU 的矛盾

文档 §2.2 强调产出"可复现"，但未考虑 GPU 原子浮点加法的不确定性。

**判断**：**补充说明**。CPU + OpenMP 路线下，只要密度累加用 per-thread 局部网格 + 固定顺序规约，
就能保证逐位可复现。若将来加 GPU 后端，需明确标注该后端不保证逐位一致，并提供确定性开关。

### 4.3 应当补充进设计文档的

| 补充项 | 来源 | 说明 |
| --- | --- | --- |
| **`obj_and_grad_fn` 契约** | DREAMPlace | 在 `PlacementStage` 之下再加一层：优化器与目标函数之间只通过 `(obj, grad) = f(pos)` 通信，使优化器可插拔 |
| **Backend Strategy 模式** | OpenROAD | 数值内核用抽象基类 + 工厂，为 CUDA/OpenMP 多后端预留位置，且不污染调用方 |
| **三层嵌套调度** | DREAMPlace | γ 调度 / λ 调度 / 内层下降步分离，比单层循环收敛稳 |
| **符号约定用接口契约固定** | DREAMPlace | 不靠注释，靠函数命名与返回值定义强制区分 \(\xi\) 与 \(\nabla N\) |
| **确定性设计** | DREAMPlace | per-thread 局部网格 + 固定顺序规约 |

---

## 5. CUDA 路线可行性评估

### 5.1 环境实测：完全具备条件

| 项目 | 状态 |
| --- | --- |
| GPU | ✅ NVIDIA GeForce RTX 4060 Laptop，8 GB 显存 |
| 驱动 | ✅ 560.94（WSL2 直通） |
| CUDA Runtime | ✅ 12.6 |
| **CUDA Toolkit / nvcc** | ✅ **已安装**，`/usr/local/cuda/bin/nvcc`，release 12.6 V12.6.85 |
| PyTorch | ❌ 未安装（DREAMPlace 路线需要） |

**技术上没有任何障碍。** RTX 4060 的 FP32 性能不弱，而布局计算用的正是 float32
（DREAMPlace 配置中 `"dtype": "float32"`）。显存也不构成约束：250 万单元的位置数组约 20 MB，
1024×1024 bin 网格的若干场量合计约 30 MB，8 GB 绰绰有余。

### 5.2 课程要求 vs 项目目标

**课程任务书本身不考核性能**：

| 任务 | 要求 | GPU 能带来什么 |
| --- | --- | --- |
| 任务 3 | BookShelf 解析 | 无关 |
| 任务 4 | 三种初始布局 + **对比分析** | 间接（对比实验需反复跑，加速缩短迭代周期） |
| 任务 5 | 全局布局设计 | 只影响运行时间，不影响算法正确性 |
| 任务 6 | 可视化（CImg + GDSII） | 无关 |
| 任务 7 | 合法化（选做） | 无关 |

**但项目目标高于课程下限。** 本项目的既定目标包含**跑通工业级大规模 benchmark**
（ISPD2005/2006 最大 250 万单元、MMS 混合尺寸套件共 16 个设计），
这使得性能从"不考核项"变成"能否完成目标的约束条件"。规模分析见
[§6 Benchmark 调研](#6-benchmark-调研与规模规划)。

### 5.3 判断（已修订）：CUDA 是规划内的第二阶段目标

> **初版判断为"不建议作为主线"，现上调。** 初版的核心依据是"只需跑通 `test_data/`
> 中的小规模样例"，该前提已不成立。以下逐条复核初版的五条理由。

| 初版理由 | 复核结论 |
| --- | --- |
| ① 收益落在不考核的维度 | ⚠️ **部分失效**。课程不考核，但**项目目标考核**——250 万单元跑不动就是做不完 |
| ② 规模够不上 GPU 门槛 | ❌ **失效**。adaptec1 的 21 万确实够不上，但 bigblue4（218 万）、newblue7（251 万）够得上，见 §6.3 的耗时估算 |
| ③ 调试难度叠加在最危险模块上 | ✅ **仍然成立**，且是最重要的一条——它约束的是**顺序**，不是**是否做** |
| ④ CUDA 不在课程知识范围 | ⚠️ **弱化**。作为超出要求的工程实现，反而是答辩亮点；只要主线算法讲清楚，不构成"跑偏" |
| ⑤ 依赖复杂度不可控 | ❌ **不适用**。该条针对的是 DREAMPlace 的 PyTorch 全家桶；我们只用裸 CUDA 写单个算子，依赖仅 nvcc |

**修订后的判断**：

1. **CUDA 纳入规划**，作为第二阶段目标，而非"可能不做的加分项"。
2. **顺序不可颠倒**——先在 CPU 上把算法验证正确，再上 GPU。理由（③）依然成立：
   ePlace 的密度项是符号错误与缩放错误的重灾区
   （[02 §2.3](02-baseline%20代码评估.md) 的宏单元缩放不自洽就是实例），
   在算法未验证前引入 CUDA 等于同时求解两个未知数，出错时无法定位是数学错了还是 kernel 错了。
3. **SoA + 扁平缓冲从"性能优化"升级为"前置条件"**——它是 GPU 的物理入场券，
   必须从第一天就守住，事后重构的代价极高。

### 5.4 推荐路线：拿走思想，留下接口

**采纳（与 CUDA 无关，纯收益）：**

1. **全流程纯 SoA 扁平数组 + 句柄视图** —— 解析阶段直接构造扁平数组，不保留 AoS 副本；
   句柄视图仅提供可读的访问语法（详见 §4.2 冲突 3）。
2. **算子化划分** —— `wirelength` / `density` / `dct` / `hpwl` / `legality_check` 各自独立、
   各自可单测。目录结构直接模仿 DREAMPlace 的 `ops/`。
3. **`obj_and_grad_fn` 契约** —— 优化器与目标函数解耦，Nesterov / Adam / CG 可换。
4. **符号约定写进接口** —— 函数名与返回值定义明确区分电场与梯度。

**预留（零成本保留 CUDA 可能性）：**

5. **按 OpenROAD 的 Strategy 模式设计数值内核**：

```cpp
// numeric/poisson_backend.h —— 纯 C++，不含任何 CUDA/OpenMP 细节
struct BinGridSpan {
    float* data = nullptr;
    int nx = 0, ny = 0;
    float&       operator()(int x, int y)       { return data[x * ny + y]; }
    const float& operator()(int x, int y) const { return data[x * ny + y]; }
};

class PoissonBackend {
public:
    virtual ~PoissonBackend() = default;
    // 读 density，写 phi / field_x / field_y。四者共享同一网格维度。
    virtual void solve(BinGridSpan density, BinGridSpan phi,
                       BinGridSpan field_x, BinGridSpan field_y) = 0;
    virtual const char* name() const = 0;
};

// 工厂：默认返回 CpuPoissonBackend；ENABLE_CUDA 构建下可返回 CudaPoissonBackend
std::unique_ptr<PoissonBackend> makePoissonBackend(const BackendConfig& cfg);
```

第一阶段只实现 `CpuPoissonBackend`（Ooura FFT 或 FFTW），第二阶段新增
`CudaPoissonBackend.cu`，**调用方零改动**。

**不采纳：**

6. Python / PyTorch 技术栈 —— 保持纯 C++ 单体 + CLI 形态。
7. 第一阶段的 CUDA 实现 —— 推迟到 CPU 版本在 adaptec1 上验证正确之后。

### 5.5 CUDA 实施路径（第二阶段）

**首个 CUDA 算子选 DCT / 泊松求解**，理由：

- 计算最密集、最规整（512²~1024² 网格上的二维变换），GPU 收益最明显；
- 输入输出都是扁平 float 缓冲，接口最干净，一次 `cudaMemcpy` 搞定；
- 有**解析可验证的正确性判据**（高斯分布输入 → 电场方向、边界法向分量为 0、DC 消除），
  且 CPU 版可直接作为逐元素对拍基准，不存在"不知道对不对"的问题；
- 工作量可控（约 200~300 行 CUDA），失败也不影响主线。

**后续可扩展**（按收益排序）：密度统计 kernel（`binNodeDensityUpdate` 等价物）→
线长梯度 kernel（WA 模型）→ Nesterov 更新 kernel（纯 elementwise，最简单）。

**严禁**的做法是一上来就全流程 GPU 化——那会让整个项目的正确性验证失去抓手。

**确定性注意事项**：GPU 上多线程原子浮点加法的累加顺序不确定，浮点加法不满足结合律，
因此同一输入两次运行可能得到不同结果。DREAMPlace 为此专门提供 `deterministic_flag`
与整套 `*_atomic` 变体实现。我们的 CUDA 后端需明确标注**不保证与 CPU 后端逐位一致**，
对拍时应使用相对误差阈值（建议 1e-5）而非精确相等。

---

## 6. Benchmark 调研与规模规划

> 本章是技术路线决策所需的摘要。**完整调研（全部套件明细、已实测的下载直链、
> 目录约定、MMS 自行重建方案）见
> [04-Benchmark 调研与获取指南](04-Benchmark%20调研与获取指南.md)。**

### 6.1 ⚠️ 关键发现：现有数据无法验证混合尺寸布局

统计 `test_data/` 三个 benchmark 的**可移动宏单元**数量（判据：非 terminal 且高度 ≠ 行高 12）：

| benchmark | 总单元 | 固定终端 | **可移动宏** |
| --- | --- | --- | --- |
| adaptec1 | 210,904 | 543 | **0** |
| adaptec4 | 494,716 | 1,329 | **0** |
| thin1 | 3 | 1 | **0** |

**三个全为 0。** ISPD2005/2006 原版将所有宏单元固定，非 terminal 单元的高度一律等于行高，
即清一色标准单元。

这直接意味着课程材料 2 §3 明确规定的核心目标——"本设计重点为全局布局，该步骤采用**混合尺寸布局**
的方式进行，即宏单元和标准单元一起参与布局"——**用现有数据完全无法验证**。

> **旁证**：easyPlace 的 `main/ePlace_main.cpp:130` 是
> `if (placedb->dbMacroCount > 0 && !gArg.CheckExist("nomLG"))`，
> 因此在 adaptec1 / adaptec4 上 **mLG → FILLERONLY → cGP 三阶段流程从未被执行**。
> 它实现的 ePlace-MS 三阶段在这两个 benchmark 上是死代码。这也解释了
> 为何作者在宏单元密度缩放处留下一串 `?????`（`eplace.cpp:560`）——他无从测试。

**结论：MMS benchmark 不是"锦上添花"，而是验证课程核心目标的必需品。**

### 6.2 Benchmark 全景

**ISPD 2005 placement contest**（BookShelf 格式，宏单元固定，8 个设计）：

| 设计 | 对象数 | 可移动 | 线网数 | 引脚数 |
| --- | --- | --- | --- | --- |
| adaptec1 | 211,447 | 210,904 | 221,142 | 944,053 |
| adaptec2 | 255,023 | 254,457 | 266,009 | 1,069,482 |
| adaptec3 | 451,650 | 450,927 | 466,758 | 1,875,039 |
| adaptec4 | 496,045 | 494,716 | 515,951 | 1,912,420 |
| bigblue1 | 278,164 | 277,604 | 284,479 | 1,144,691 |
| bigblue2 | 557,866 | 534,782 | 577,235 | 2,122,282 |
| bigblue3 | 1,096,812 | 1,095,519 | 1,123,170 | 3,833,218 |
| **bigblue4** | **2,177,353** | 2,169,183 | 2,228,903 | **8,900,078** |

**ISPD 2006 placement contest**（增加密度目标与 scaled HPWL，密度目标 0.5~0.9，8 个设计）：

| 设计 | 对象数 | 设计 | 对象数 |
| --- | --- | --- | --- |
| adaptec5 | 843,128 | newblue4 | 646,139 |
| newblue1 | 330,474 | newblue5 | 1,233,058 |
| newblue2 | 441,516 | newblue6 | 1,255,039 |
| newblue3 | 494,011 | **newblue7** | **2,507,954**（10.1M pins） |

**MMS（Modern Mixed-Size）—— 本项目最需要的**：

- 由 Yan / Viswanathan / Chu（DAC 2009）从 ISPD2005/2006 派生：**将全部宏单元解放为可移动**，
  并将 I/O 对象尺寸置零，共 16 个设计；
- 是混合尺寸布局领域的标准 benchmark，ePlace-MS、RePlAce、DREAMPlace 及后续工作均用它评测；
- ⚠️ **爱荷华州立原站（`public.iastate.edu/~zijunyan`）已失效**；
- 目前可获取的重建版位于 IEEE DataPort，DOI **`10.21227/2n68-tx57`**，
  379.72 MB，BookShelf 格式，覆盖全部 16 个设计；
- DREAMPlace 仓库 `test/mms/` 已备好 16 个 json 配置（adaptec1-5、bigblue1-4、newblue1-7），
  说明数据格式路径与我们的解析器完全兼容。

**其他方向**（本项目暂不涉及，备查）：

| 套件 | 特点 |
| --- | --- |
| DAC2012 / ICCAD2012 superblue | 布线拥塞驱动，400k~2M |
| ISPD2015 | 详细布线驱动 |
| ISPD2018 / 2019 | LEF/DEF 格式；课程材料 3.3 讲解 DEF 所用的 `ispd19_test1` 即出自此 |
| TILOS / MacroPlacement | Ariane、MemPool、BlackParrot、NVDLA，现代工艺 LEF/DEF |

### 6.3 规模对性能的要求

以 250 万单元、约 500 轮 Nesterov 迭代粗估：

| 后端 | 单轮耗时（估） | 单设计总耗时 | MMS 全套 16 个 |
| --- | --- | --- | --- |
| CPU 单线程（easyPlace 式） | ~5 s | ~40 min | ~10 h |
| CPU 8 线程 + SoA | 0.3~1 s | 3~8 min | 1~2 h |
| GPU（RTX 4060，估 10~20X） | ~0.05 s | ~30 s | ~10 min |

> DREAMPlace README 宣称在 ISPD2005 上全局布局 + 合法化相对 RePlAce（多线程 CPU）
> 有 **>30X 加速**（Tesla V100）。RTX 4060 Laptop 弱于 V100，但布局用 float32，
> 4060 的 FP32 吞吐尚可，取 10~20X 为保守预期。

**判读**：CPU 8 线程下单设计 3~8 分钟是可接受的，但**参数调优需要反复跑数十轮 sweep**，
届时 1~2 小时/轮的全套耗时会成为真正的瓶颈。这是 CUDA 价值的主要来源。

### 6.4 获取优先级

| 优先级 | 套件 | 理由 |
| --- | --- | --- |
| **P0** | **MMS** | 唯一能验证混合尺寸布局（课程核心目标）的数据 |
| P1 | ISPD2005 全套 | 规模阶梯 211k → 2.18M，用于性能与稳定性验证 |
| P2 | ISPD2006 | 密度目标多样（0.5~0.9），验证 targetDensity 适应性 |

---

## 7. 对设计文档的修订建议

以下修订**已于 2026-09-09 全部执行**到 [01-架构设计与技术选型](01-架构设计与技术选型.md)：

| 章节 | 修订内容 | 状态 |
| --- | --- | --- |
| §3.1 架构图 | 数值内核层补充 Backend Strategy 抽象 | ✅ 已改 |
| §4 决策 3 | 改为**纯 SoA + CSR + 句柄视图**，不保留 AoS 副本 | ✅ 已改 |
| §4 新增决策 7 | **数值内核采用 Backend Strategy 模式**，附 `PoissonBackend` 接口示例 | ✅ 已加 |
| §4 新增决策 8 | **优化器通过 `obj_and_grad_fn` 契约解耦**，使 Nesterov/Adam/CG 可插拔 | ✅ 已加 |
| §5.1 目录结构 | `numeric/` 下区分 `*_backend.h`（接口）与 `cpu/`、`cuda/`（实现） | ✅ 已改 |
| §2.2 可复现性 | 补充确定性要求：per-thread 局部网格 + 固定顺序规约；GPU 后端不保证逐位一致 | ✅ 已改 |
| §8 演进节奏 | M3 补充三层嵌套调度；新增 M6（大规模 benchmark）与 M7（CUDA 泊松求解器） | ✅ 已改 |
| §9 依赖 | 记录 CUDA 12.6 + RTX 4060 已就绪；补充 benchmark 获取清单 | ✅ 已改 |

---

## 附：本次评估的关键证据索引

| 结论 | 证据位置 |
| --- | --- |
| DREAMPlace 纯 SoA | `DREAMPlace/dreamplace/PlaceDB.py:44-54` |
| 超图 CSR 扁平化 | `DREAMPlace/dreamplace/PlaceDB.py:61-69` |
| CPU/CUDA 运行时分派 | `DREAMPlace/dreamplace/ops/electric_potential/electric_potential.py` backward |
| CUDA 编译期门控 | `DREAMPlace/dreamplace/ops/electric_potential/electric_potential.py:34`；`CMakeLists.txt:86` |
| 手写解析梯度（非 autodiff） | 同上 backward 返回 `-electric_force(...)` |
| 优化器契约 | `DREAMPlace/dreamplace/NesterovAcceleratedGradientOptimizer.py:23` |
| 优化器可插拔 | `DREAMPlace/dreamplace/NonLinearPlace.py:173-213` |
| 三层嵌套调度 | `DREAMPlace/dreamplace/NonLinearPlace.py:286,329,358` |
| GPU 确定性问题 | `deterministic_flag` 参数 + `ops/*_atomic` 系列实现 |
| OpenROAD Backend 抽象 | `OpenROAD/src/gpl/src/fftBackend.h`（及 `densityGradient/wirelengthGradient/hpwl` 三个同构头） |
| OpenROAD GPU 实现 | `OpenROAD/src/gpl/src/gpu/`（Kokkos，非裸 CUDA） |
| 本机 CUDA 就绪 | `nvcc` release 12.6 V12.6.85；`nvidia-smi` RTX 4060 8GB |
| **现有 benchmark 无可移动宏** | `test_data/{adaptec1,adaptec4,thin1}/*.nodes` 统计：非 terminal 且高度 ≠ 12 的单元数均为 **0** |
| easyPlace 三阶段在现有数据上是死代码 | `easyPlace/main/ePlace_main.cpp:130` 的 `if (placedb->dbMacroCount > 0 && ...)` |
| DREAMPlace 支持 MMS | `DREAMPlace/test/mms/` 下 16 个 json 配置（adaptec1-5 / bigblue1-4 / newblue1-7） |
| DREAMPlace 加速比声明 | `DREAMPlace/README.md:6-7`：全局布局+合法化相对 RePlAce **>30X**（V100）；ABCDPlace 详细布局 ~16X |
| ISPD2005/2006 规模数据 | ISPD 官方竞赛页与 Nam（IBM）竞赛总结；MMS 出处 Yan/Viswanathan/Chu, DAC 2009 |
| MMS 重建版获取 | IEEE DataPort，DOI `10.21227/2n68-tx57`（原 iastate 站点已失效） |
