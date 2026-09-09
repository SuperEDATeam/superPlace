# Benchmark 调研与获取指南

> 目的：为本项目梳理可用于测试的工业级 / 论文来源大规模 benchmark，
> 明确各套件的格式、规模、适用性与获取途径。
>
> 所有下载链接均于 **2026-09-09 实测验证**（HTTP HEAD），标注了实际状态与体积。
>
> 相关文档：[01-架构设计与技术选型](01-架构设计与技术选型.md) §9.4、
> [03-DREAMPlace 架构评估](03-DREAMPlace%20架构评估与技术路线选择.md) §6

---

## 目录

- [0. 结论速览](#0-结论速览)
- [1. 两个硬性筛选条件](#1-两个硬性筛选条件)
- [2. 当前数据的致命缺口](#2-当前数据的致命缺口)
- [3. BookShelf 系（可直接使用）](#3-bookshelf-系可直接使用)
- [4. LEF/DEF 系（需扩展解析器）](#4-lefdef-系需扩展解析器)
- [5. 规模与性能预估](#5-规模与性能预估)
- [6. 获取清单与验证结果](#6-获取清单与验证结果)
- [7. 采用建议与目录约定](#7-采用建议与目录约定)
- [附录：从 ISPD2005 自行重建 MMS](#附录从-ispd2005-自行重建-mms)

---

## 0. 结论速览

| 套件 | 格式 | 设计数 | 规模范围 | 可移动宏 | 直接可用 | 可获取性 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **MMS** | BookShelf | 16 | 211k–2.6M | ✅ **有** | ✅ | ⚠️ 需注册 | **P0** |
| **ISPD 2005** | BookShelf | 8 | 211k–2.18M | ❌ 全固定 | ✅ | ✅ 实测可下 | **P1** |
| **ISPD 2006** | BookShelf | 8 | 330k–2.51M | ❌ 全固定 | ✅ | ✅ 实测可下 | **P2** |
| **DAC 2012** | BookShelf | 10 | ~400k–2M | ❌ | ✅ | ⚠️ 需另找源 | P3 |
| **ICCAD 2015** | BookShelf + 时序 | 8 | ~400k–2M | ❌ | ✅ | ✅ Google Drive | P3 |
| ISPD 2015 | LEF/DEF | 8 | 中等 | — | ❌ | ✅ 实测可下 | 暂不 |
| ISPD 2018/2019 | LEF/DEF | 10 | 中等 | — | ❌ | ✅ 实测可下 | 暂不 |
| TILOS MacroPlacement | RTL + LEF/DEF | 6 | 18k–360k flop | ✅ **有** | ❌ 需转换 | ✅ GitHub | 暂不 |

**一句话建议**：先用一条命令拉下 ISPD2005 全套（103 MB，立刻获得 211k→2.18M 的完整规模阶梯），
同时想办法搞到 MMS（唯一能验证课程核心目标"混合尺寸布局"的数据）。

---

## 1. 两个硬性筛选条件

### 条件一：格式必须是 BookShelf

课程任务 3 只要求 BookShelf 解析器，我们的 `io/bookshelf_reader` 也只支持这一种。
LEF/DEF 需要另写一套解析器——LEF 含工艺层定义、宏几何、引脚端口，
DEF 含 COMPONENTS / NETS / SPECIALNETS / TRACKS，工作量不小。

因此本文把 benchmark 分为"BookShelf 系（可直接吃）"与"LEF/DEF 系（需扩展）"两类。

### 条件二：最好含可移动宏单元

课程材料 2 §3 明确规定："本设计重点为全局布局，该步骤采用**混合尺寸布局**的方式进行，
即宏单元和标准单元一起参与布局。"

若 benchmark 中所有宏都是固定的，则宏单元密度缩放、宏单元合法化（模拟退火）、
mGP/cGP 阶段切换等代码路径**永远不会被执行**——跑再多设计也验证不了课程的核心目标。

---

## 2. 当前数据的致命缺口

统计 `test_data/` 三个 benchmark 的可移动宏单元数
（判据：`.nodes` 中非 `terminal` 且高度 ≠ 行高 12）：

| benchmark | 总单元 | 固定终端 | **可移动宏** |
| --- | --- | --- | --- |
| adaptec1 | 210,904 | 543 | **0** |
| adaptec4 | 494,716 | 1,329 | **0** |
| thin1 | 3 | 1 | **0** |

**三个全为 0。** ISPD2005/2006 原版将所有宏单元固定，非 terminal 单元的高度一律等于行高，
即清一色标准单元。

> **旁证**：参考实现 easyPlace 的 `main/ePlace_main.cpp:130` 是
> `if (placedb->dbMacroCount > 0 && !gArg.CheckExist("nomLG"))`，
> 因此在 adaptec1 / adaptec4 上 **mLG → FILLERONLY → cGP 三阶段流程从未被执行过**。
> 它实现的 ePlace-MS 三阶段在这两个 benchmark 上是死代码——这也解释了作者为何在
> 宏单元密度缩放处留下一串 `?????`（`eplace.cpp:560`），他根本无从测试。

**结论：MMS 不是"锦上添花"，而是验证课程核心目标的必需品。**

---

## 3. BookShelf 系（可直接使用）

### 3.1 MMS（Modern Mixed-Size）—— 优先级最高

**定位**：混合尺寸布局领域的事实标准 benchmark。ePlace-MS、RePlAce、DREAMPlace
及后续所有混合尺寸工作均以它评测。

**来源**：J. Z. Yan, N. Viswanathan, C. Chu，
*"Handling complexities in modern large-scale mixed-size placement"*, DAC 2009, pp. 436–441.

**构造方法**：在 ISPD2005/2006 基础上做两处修改——

1. **将全部宏单元从固定改为可移动**（去掉 `terminal` 标记与 `/FIXED`）；
2. 将所有 I/O 对象的尺寸置零。

**内容**：16 个设计，沿用 BookShelf 格式，设计名与 ISPD2005/2006 一致
（adaptec1-5、bigblue1-4、newblue1-7）。

**获取**：

| 途径 | 状态 | 说明 |
| --- | --- | --- |
| 爱荷华州立原站 `public.iastate.edu/~zijunyan` | ❌ **已失效** | 论文中给出的官方地址 |
| IEEE DataPort，DOI `10.21227/2n68-tx57` | ⚠️ 需注册 | 重建版，379.72 MB，BookShelf 格式，覆盖全部 16 个设计 |
| 自行从 ISPD2005 重建 | ✅ 可行 | 见[附录](#附录从-ispd2005-自行重建-mms) |

> DREAMPlace 仓库 `~/DREAMPlace/test/mms/` 已备好 16 个 json 配置文件
> （`adaptec1.json` ⋯ `newblue7.json`），其中的 `target_density`、`num_bins_x/y`、
> `stop_overflow` 等参数可直接作为我们的参考基线。

### 3.2 ISPD 2005 placement contest —— 规模阶梯主力

**定位**：布局领域被引用最多的 benchmark 套件，来自 IBM 的真实 ASIC 设计。

**特点**：8 个设计，21 万 → 218 万单元，宏单元全部固定。
adaptec2/adaptec3 含大型固定块，bigblue3 含可移动宏（原版中亦被固定）。

| 设计 | 对象数 | 可移动 | 线网数 | 引脚数 | 密度 |
| --- | --- | --- | --- | --- | --- |
| adaptec1 | 211,447 | 210,904 | 221,142 | 944,053 | 75.71% |
| adaptec2 | 255,023 | 254,457 | 266,009 | 1,069,482 | 78.59% |
| adaptec3 | 451,650 | 450,927 | 466,758 | 1,875,039 | 74.53% |
| adaptec4 | 496,045 | 494,716 | 515,951 | 1,912,420 | 62.67% |
| bigblue1 | 278,164 | 277,604 | 284,479 | 1,144,691 | 54.19% |
| bigblue2 | 557,866 | 534,782 | 577,235 | 2,122,282 | 61.80% |
| bigblue3 | 1,096,812 | 1,095,519 | 1,123,170 | 3,833,218 | 85.65% |
| **bigblue4** | **2,177,353** | 2,169,183 | 2,228,903 | **8,900,078** | 65.30% |

**获取（两种方式，均已实测）**：

**方式 A —— 一次拿全（推荐）**

```bash
# DREAMPlace 维护者打包版，103 MB，实测 HTTP 200
wget http://www.cerc.utexas.edu/~zixuan/ispd2005dp.tar.xz
```

**方式 B —— ISPD 官方逐个下载**

⚠️ 注意 adaptec1 / adaptec3 与其余六个**不在同一目录**
（当年这两个是提前两个月发给参赛者的样例）：

```bash
BASE=https://www.ispd.cc/contests/05/ispd05-contest
wget $BASE/adaptec1.tar.gz          # 4 MB
wget $BASE/adaptec3.tar.gz
for d in adaptec2 adaptec4 bigblue1 bigblue2 bigblue3 bigblue4; do
    wget $BASE/benchmarks/$d.tar.gz # bigblue4 = 43 MB
done
```

> `archive.sigda.org` 的镜像已失效（实测 502），不要用。

### 3.3 ISPD 2006 placement contest —— 密度目标维度

**相对 ISPD2005 的增量**：每个设计指定**不同的密度目标**（0.5 ~ 0.9），
并引入 scaled HPWL 评价指标。

这对我们的价值在于：只跑 adaptec1 的话，`targetDensity` 永远在同一个工作点上；
ISPD2006 能验证从 0.5 到 0.9 的全范围收敛性。

| 设计 | 对象数 | 密度目标 | 设计 | 对象数 | 密度目标 |
| --- | --- | --- | --- | --- | --- |
| adaptec5 | 843,128 | 0.5 | newblue4 | 646,139 | 0.5 |
| newblue1 | 330,474 | 0.8 | newblue5 | 1,233,058 | 0.5 |
| newblue2 | 441,516 | 0.9 | newblue6 | 1,255,039 | 0.8 |
| newblue3 | 494,011 | 0.8 | **newblue7** | **2,507,954** | 0.8 |

> newblue7 有 **10,098,844 个引脚**，是全部 BookShelf benchmark 中引脚数最多的。

**获取（已实测）**：

```bash
BASE=https://www.ispd.cc/contests/06/contest
for d in adaptec5 newblue1 newblue2 newblue3 newblue4 newblue5 newblue6 newblue7; do
    wget $BASE/$d.tar.gz     # adaptec5 = 17 MB, newblue7 = 51 MB
done
```

**附加：Inflated 版 ISPD2005**（同一页面提供）

```bash
for d in adaptec1 adaptec2 adaptec3 adaptec4 bigblue1 bigblue2 bigblue3 bigblue4; do
    wget $BASE/$d.inf.tar.gz
done
```

将标准单元尺寸膨胀以提高利用率（pin 偏移保持不变）。**这是很好的压力测试**——
高利用率下 filler 总面积会变为负值，正好验证我们在
[02 §2.2](02-baseline%20代码评估.md) 中指出的那个下界保护是否到位。

### 3.4 DAC 2012 / ICCAD 2015（superblue 系列）

这两套虽然分别面向布线拥塞与时序驱动，但**输入仍是 BookShelf 格式**
（已从 DREAMPlace 配置确认：`test/dac2012/superblue11.json` 使用的是 `"aux_input"`），
因此**我们的解析器可以直接读取**。额外的拥塞/时序数据我们用不上，但纯布局部分能跑。

| 套件 | 设计 | 侧重 |
| --- | --- | --- |
| **DAC 2012** | superblue 2 / 3 / 6 / 7 / 9 / 11 / 12 / 14 / 16 / 19 | 布线拥塞驱动 |
| **ICCAD 2015** | superblue 1 / 3 / 4 / 5 / 7 / 10 / 16 / 18 | 时序驱动 |

规模在 40 万 ~ 200 万之间，比 ISPD2005 更接近现代设计。**等于白拿 18 个规模合适的测试用例。**

**获取**：

- **ICCAD 2015**：DREAMPlace 提供两个变体的 Google Drive 链接
  （见 `~/DREAMPlace/benchmarks/iccad2015.ot.md` 与 `iccad2015.hs.md`）：
  - `.ot`（OpenTimer 版）：`https://drive.google.com/file/d/1xeauwLR9lOxnYvsK2JGPSY0INQh8VuE4/view`
  - `.hs`（HeteroSTA 版）：`https://drive.google.com/file/d/1HsAW_qcRje_-Ex1anWqAEQOKpGeCxpZa/view`
- **DAC 2012**：DREAMPlace 的下载脚本与 README 中**均未提供**获取途径，需另找
  （可尝试 DAC 2012 竞赛官网或 UT Austin / CUHK 的镜像）。

---

## 4. LEF/DEF 系（需扩展解析器）

以下套件格式为 LEF/DEF，**当前不可直接使用**，列出备查。

### 4.1 ISPD 2015 / 2018 / 2019

- **ISPD 2015**：详细布线驱动布局。打包版 `http://www.cerc.utexas.edu/~zixuan/ispd2015dp.tar.xz`，
  **169 MB，实测 HTTP 200**。
- **ISPD 2018 / 2019**：初始详细布线竞赛，10 个 testcase。
  `https://www.ispd.cc/contests/19/benchmarks/ispd19_test1.tgz` **实测 HTTP 200**。

> 值得注意：课程材料 3.3 讲解 DEF 格式时所用的示例正是 `ispd19_test1.input.def`，
> 说明课程组考虑过这条路径。**若后期想把 LEF/DEF 解析作为扩展项，这里就是现成数据。**

### 4.2 TILOS MacroPlacement —— 最现代，含大量可移动宏

TILOS-AI 研究所维护，是 Google Nature 论文（RL 宏布局）的开源复现基准，非常"当代"。

| 设计 | 触发器数 | 宏单元配置 |
| --- | --- | --- |
| Ariane136 | 19,839 | (256×16-bit SRAM) × 136 |
| Ariane133 | 19,807 | (256×16-bit SRAM) × 133 |
| MemPool tile | 18,278 | (256×32) × 16 + (64×64) × 4 |
| **MemPool group** | **360,724** | (256×32) × 256 + (64×64) × 64 + 其他 × 4 = **324 个宏** |
| NVDLA | 45,295 | (256×64-bit SRAM) × 128 |
| BlackParrot | 214,441 | 多种规格合计 **220 个宏** |

工艺为 NanGate45 与 ASAP7，提供 RTL、LEF、DEF、SDC 全套。
**优势是宏单元数量多且真实**，是验证混合尺寸布局的现代数据。

⭐ **它自带 LEF/DEF ↔ BookShelf 格式转换器**（`CodeElements/FormatTranslators/`，基于 OpenDB），
理论上可转成我们能读的格式。但转换会丢失信息（引脚位置被并到单元中心、金属层信息丢失等），
需要评估可用性。

仓库：`https://github.com/TILOS-AI-Institute/MacroPlacement`（实测可达）

---

## 5. 规模与性能预估

以 250 万单元（bigblue4 / newblue7 量级）、约 500 轮 Nesterov 迭代粗估：

| 后端 | 单轮耗时（估） | 单设计总耗时 | 16 个设计全套 |
| --- | --- | --- | --- |
| CPU 单线程（easyPlace 式 AoS） | ~5 s | ~40 min | ~10 h |
| **CPU 8 线程 + SoA**（我们的 M1–M6） | 0.3~1 s | **3~8 min** | **1~2 h** |
| GPU（RTX 4060，估 10~20X）（M7） | ~0.05 s | ~30 s | ~10 min |

> DREAMPlace README 宣称在 ISPD2005 上全局布局 + 合法化相对 RePlAce（多线程 CPU）
> 有 **>30X 加速**（Tesla V100）；ABCDPlace 详细布局相对 NTUPlace3 约 **16X**。
> RTX 4060 Laptop 弱于 V100，但布局计算用 float32，取 10~20X 为保守预期。

**判读**：CPU 8 线程下单设计 3~8 分钟可以接受，但**参数调优需反复跑数十轮 sweep**，
届时 1~2 小时/轮的全套耗时会成为真正瓶颈。这是 M7 引入 CUDA 的主要动因。

**磁盘预算**：ISPD2005 全套约 103 MB（压缩）/ 约 1 GB（解压后）；
加 ISPD2006 与 MMS 后建议预留 **5 GB**。`benchmarks/` 目录应加入 `.gitignore`。

---

## 6. 获取清单与验证结果

**验证方式**：`curl -sSI -L`（HTTP HEAD），**验证时间 2026-09-09**。

| 资源 | URL | 状态 | 体积 |
| --- | --- | --- | --- |
| ISPD2005 打包版 | `http://www.cerc.utexas.edu/~zixuan/ispd2005dp.tar.xz` | ✅ 200 | 103 MB |
| ISPD2005 adaptec1 | `https://www.ispd.cc/contests/05/ispd05-contest/adaptec1.tar.gz` | ✅ 200 | 4 MB |
| ISPD2005 bigblue3 | `.../ispd05-contest/benchmarks/bigblue3.tar.gz` | ✅ 200 | — |
| ISPD2005 bigblue4 | `.../ispd05-contest/benchmarks/bigblue4.tar.gz` | ✅ 200 | 43 MB |
| ISPD2006 adaptec5 | `https://www.ispd.cc/contests/06/contest/adaptec5.tar.gz` | ✅ 200 | 17 MB |
| ISPD2006 newblue5 | `https://www.ispd.cc/contests/06/contest/newblue5.tar.gz` | ✅ 200 | — |
| ISPD2006 newblue7 | `https://www.ispd.cc/contests/06/contest/newblue7.tar.gz` | ✅ 200 | 51 MB |
| ISPD2015 打包版 | `http://www.cerc.utexas.edu/~zixuan/ispd2015dp.tar.xz` | ✅ 200 | 169 MB |
| ISPD2019 test1 | `https://www.ispd.cc/contests/19/benchmarks/ispd19_test1.tgz` | ✅ 200 | — |
| TILOS MacroPlacement | `https://github.com/TILOS-AI-Institute/MacroPlacement` | ✅ 200 | — |
| MMS 重建版 | IEEE DataPort DOI `10.21227/2n68-tx57` | ⚠️ 需注册 | 379 MB |
| ICCAD2015 `.ot` | Google Drive `1xeauwLR9lOxnYvsK2JGPSY0INQh8VuE4` | ⚠️ 未验证 | — |
| ICCAD2015 `.hs` | Google Drive `1HsAW_qcRje_-Ex1anWqAEQOKpGeCxpZa` | ⚠️ 未验证 | — |
| MMS 原站 | `public.iastate.edu/~zijunyan` | ❌ **已失效** | — |
| SIGDA 镜像 | `http://archive.sigda.org/ispd2005/...` | ❌ **502** | — |

> ⚠️ 目录列表（`.../contests/19/benchmarks/`、`.../~zixuan/`）返回 403，
> 这只是禁止列目录，**具体文件仍可下载**，不要被误导。

---

## 7. 采用建议与目录约定

### 7.1 分级建议

| 优先级 | 动作 | 理由 |
| --- | --- | --- |
| **P0** | 获取 **MMS** | 唯一能让"混合尺寸布局"这一课程核心目标真正被执行的数据。IEEE DataPort 需注册；若受阻，按附录方案自行重建 |
| **P1** | 下载 **ISPD2005 全套** | 一条命令 103 MB，立刻获得 211k → 2.18M 完整规模阶梯，用于性能、内存与数值稳定性验证。**成本最低、收益最直接** |
| **P2** | 下载 **ISPD2006** + inflated 版 | 8 个不同密度目标（0.5~0.9）验证 `targetDensity` 适应性；inflated 版做高利用率压力测试 |
| P3 | DAC2012 / ICCAD2015 superblue | 白拿 18 个 BookShelf 用例，但需先找到 DAC2012 的下载源 |
| 暂不 | LEF/DEF 系、TILOS | 需额外解析器或格式转换，等主线跑通再评估 |

### 7.2 目录约定

```
benchmarks/                 ← 加入 .gitignore，体积大
├── mms/
│   ├── adaptec1/{*.aux,*.nodes,*.nets,*.pl,*.scl,*.wts}
│   └── ...  (16 个)
├── ispd2005/
│   ├── adaptec1/ ... bigblue4/   (8 个)
├── ispd2006/
│   ├── adaptec5/ ... newblue7/   (8 个)
└── ispd2005.inf/                 (可选，压力测试)
```

保持与 `test_data/` 相同的 `<套件>/<设计>/<设计>.aux` 结构，
这样 `run_matrix.py` 只需扫描目录即可自动发现全部用例。

### 7.3 与里程碑的对应

- **M1（任务 3）**：仍用 `test_data/adaptec1` 对拍统计数字，无需新数据。
- **M6（规模扩展）**：需要 P0 + P1。这是 M6 的**阻塞项**——
  没有 MMS 就无法验证混合尺寸，没有 ISPD2005 全套就无法验证规模可扩展性。
- **M7（CUDA）**：需要 P1 中的大规模设计（bigblue3/4）来体现加速比。

---

## 附录：从 ISPD2005 自行重建 MMS

若 IEEE DataPort 注册受阻，可按 DAC 2009 论文的规则自行重建。规则本身很简单：

**修改 1 —— 解放宏单元**

在 `.nodes` 文件中，宏单元当前被标记为 `terminal`：

```text
o496037  726  1068  terminal      ← 原始：固定宏
o496037  726  1068                ← 修改后：可移动宏（去掉 terminal 标记）
```

在 `.pl` 文件中去掉对应的 `/FIXED`：

```text
o496037  4005  4114  : N /FIXED   ← 原始
o496037  4005  4114  : N          ← 修改后
```

**修改 2 —— I/O 尺寸置零**

真正的 I/O pad（位于芯片边界、尺寸较小的那些 terminal）保持固定，但宽高置为 0。

**判别宏单元与 I/O 的经验规则**：宏单元面积远大于标准单元（高度 ≫ 行高 12），
且通常位于 core region 内部；I/O pad 位于 core region 边界外或紧贴边界。

> **注意**：重建结果与官方 MMS 未必逐字节一致（论文未给出完整的 I/O 判别脚本），
> 因此**不能用于跨论文的 HPWL 横向比较**，但完全可以用于
> **验证我们自己的混合尺寸代码路径是否被正确执行**——这才是我们的主要目的。
>
> 如需此脚本，可在 M6 阶段实现，输入 ISPD2005 目录、输出 MMS 风格目录，约 100 行 Python。
