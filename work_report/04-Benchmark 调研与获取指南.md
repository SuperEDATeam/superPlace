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
- [2. 混合尺寸数据的真实情况](#2-混合尺寸数据的真实情况)
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
| **MMS** | BookShelf | 16 | 211k–2.6M | ✅ **全部** | ✅ | ❌ 需付费订阅 | **P0** |
| **ISPD 2005** | BookShelf | 8 | 211k–2.18M | ⚠️ 仅 bigblue3 | ✅ | ✅ 已下载 | **P1** |
| **ISPD 2006** | BookShelf | 8 | 330k–2.51M | ✅ **newblue1** | ✅ | ✅ 已下载 | **P1** |
| **DAC 2012** | BookShelf | 10 | ~400k–2M | ❌ | ✅ | ⚠️ 需另找源 | P3 |
| **ICCAD 2015** | BookShelf + 时序 | 8 | ~400k–2M | ❌ | ✅ | ✅ Google Drive | P3 |
| ISPD 2015 | LEF/DEF | 8 | 中等 | — | ❌ | ✅ 实测可下 | 暂不 |
| ISPD 2018/2019 | LEF/DEF | 10 | 中等 | — | ❌ | ✅ 实测可下 | 暂不 |
| TILOS MacroPlacement | RTL + LEF/DEF | 6 | 18k–360k flop | ✅ **有** | ❌ 需转换 | ✅ GitHub | 暂不 |

**一句话建议**：ISPD2005 + ISPD2006 全 16 个设计已下载（见 [§6.1](#61-2026-10-02-复测与实际下载结果)），
规模阶梯 21 万 → 251 万齐备。混合尺寸用 **ISPD2006 newblue1**（原版自带 64 个可移动宏，
占可移动面积 53.7%）加我们自己重建的 MMS；官方 MMS 需付费订阅，暂不可得。

> ⚠️ **本文档 2026-10-02 做过一次系统性更正。** 初版有两处结论是从 `test_data/` 仅有的
> 3 个设计外推出来的，拿到全套后发现不成立：
> ① "ISPD2005/2006 原版将所有宏单元固定"——错，见 [§2](#2-混合尺寸数据的真实情况)；
> ② "MMS 是唯一能验证混合尺寸的数据"——错，newblue1 就可以。
> 另有一处下载源结论已失效（`ispd.cc` 当时判为反爬，实为限流）。

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

## 2. 混合尺寸数据的真实情况

> **本节已于 2026-10-02 整节重写。** 原标题是"当前数据的致命缺口"，结论是
> "ISPD2005/2006 原版将所有宏单元固定，MMS 是唯一可行的混合尺寸数据"。
> 那个结论是**从 `test_data/` 仅有的 adaptec1 / adaptec4 / thin1 三个设计外推**出来的。
> 拿到全部 16 个设计后实测，外推不成立。

### 2.1 `test_data/` 三个设计确实全为 0

（判据：`.nodes` 中非 `terminal` 且高度 > 1.5 倍行高）

| benchmark | 总单元 | 固定终端 | 可移动宏 |
| --- | --- | --- | --- |
| adaptec1 | 210,904 | 543 | **0** |
| adaptec4 | 494,716 | 1,329 | **0** |
| thin1 | 3 | 1 | **0** |

这一部分没错，错在由此推广到整个套件。

### 2.2 全套实测：三个设计原版就带可移动宏

| 设计 | 可移动宏 | 占可移动面积 | 高度分布（行数） | 面积 > 1e5 的真大宏 |
| --- | --- | --- | --- | --- |
| **ispd2006/newblue1** | **64** | **53.7%** | 2 行 ×11、19/23/29/49/59 行各若干 | **45 个**，最大 5986×4728 |
| ispd2005/bigblue3 | 2485 | 6.4% | **2 行 ×2480**、3 行 ×1、18 行 ×3、132 行 ×1 | 4 个 |
| ispd2006/newblue2 | 3723 | 6.6% | **2 行 ×3723**（全部） | **0 个** |
| *对照*：mms/adaptec1 | 63 | 56.9% | — | 57 个 |

**两类要分开看，这是关键区别：**

- **newblue1 是真正的混合尺寸设计**：64 个宏占可移动面积 53.7%，其中 45 个是大宏，
  与我们重建的 MMS adaptec1（63 个宏、56.9%）完全同量级。
  **它足以真实地压测宏密度缩放、mLG 合法化、mGP/cGP 切换全部路径。**
- **bigblue3 与 newblue2 的"宏"几乎全是双高单元**（2 行高）。它们会让
  `isMacro > 0` 成立、从而触发四阶段代码路径，但宏只占 6% 面积、几乎没有大块，
  **能跑通不等于压测到**。newblue2 更极端：3723 个"宏"里一个大宏都没有。

> 这也提醒 `isMacro(i) = !isFixed(i) && h > rowHeight*1.5`（见 05 §5.1.2）这个判据的
> 性质：它识别的是"比标准单元高"，不是"是大宏"。双高单元会被判为宏。
> 这对密度缩放是合理的（它们确实不是标准行单元），但**不能用宏的个数来判断
> 一个设计是否适合做混合尺寸压测**，要看宏占面积的比例。

### 2.3 修正后的结论

原结论"MMS 是必需品"**不成立**：ISPD2006 newblue1 就能真实执行并压测混合尺寸路径。

但 MMS 仍然有价值，理由变了——不再是"唯一可行"，而是**覆盖面**：
它给出 16 个规模从 21 万到 251 万、全部带可移动宏的设计，便于观察宏数量与规模
对算法的影响；而原版里真正可用的只有 newblue1 一个。

> **旁证仍然成立**：easyPlace 的 `main/ePlace_main.cpp:130` 是
> `if (placedb->dbMacroCount > 0 && !gArg.CheckExist("nomLG"))`，
> 因此在 adaptec1 / adaptec4 上 **mLG → FILLERONLY → cGP 三阶段从未被执行过**，
> 它的 ePlace-MS 三阶段在这两个设计上是死代码——这解释了作者为何在宏密度缩放处
> 留下一串 `?????`（`eplace.cpp:560`）。换成 newblue1 或 MMS 就能跑到。

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
| IEEE DataPort，DOI `10.21227/2n68-tx57` | ❌ **需付费订阅** | 379.72 MB，覆盖 16 个设计。页面原文 "Subscription Required — This dataset requires an IEEE DataPort Subscription to access"。且这是 2025-08-15 由第三方上传的重传（标题 "…for LightPlace"），非 DAC 2009 官方发布 |
| 自行从 ISPD2005 重建 | ✅ 可行 | 见[附录](#附录从-ispd2005-自行重建-mms) |

> DREAMPlace 仓库 `~/DREAMPlace/test/mms/` 已备好 16 个 json 配置文件
> （`adaptec1.json` ⋯ `newblue7.json`），其中的 `target_density`、`num_bins_x/y`、
> `stop_overflow` 等参数可直接作为我们的参考基线。

### 3.2 ISPD 2005 placement contest —— 规模阶梯主力

**定位**：布局领域被引用最多的 benchmark 套件，来自 IBM 的真实 ASIC 设计。

**特点**：8 个设计，21 万 → 218 万单元。宏单元**基本**固定——唯一的例外是 bigblue3，
它有 2485 个非 terminal 的高单元，但其中 2480 个只是双高单元（见 [§2.2](#22-全套实测三个设计原版就带可移动宏)）。
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
| ISPD2005 打包版 | `http://www.cerc.utexas.edu/~zixuan/ispd2005dp.tar.xz` | ❌ **403**（2026-09-29 复测） | 103 MB |
| ISPD2005 adaptec1 | `https://www.ispd.cc/contests/05/ispd05-contest/adaptec1.tar.gz` | ✅ 200 | 4 MB |
| ISPD2005 bigblue3 | `.../ispd05-contest/benchmarks/bigblue3.tar.gz` | ✅ 200 | — |
| ISPD2005 bigblue4 | `.../ispd05-contest/benchmarks/bigblue4.tar.gz` | ✅ 200 | 43 MB |
| ISPD2006 adaptec5 | `https://www.ispd.cc/contests/06/contest/adaptec5.tar.gz` | ✅ 200 | 17 MB |
| ISPD2006 newblue5 | `https://www.ispd.cc/contests/06/contest/newblue5.tar.gz` | ✅ 200 | — |
| ISPD2006 newblue7 | `https://www.ispd.cc/contests/06/contest/newblue7.tar.gz` | ✅ 200 | 51 MB |
| ISPD2015 打包版 | `http://www.cerc.utexas.edu/~zixuan/ispd2015dp.tar.xz` | ✅ 200 | 169 MB |
| ISPD2019 test1 | `https://www.ispd.cc/contests/19/benchmarks/ispd19_test1.tgz` | ✅ 200 | — |
| TILOS MacroPlacement | `https://github.com/TILOS-AI-Institute/MacroPlacement` | ✅ 200 | — |
| MMS 第三方重传 | IEEE DataPort DOI `10.21227/2n68-tx57` | ❌ **需付费订阅** | 379 MB |
| ICCAD2015 `.ot` | Google Drive `1xeauwLR9lOxnYvsK2JGPSY0INQh8VuE4` | ⚠️ 未验证 | — |
| ICCAD2015 `.hs` | Google Drive `1HsAW_qcRje_-Ex1anWqAEQOKpGeCxpZa` | ⚠️ 未验证 | — |
| MMS 原站 | `public.iastate.edu/~zijunyan` | ❌ **已失效** | — |
| SIGDA 镜像 | `http://archive.sigda.org/ispd2005/...` | ❌ **502** | — |

> ⚠️ 目录列表（`.../contests/19/benchmarks/`、`.../~zixuan/`）返回 403，
> 这只是禁止列目录，**具体文件仍可下载**，不要被误导。

### 6.1 2026-10-02 复测与实际下载结果

**ISPD2005 + ISPD2006 全 16 个设计已下载到本地并通过解析器验证。**

| 源 | 现状 | 说明 |
| --- | --- | --- |
| `ispd.cc` | ✅ **可用** | 16 个设计全部返回真实 gzip 数据 |
| UT Austin 打包版 | ❌ **403** | 2026-09-29 起拒绝访问，至今未恢复 |
| IEEE DataPort（MMS） | ❌ **需付费订阅** | 页面明确写 "Subscription Required"，见 [§3.1](#31-mmsmodern-mixed-size-优先级最高) |

> ⚠️ **更正 2026-09-29 的记录。** 当时判定 `ispd.cc` 有反爬保护（`GET` 返回 12 KB 的
> JS 挑战页）。实为**短时间密集请求触发的限流**，不是永久封锁——下载时每个文件之间
> `sleep 2` 即可，16 个全部一次成功。

**两条仍然有效的教训：**

1. **`curl -sSI`（HTTP HEAD）不足以验证可下载性。** 限流状态下服务器对 HEAD 放行、
   对 GET 返回挑战页，HEAD 的 200 是**假阳性**。验证下载源必须实际 `GET` 下来再
   `file` 检查类型。本文档 2026-09-09 那张"实测 200"的表就是这么错的。
2. **`wget` 成功退出不代表拿到了数据。** 它会把挑战页当正常内容保存。必须校验：

   ```bash
   file benchmarks/_dl/*.tar.gz | grep -v gzip   # 有输出就是没下对
   gzip -t benchmarks/_dl/*.tar.gz
   ```

### 6.2 实测下载命令与两个坑

```bash
mkdir -p benchmarks/_dl && cd benchmarks/_dl
B=https://www.ispd.cc/contests/05/ispd05-contest
C=https://www.ispd.cc/contests/06/contest

# 坑一：ISPD2005 的 8 个设计分处两个路径
for d in adaptec1 adaptec3; do wget -q -c "$B/$d.tar.gz"; sleep 2; done
for d in adaptec2 adaptec4 bigblue1 bigblue2 bigblue3 bigblue4; do
  wget -q -c "$B/benchmarks/$d.tar.gz"; sleep 2
done
# ISPD2006 路径统一
for d in adaptec5 newblue1 newblue2 newblue3 newblue4 newblue5 newblue6 newblue7; do
  wget -q -c "$C/$d.tar.gz"; sleep 2
done
```

**坑一：ISPD2005 的包分处两个路径。** 只有 `adaptec1` 与 `adaptec3` 在 `ispd05-contest/`
根下，其余 6 个在 `ispd05-contest/benchmarks/` 子目录。放错得到 404。

**坑二：包内有两种结构，且都是二次压缩。**

| 包 | 内部结构 |
| --- | --- |
| adaptec1、adaptec3、全部 ISPD2006 | 平铺：`adaptec1.aux.gz` … |
| adaptec2、adaptec4、bigblue1-4 | 多一层同名目录：`./adaptec2/adaptec2.aux.gz` … |

统一用一种方式解包，后 6 个会多套一层目录导致 `.aux` 找不到。解包后还要再 `gunzip` 一次：

```bash
cd benchmarks
for f in _dl/*.tar.gz; do
  d=$(basename "$f" .tar.gz); suite=ispd2005
  case "$d" in adaptec5|newblue*) suite=ispd2006;; esac
  mkdir -p "$suite/$d"
  # 靠包内首项是否含 "/" 区分两种结构
  if tar tzf "$f" | head -1 | sed 's|^\./||' | grep -q '/'; then
    tar xzf "$f" -C "$suite/$d" --strip-components=1
  else
    tar xzf "$f" -C "$suite/$d"
  fi
  gunzip -f "$suite/$d"/*.gz
done
```

**实测体积**：压缩包 271 MB，解压后 ISPD2005 968 MB + ISPD2006 1.4 GB，
`benchmarks/` 总计约 2.7 GB（已在 `.gitignore` 内）。

**验证**：16 个设计全部用 `--stage parse` 读通，对象数/线网数/引脚数与
[§6.3](#63-全套实测统计) 的文献值一致。

### 6.3 全套实测统计

`--stage parse` 的输出（2026-10-02）：

| 设计 | 对象数 | 固定 | 线网 | 引脚 | 可移动宏 |
| --- | --- | --- | --- | --- | --- |
| ispd2005/adaptec1 | 211,447 | 543 | 221,142 | 944,053 | 0 |
| ispd2005/adaptec2 | 255,023 | 566 | 266,009 | 1,069,482 | 0 |
| ispd2005/adaptec3 | 451,650 | 723 | 466,758 | 1,875,039 | 0 |
| ispd2005/adaptec4 | 496,045 | 1,329 | 515,951 | 1,912,420 | 0 |
| ispd2005/bigblue1 | 278,164 | 560 | 284,479 | 1,144,691 | 0 |
| ispd2005/bigblue2 | 557,866 | 23,084 | 577,235 | 2,122,282 | 0 |
| ispd2005/bigblue3 | 1,096,812 | 1,293 | 1,123,170 | 3,833,218 | **2,485** |
| ispd2005/bigblue4 | 2,177,353 | 8,170 | 2,229,886 | 8,900,078 | 0 |
| ispd2006/adaptec5 | 843,128 | 646 | 867,798 | 3,493,147 | 0 |
| ispd2006/newblue1 | 330,474 | 337 | 338,901 | 1,244,342 | **64** |
| ispd2006/newblue2 | 441,516 | 1,277 | 465,219 | 1,773,855 | **3,723** |
| ispd2006/newblue3 | 494,011 | 11,178 | 552,199 | 1,929,892 | 0 |
| ispd2006/newblue4 | 646,139 | 3,422 | 637,051 | 2,499,178 | 0 |
| ispd2006/newblue5 | 1,233,058 | 4,881 | 1,284,251 | 4,957,843 | 0 |
| ispd2006/newblue6 | 1,255,039 | 6,889 | 1,288,443 | 5,307,594 | 0 |
| ispd2006/newblue7 | 2,507,954 | 26,582 | 2,636,820 | 10,104,920 | 0 |

宏的**性质**差异见 [§2.2](#22-全套实测三个设计原版就带可移动宏)——个数不等于可压测性。

---

## 7. 采用建议与目录约定

### 7.1 分级建议

| 优先级 | 动作 | 理由 |
| --- | --- | --- |
| **P0** | **ISPD2006 newblue1** | 原版自带 64 个可移动宏、占可移动面积 53.7%，是**无需任何重建**就能真实压测混合尺寸的设计。官方 MMS 需付费订阅，而 newblue1 已在本地 |
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

### 已实现：`scripts/make_mms.py`（M3 阶段完成，非 M6）

```bash
python3 scripts/make_mms.py test_data/adaptec1 benchmarks/mms/adaptec1
```

**判别规则**：按**对象中心是否落在 core region 内**划分。中心在内为宏单元
（解放为可移动），中心在外为 I/O pad（尺寸置零、保持固定、写成 `terminal_NI`）。

**这条规则在 adaptec1 上被两个独立判据交叉验证**：480 个中心在 core 外的
terminal 恰好只有 `432×72` 与 `72×432` 两种尺寸（典型四边 pad 环），
其余 63 个在 core 内的才是真宏（`164×2136` / `500×2136` / `1206×2856` …）。
位置判据与尺寸判据给出同一划分。

**一处论文未提、但必须处理的细节**：BookShelf 的引脚偏移是**相对对象中心**的。
若把 `432×72` 的 pad 就地改成 `0×0`，其中心从 `(x+216, y+36)` 跳回 `(x, y)`，
挂在其上的引脚随之平移——实测 adaptec1 的 480 个 pad 平均平移 **126 单位**。
脚本因此在置零的同时把坐标改写为原中心。**验证**：重建前后 HPWL 逐位相同
（adaptec1 `1.0492422900e+08`，adaptec4 `3.9788536200e+08`）。

**重建结果**：

| 设计 | 可移动宏 | I/O pad 置零 | 宏占可移动面积 | 说明 |
| --- | --- | --- | --- | --- |
| adaptec1 | **63** | 480 | **56.9%** | 干净的宏/pad 分离，四阶段验证主力 |
| adaptec4 | **1329** | 0 | — | 该设计 `.nodes` 中本就没有独立 pad 对象 |

> adaptec4 的 1329 个 terminal 全部中心在 core 内、最小尺寸 `504×216`（18 个 row 高），
> 即原始数据里没有 I/O pad 对象。脚本如实输出"0 个 pad"，这是规则的正确结果而非 bug。
> 副作用是重建后 adaptec4 **没有任何固定节点**（`terminalDensity` 恒为 0），
> 是个有用的退化边界用例。

`.nets` / `.wts` / `.scl` 逐字节不变，`.nodes` / `.pl` 行数守恒，
`NumTerminals` 同步更新（543→480，1329→0）。
