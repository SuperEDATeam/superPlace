# 08 · M5 Abacus 标准单元合法化：开发任务（交给同伴）

> 2026-11-09 建立。对象：同伴。对应课程任务 7（选做）、里程碑 M5。
>
> 配套阅读：[05 §1 铁律](05-开发执行规范.md)、[05 §4.1 阶段统一接口](05-开发执行规范.md)、
> [05 §5.6.1 Abacus](05-开发执行规范.md)、[05 §4.6 检查器约束](05-开发执行规范.md)；
> 论文 `paper/Abacus_Fast Legalization of Standard Cell Circuits with Minimal Movement.pdf`（Spindler 等，ISPD 2008）。
>
> **这是同伴第一次改算法侧代码（`src/`）。** 前端的规则是「只改 `gui/`」，本任务例外地开放了一小块算法侧代码，
> 范围见 [§2](#2-工作边界哪些能改哪些不能)。**范围之外的任何改动都要先与算法侧商量。**

## 目录

- [0. 结论速览](#0-结论速览)
- [1. 背景：合法化做什么、为什么要做](#1-背景合法化做什么为什么要做)
- [2. 工作边界：哪些能改，哪些不能](#2-工作边界哪些能改哪些不能)
- [3. 开发前准备：环境与数据](#3-开发前准备环境与数据)
- [4. 必须遵守的工程规则](#4-必须遵守的工程规则)
- [5. 接口：合法化阶段在流程中的位置](#5-接口合法化阶段在流程中的位置)
- [6. 算法规格](#6-算法规格)
- [7. 任务卡](#7-任务卡)
- [8. 验收标准](#8-验收标准)
- [9. 由算法侧负责的配套工作](#9-由算法侧负责的配套工作)
- [10. 容易踩的坑](#10-容易踩的坑)

---

## 0. 结论速览

| 问题 | 答案 |
| --- | --- |
| 做什么 | 新增一个合法化阶段 `lg`，用 Abacus 算法把全局布局后的**标准单元**挪进行里、对齐到 site、消除重叠，且总位移尽量小 |
| 不做什么 | 宏单元合法化（M3 的 mLG 已经做完）、详细布局（detailed placement）、合法性检查器（由算法侧另写，见 §9） |
| 改哪些代码 | 新增 6 个文件；另有 5 个已有文件只许按规定加几行。见 §2 |
| 怎么用 | `superplace-cli --aux <aux> --stage parse,init,gp,lg`。不写 `lg` 时，现有行为与结果**逐位不变** |
| 怎么算做完 | 三个设计上合法化后由 `superplace-check` 报 PASS，HPWL 增幅约 5% 以内，newblue1 合法化 10 秒以内，结果可复现。见 §8 |
| 工作量 | 约 5–7 人日 |
| 排期建议 | 在前端演示任务（06 文档 §9.1d、§9.1e）完成之后开始；演示优先 |

---

## 1. 背景：合法化做什么、为什么要做

**全局布局（GP，M3）的结果不是一个合法布局。** ePlace 把单元当作可以重叠的电荷，优化结束时：

- 单元之间仍有重叠：newblue1 结束时溢出率约 0.069，即约 7% 的单元面积落在超额区域；
- 单元的 y 没有对齐到行（row），x 没有对齐到 site 网格。

**合法布局**要求每个标准单元：完整落在某一行里、左边缘对齐到该行的 site 网格、与其他单元和固定物体（固定宏、终端）都不重叠、不越出 core。

ISPD 竞赛的评测与 ePlace 论文里报告的 HPWL，都是合法化之后的数值。我们目前报告的是 GP 阶段的 HPWL，
和这些数字直接比较会偏乐观。合法化之后会多出一个「合法化后 HPWL」，一般比 GP 的 HPWL 高几个百分点。

**Abacus** 是标准的标准单元合法化算法：按 x 坐标依次处理单元，为每个单元在附近几行里试放，
选总位移最小的那一行；行内用「簇合并」的二次规划，在不重叠的前提下让行内单元的位移平方和最小。算法规格见 §6。

**和现有代码的关系**：

| 阶段 | 做什么 | 状态 |
| --- | --- | --- |
| `parse` | 读 BookShelf | 已有 |
| `init` | 初始布局（二次布局等） | 已有 |
| `gp` | ePlace 全局布局；其中 mLG 子阶段已经把**宏**合法化并钉死（置 `F_FIXED`） | 已有（M3） |
| **`lg`** | **Abacus：只处理标准单元** | **本任务** |

---

## 2. 工作边界：哪些能改，哪些不能

### ✅ 新增文件（完全由你负责）

```
src/lg/abacus.h          Abacus 算法：行段构建、单元排序、试放、簇合并、site 对齐
src/lg/abacus.cpp
src/stages/lg_stage.h    合法化阶段：调用 Abacus，记录指标，打日志
src/stages/lg_stage.cpp
test/test_abacus.cpp     单元测试（手写小夹具，见 M5-4）
```

### ⚠️ 已有文件：只许按下面的方式加几行

| 文件 | 允许的改动 | 不允许 |
| --- | --- | --- |
| `src/stages/parse_stage.cpp` | `makeStage()` 里加一行 `if (name == "lg") return std::make_unique<LegalizeStage>();`，以及对应的 `#include` | 改其他阶段的注册 |
| `CMakeLists.txt`（根目录） | `SUPERPLACE_SOURCES` 列表里加 `src/lg/abacus.cpp`、`src/stages/lg_stage.cpp` 两行 | 改编译选项、依赖、其他目标 |
| `test/CMakeLists.txt` | 加一行 `sp_add_test(test_abacus)` | 改其他测试与数据解压逻辑 |
| `src/util/config.h`、`src/util/config.cpp`、`configs/default.json` | **只新增**以 `lg_` 开头的配置项（字段、`get("lg_...")`、默认值三处同步） | 改任何已有配置项的名字或默认值；**不要**把 `lg` 加进默认的 `stages` |
| `src/db/place_db.h` | 如需要，只新增两个派生写入函数 `setLlx(int i, float v)`、`setLly(int i, float v)`（按左下角设置中心坐标），见 §4 铁律 2 | 改任何已有字段与函数 |

### ⛔ 不要修改

```
src/gp/  src/init/  src/numeric/  src/io/  src/plot/     算法其他部分
src/lg/macro_sa.*                                        宏合法化（M3）
src/stages/ 下已有的三个阶段                              parse / init / gp
src/main.cpp  src/util/metrics.*  src/db/ 下其余内容
test/ 下已有的测试与 golden 数据
tools/                                                   检查器 superplace-check 由算法侧写（§9）
scripts/  gui/  configs/ 下已有配置项  work_report/
third_party/                                             不引入新的第三方库
```

### 需要先商量

- 任何「⚠️」表以外的改动，包括你认为必须改的已有代码（例如觉得 `PlaceDB` 缺字段、读入器对行的处理不对）；
- 新增任何依赖；
- 想并行化 Abacus（会影响可复现性，见 §4 铁律 7）。

**提交方式**：在分支 `feat/abacus` 上开发，提一个 PR。PR 的改动文件若超出本节列出的范围，默认打回。
PR 里不要附带构建产物，也不要往 `work_report/` 写文档；开发记录、实测数据写在 PR 描述里。

---

## 3. 开发前准备：环境与数据

**环境**：算法侧需要 FFTW、Eigen、libpng，安装与构建方法见根目录 `README.md`：

```bash
sudo apt install -y pkgconf libpng-dev libeigen3-dev libfftw3-dev ninja-build ccache
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure     # 开工前先确认全部通过
```

**数据**：

| 设计 | 来源 | 特点 | 用途 |
| --- | --- | --- | --- |
| `test_data/adaptec1`（ISPD2005） | 仓库自带压缩包，首次 `cmake` 配置时自动解压 | 21 万单元；**固定宏在 core 内部**，会把行切成多段 | 主力调试 |
| `test_data/mms_adaptec1`（MMS） | 同上 | 宏原本可移动，mLG 后被钉死 | 验收 |
| `benchmarks/ispd2006/newblue1` | **不在仓库里**；算法侧已线下发过原始压缩包 `benchmarks/_dl/newblue1.tar.gz`（7 MB），按下面的命令解压 | 33 万单元、64 个宏 | 验收与演示 |
| `test_data/thin1` | 仓库自带 | 很小 | 冒烟测试 |

**解压 newblue1**：压缩包里是 6 个各自 gzip 过的文件、没有目录层级，所以要先建好目录，再解包、再 `gunzip` 一次
（其他设计的解包方法见 [04 §6.2](04-Benchmark%20调研与获取指南.md)，`benchmarks/README.md` 也有说明）：

```bash
mkdir -p benchmarks/ispd2006/newblue1
tar xzf benchmarks/_dl/newblue1.tar.gz -C benchmarks/ispd2006/newblue1
gunzip -f benchmarks/ispd2006/newblue1/*.gz
./build/superplace-cli --aux benchmarks/ispd2006/newblue1/newblue1.aux --stage parse   # 校验能读通
```

解压后目录里应有 `newblue1.aux / .nets / .nodes / .pl / .scl / .wts` 六个文件，共约 55 MB；
结果目录下 `summary.json` 里的 `num_objects`、`num_nets`、`num_pins` 应为 330474、338901、1244342。

跑一次完整流程（adaptec1 上全局布局约 20 秒）：

```bash
./build/superplace-cli --aux test_data/adaptec1/adaptec1.aux --stage parse,init,gp,lg --no-plot --out results/lgdev
# 结果：results/lgdev/adaptec1/adaptec1.lg.pl，以及 summary.json
```

调试 Abacus 时不必每次重跑 GP：先跑一次 `--stage parse,init,gp` 得到 `adaptec1.gp.pl`，
之后可以在测试程序或临时工具里直接读入这个 `.pl` 再调 Abacus（临时工具不要提交）。

---

## 4. 必须遵守的工程规则

05 文档 §1 的 8 条铁律**违反即返工**。和本任务相关的是下面这些：

| 规则 | 对本任务意味着什么 |
| --- | --- |
| **阶段依赖**（`src/stage.h` 顶部注释） | `lg/` 与 `stages/lg_stage.*` **不得** `#include` `gp/`、`init/` 下的任何头文件。阶段之间只通过 `PlaceDB` 通信 |
| **铁律 2：坐标只有一种约定** | `PlaceDB` 的 `node_x / node_y` 是**中心坐标**。Abacus 天然用左下角思考，读可以用 `db.llx(i)`、`db.lly(i)`；写回时**不要**在 `lg/` 里写 `x - 0.5f * w` 这类换算，按 §2 的许可在 `place_db.h` 加 `setLlx / setLly` 两个派生函数，换算只在那里发生 |
| **铁律 3：纯 SoA** | 热循环直接用 `db.node_x.data()` 这类裸数组，不要用 `ModuleRef`，不要造 `struct Cell { x, y, w, h }` 的副本数组。Abacus 自己的簇、行段结构是算法状态，不算副本，可以用结构体 |
| **铁律 4：热路径不按值传容器** | 在逐单元循环里调用的函数，参数用指针 + 长度或引用，不要按值传 `std::vector` |
| **铁律 7：可复现** | 同输入同配置两次运行 `.pl` **逐字节相同**，且与线程数无关。单元排序要有确定的次序（x 相同时按下标）；不要用随机数；**保持串行** |
| **铁律 8：X/Y 不复制粘贴** | Abacus 本身只在 x 方向做行内优化，天然不对称，这条基本不涉及；若出现「x 一段、y 一段」结构相同的代码，抽成函数 |
| 05 §6 编码约定 | 命名空间 `sp`；类型 `PascalCase`、函数与变量 `camelCase`、私有成员尾下划线；`#pragma once`；位置用 `float`、HPWL 与面积累加用 `double`；注释只写「为什么」 |

**编译警告**：`-Wall -Wextra` 下新增代码零警告。

---

## 5. 接口：合法化阶段在流程中的位置

### 5.1 阶段类

```cpp
// src/stages/lg_stage.h
class LegalizeStage : public PlacementStage {
public:
    void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "lg"; }
};
```

`run()` 调用 `src/lg/abacus.h` 提供的算法函数，自身只负责前置检查、计时、指标与日志。
算法函数的签名自定，建议形如 `AbacusResult legalizeCells(PlaceDB& db, const AbacusOptions& opt);`，便于单测直接调用。

### 5.2 进入 `lg` 时 `PlaceDB` 的状态（gp 之后）

| 对象 | 下标范围 / 标志 | `lg` 怎么对待 |
| --- | --- | --- |
| 可移动标准单元 | `[0, numMovable)` 中 `!isMacro(i)` 的节点 | **要合法化的对象** |
| 宏 | 已被 mLG 置 `F_FIXED`（`isFixed(i)` 为真） | 当作障碍物，不移动 |
| 固定终端 | `[numMovable, numNodes)`，`F_FIXED` | 面积非零且落在 core 内的，当作障碍物 |
| 零面积 I/O 引脚 | `F_NI` | 忽略 |
| filler | `[numNodes, numNodes + numFillers)` | **忽略，不删除**（删除 filler 的函数在 `gp/` 里，`lg/` 不得引用） |
| 行 | `db.rows`，每项 `SiteRow{ly, height, lx, step, numSites}`，右端 `hx()` | 行的 site 宽度是 `step` |

**前置检查**：若存在「可移动且是宏但没有 `F_FIXED`」的节点（说明没跑 `gp` 就跑了 `lg`），
打印错误「lg 需要在 gp 之后运行（宏尚未合法化）」并返回，不做任何修改。

### 5.3 输出

- 改写可移动标准单元的 `node_x / node_y`（中心坐标），其余节点不动；
- `main.cpp` 会自动写出 `<设计>.lg.pl`、`lg.png`（开了绘图时），并在录制前端回放时打一个「lg 结束」的里程碑帧——**这些都不需要你改代码**；
- 通过 `sink.setSummary()` 写入下列指标（键名固定，前端与报告会用）：

| 键 | 含义 |
| --- | --- |
| `lg_hpwl_before` | 合法化前 HPWL（`db/hpwl.h` 的 `computeHPWL(db)`） |
| `lg_hpwl` | 合法化后 HPWL |
| `lg_cells` | 参与合法化的标准单元数 |
| `lg_failed` | 放不进任何行段的单元数（验收要求为 0） |
| `lg_disp_mean`、`lg_disp_max` | 单元中心位移的平均值与最大值（欧氏距离，设计坐标单位） |
| `lg_rows`、`lg_segments` | 行数与切分后的行段数 |
| `lg_time_ms` | 本阶段耗时 |
| `lg_selfcheck_overlaps` | 阶段末尾自检得到的重叠对数（见 M5-3；应为 0） |

- 日志：结束时一行 `SP_INFO`，格式如下（数值仅为示意）：
  `lg: N 个单元，HPWL A -> B (+x.x%)，位移 均值 m / 最大 M，失败 0，耗时 t s`。

### 5.4 新增配置项（可选）

只在确实需要时添加，以 `lg_` 开头，默认值写进 `configs/default.json`。建议最多这几个：

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `lg_row_search` | `0` | 每个单元最多试放的行数；0 表示按 §6.3 的剪枝自动决定 |
| `lg_cell_weight` | `"unit"` | 簇合并时单元的权重：`"unit"`（全为 1）或 `"area"` |

---

## 6. 算法规格

### 6.1 行段（segment）

每一行被 core 内的障碍物（§5.2 表中的宏与固定终端）切成若干**行段**，单元只能放在行段里：

1. 对每一行 `r`，收集 y 方向与 `[r.ly, r.hy())` 有**正面积重叠**、x 方向与 `[r.lx, r.hx())` 有重叠的障碍物；
2. 从 `[r.lx, r.hx())` 里扣掉这些障碍物的 x 区间，得到若干空闲区间；
3. 每个区间的两端**向内**对齐到 site 网格：左端 `lx + ceil((a - lx) / step) * step`，右端 `lx + floor((b - lx) / step) * step`；
4. 宽度小于最窄单元的区间丢弃。

### 6.2 单元处理顺序

所有待合法化单元按 GP 结果的**左下角 x** 升序排序；x 相同时按节点下标升序（保证可复现）。

### 6.3 试放与选行

对每个单元 `i`（GP 位置的左下角为 `(x'_i, y'_i)`）：

1. 按行与 `y'_i` 的距离 `|r.ly - y'_i|` 从近到远遍历行；
2. 在每一行里，取与 `x'_i` 最近、且剩余容量放得下 `w_i` 的行段，**试放**：把 `i` 追加到该行段的末尾并试算 PlaceRow（§6.4），
   得到 `i` 的试放位置 `(x_i, r.ly)`，代价 `c = (x_i - x'_i)² + (r.ly - y'_i)²`；
3. 记录代价最小的行段；**剪枝**：当某一行的 `(r.ly - y'_i)²` 已经不小于当前最小代价时，更远的行不可能更好，停止遍历；
4. 把 `i` 正式插入最佳行段并执行 PlaceRow；
5. 所有行段都放不下时，`lg_failed` 加一，单元保持 GP 位置（验收要求不出现这种情况）。

> 只用单元自己的位移作为代价，是论文的做法：试放的目的是选行，行内其余单元被推开的代价由 PlaceRow 在正式插入后统一优化。

### 6.4 PlaceRow：簇合并（论文 Algorithm 2）

行段里的单元按插入顺序（即 x 顺序）从左到右排列，相邻且紧挨着的单元构成一个**簇**。每个簇记录：

| 量 | 含义 |
| --- | --- |
| `e_c` | 簇内单元权重之和（权重默认 1） |
| `q_c` | 加权位置和 |
| `w_c` | 簇的总宽度 |
| `x_c` | 簇的左边缘位置 |

三个操作（`x'_i` 为单元 `i` 在 GP 结果中的左下角 x，`e_i` 为其权重）：

```
AddCell(c, i):     e_c += e_i;   q_c += e_i * (x'_i - w_c);   w_c += w_i
AddCluster(c, c2): e_c += e_c2;  q_c += q_c2 - e_c2 * w_c;    w_c += w_c2
Collapse(c):       x_c = q_c / e_c
                   把 x_c 夹到 [段左端, 段右端 - w_c]
                   若 c 的前一个簇 p 满足 x_p + w_p > x_c：
                       AddCluster(p, c)；删除 c；Collapse(p)
```

插入单元 `i` 时：若行段为空，或末尾簇的右端 `x_c + w_c <= x'_i`，就新建一个只含 `i` 的簇；
否则 `AddCell(末尾簇, i)`。然后对末尾簇执行 `Collapse`。

**试放必须不改动行段状态**：只有末尾簇（以及 Collapse 时被合并的前驱簇）会变，试放时把它们复制一份再算，算完丢弃。
这样每次试放的开销与被合并的簇数成正比，而不是与行内单元数成正比。**这是能否在 10 秒内跑完 newblue1 的关键。**

### 6.5 site 对齐与写回

所有单元处理完后，逐簇确定最终位置：

1. 簇左端对齐到 site 网格：`x_c = lx + round((x_c - lx) / step) * step`，再夹回行段范围内；
2. 簇内单元从 `x_c` 起依次紧挨排列；
3. 单元 y 取所在行的 `ly`；
4. 通过 `setLlx / setLly`（§4 铁律 2）写回中心坐标。

单元宽度应为 `step` 的整数倍（ISPD2005/2006 与 MMS 都满足）。开始时检查一遍，不满足的单元数写进日志，宽度向上取整到 site 后参与排布。

---

## 7. 任务卡

按 M5-1 → M5-5 的顺序做。每张卡完成后都应能编译、`ctest` 全部通过。

### M5-1 · 行段构建

- **做什么**：实现 §6.1。输入 `PlaceDB`，输出每一行的行段列表（左端、右端，均已对齐 site）。
- **注意**：障碍物与行在 y 方向只「贴边」（例如宏的下边缘正好等于行的上边缘）不算重叠；
  ISPD2005 adaptec1 的固定宏在 core 内，正好用来检查切分结果。
- **验收**：单测里构造「一行 + 中间一个障碍物」，得到两段，且两端都落在 site 网格上；
  在 adaptec1 上打印 `lg_rows` 与 `lg_segments`，行段数明显多于行数。

### M5-2 · Abacus 核心：试放、选行、簇合并

- **做什么**：实现 §6.2–§6.4。
- **注意**：先写一个不做试放优化的朴素版本，用小夹具对拍正确性；再按 §6.4 的「只复制末尾簇」改成快速版本，两者结果必须逐位相同。
- **验收**：M5-4 的单测全部通过；adaptec1 上合法化耗时在秒级（目标量级，不是硬指标；硬指标见 §8 的 newblue1 10 秒）。

### M5-3 · `lg` 阶段接入与自检

- **做什么**：实现 `LegalizeStage`（§5.1），按 §2 注册阶段、加入构建；前置检查（§5.2）、§6.5 的写回、§5.3 的指标与日志。
- **自检**：阶段末尾在每个行段内按 x 顺序检查相邻单元是否重叠、是否对齐 site，把结果写进 `lg_selfcheck_overlaps` 并打日志。
  这只是开发期的快速自检，**不能代替**独立检查器（§9）。
- **验收**：`--stage parse,init,gp,lg` 在 thin1 与 adaptec1 上跑通，生成 `<设计>.lg.pl`，自检为 0；
  不写 `lg` 时，`<设计>.gp.pl` 与 `metrics.csv` 和改动前逐字节相同。

### M5-4 · 单元测试 `test/test_abacus.cpp`

参照 `test/test_macro_sa.cpp` 的写法：手写几十个节点的小 `PlaceDB` 夹具，用 `test/test_util.h` 的 `CHECK_EQ` 等断言，不读大设计。至少覆盖：

| 场景 | 断言 |
| --- | --- |
| 一行内两个单元完全重叠 | 合法化后紧挨、关于原位置对称（单位权重时位移相等） |
| 行段中间有障碍物 | 单元不落在障碍物上；被挤到障碍物两侧 |
| 一行放满 | 多出的单元进入相邻的行，选的是位移更小的那一行 |
| site 对齐 | 所有单元左边缘都在 site 网格上 |
| 越出 core 的单元 | 被拉回最近的合法位置 |
| 固定节点、filler、零面积引脚 | 位置逐位不变 |
| 放不下 | 总宽度超过所有行段容量时，`lg_failed` 正确计数且不崩溃 |
| 可复现 | 同一夹具运行两次，所有坐标逐位相同 |
| 前置检查 | 存在未钉死的可移动宏时，不修改任何坐标 |

- **验收**：`ctest` 全部通过；在 AddressSanitizer + UBSan 构建下运行 `test_abacus` 无报告。

### M5-5 · 实测与调优

- **做什么**：在 §3 的三个设计上跑完整流程，按 §8 记录数据；必要时调整选行策略（如 `lg_row_search`）与权重（`lg_cell_weight`）。
- **记录**：每个设计的 `lg_hpwl_before`、`lg_hpwl`、增幅、位移均值与最大值、`lg_failed`、耗时，以及检查器结果，写在 PR 描述里。
- **验收**：§8 全部满足。

---

## 8. 验收标准

| 项 | 标准 |
| --- | --- |
| 范围 | PR 的改动文件全部在 §2 的范围内 |
| 构建 | `-Wall -Wextra` 下新增代码零警告；`ctest` 全部通过（含 `test_abacus`） |
| 不影响现有结果 | 不写 `lg` 时，adaptec1 的 `adaptec1.gp.pl` 与 `metrics.csv` 和 `main` 分支逐字节相同 |
| 合法性 | adaptec1、MMS adaptec1、newblue1 三个设计的 `<设计>.lg.pl`，由 `superplace-check` 报 **PASS**（零重叠、全部在行内、全部对齐 site、无越界） |
| 失败数 | 三个设计的 `lg_failed` 都为 0 |
| 线长 | `lg_hpwl` 相对 `lg_hpwl_before` 的增幅约 5% 以内。超过时不算不通过，但要在 PR 里报告数字并和算法侧讨论 |
| 速度 | newblue1 的 `lg_time_ms` 不超过 10 秒（Release 构建，单线程即可） |
| 可复现 | 同一设计连跑两次，`<设计>.lg.pl` 逐字节相同；`--threads 1` 与默认线程数的结果逐字节相同 |
| 内存安全 | AddressSanitizer + UBSan 下 `test_abacus` 无报告 |

---

## 9. 由算法侧负责的配套工作

这些**不由你来做**，列出来是为了说明分工与时间点：

| 项 | 说明 | 时间 |
| --- | --- | --- |
| **检查器 `superplace-check`**（`tools/check/`） | 05 §4.6 要求检查器**独立实现**，不能与合法化器共用任何几何判定代码，也不应由写合法化器的同一个人来写——共享代码会共享 bug，检查就失去意义。用法与判定口径见下文 | ✅ 已交付（2026-11-09） |
| newblue1 数据 | 已线下发过 `benchmarks/_dl/newblue1.tar.gz`，同伴按 §3 解压即可；若压缩包丢失再重发 | 已完成 |
| 是否把 `lg` 加入默认流程与演示 | 合入后由算法侧决定是否改 `configs/default.json` 的 `stages`、`scripts/viz.sh` 与 06 §3.10 的命令行 | 合入后 |
| 前端显示 `lg` 阶段 | 回放里会自动多出一个「lg 结束」的里程碑帧；阶段配色等由前端任务处理 | 合入后 |

开发期用 M5-3 的自检与 M5-4 的单测自测；**最终验收以检查器为准**。

### 9.1 检查器用法与判定口径

```bash
./build/superplace-check <design.aux> [placement.pl] [--examples N] [--eps E] [--max-pairs N]
# 例：./build/superplace-check test_data/adaptec1/adaptec1.aux results/lgdev/adaptec1/adaptec1.lg.pl
```

不给 `.pl` 时检查 `.aux` 自带的那份。退出码 0 = PASS、1 = FAIL、2 = 参数或输入错误。输出示例（newblue1 的 GP 结果，未合法化）：

```
Nodes:          330137 movable (330073 cells, 64 macros), 337 fixed, 0 ignored
Rows:           930 subrows, row height 12, core (609, 616)-(11783, 11776)
Overlap:        703979 pairs, 2.43458e+07 area
                单元-单元 698425，单元-宏 5554，单元-固定 0，宏-宏 0，宏-固定 0
Out of core:    0
Off-row:        329849
Off-site:       224
HPWL:           82274082.08
Result:         FAIL
```

FAIL 时会列出每类违规的前几个例子（节点名与坐标），`--examples` 控制个数。判定口径：

| 项 | 规则 |
| --- | --- |
| 对象分类 | 只看 `.nodes`：`terminal` / `terminal_NI` 是固定物体，其余是可移动节点。`.pl` 里的 `/FIXED` 不作数（superPlace 写出时会给 mLG 钉死的宏也加 `/FIXED`） |
| 标准单元与宏 | 可移动节点高度不超过行高的是标准单元，否则按宏处理 |
| Off-row | 标准单元：底边要落在某个子行上，且整体在该子行的水平范围内。宏：底边要落在某个子行的 y 上 |
| Off-site | 在行上的节点，左边缘要对齐所在子行的 site 网格（`(x - 子行 lx) / site` 为整数） |
| Out of core | 可移动节点不得越出全部子行的外包矩形 |
| Overlap | 只统计「可移动-可移动」与「可移动-固定」，固定物体之间的重叠是输入数据自带的，不计。只碰边不算重叠 |
| 忽略 | `terminal_NI` 与零面积节点不参与任何几何检查 |
| HPWL | 引脚 = 节点中心 + 按 `.pl` 方向（N/S/FN/FS 等）变换后的 `.nets` 偏移；度数小于 2 的 net 不计 |
| 容差 | 坐标比较容差 `1e-3`（`--eps` 可改） |

**已做的验证**：手写小设计上的 8 个用例已进 `ctest`（`ctest -L check`）；
newblue1 的 GP 结果上，检查器算出的 HPWL（82274082.08）与引擎的 `gp_hpwl`（82274082.07）一致，两份实现完全独立；
用一份贪心装箱得到的 newblue1 合法布局判为 PASS，再故意改坏几个单元，各类计数与改动一一对应；
全部单元堆在一点的输入布局，重叠数到 2000 万对即停止计数，0.5 秒给出结论。newblue1 规模的检查耗时约 0.7 秒。

> 合法化器的 HPWL 增幅可以拿检查器的 HPWL 输出核对：检查器对同一份 `.pl` 算出的 HPWL，应与 `lg_hpwl` 只差浮点舍入。

---

## 10. 容易踩的坑

1. **中心坐标与左下角混用。** `node_x` 是中心，Abacus 的公式全部是左下角。只在读取（`llx()`）和写回（`setLlx()`）两处换算，算法内部统一用左下角。
2. **试放改动了行段状态。** 试放必须在副本上算；否则试过的每一行都留下了这个单元，结果完全错乱。朴素版本与快速版本逐位对拍是最有效的检查。
3. **只对齐了簇的左端，忘了簇内单元。** 簇左端对齐 site 后，簇内单元依次紧挨排列；单元宽度若不是 site 的整数倍，后面的单元就会偏离网格（见 §6.5）。
4. **行段两端对齐方向反了。** 左端向上取整、右端向下取整，行段只会变窄；反过来会让单元压到障碍物上。
5. **障碍物「贴边」被当成重叠。** y 方向只是边界相等时不应切分行，否则会凭空少掉整行容量。
6. **把 filler 当成单元。** filler 在 `[numNodes, numNodes + numFillers)`，全部忽略；它们数量与单元同量级，误纳入会让耗时翻倍、结果错乱。
7. **排序不稳定导致结果不可复现。** x 相同的单元要按下标排序；`std::sort` 不保证稳定，比较函数里要显式加上下标。
8. **用 `double` 累加 `q_c` 才稳。** 簇很长时 `q_c` 是大量坐标的加权和，`float` 会丢精度，导致朴素版与快速版对不上。
