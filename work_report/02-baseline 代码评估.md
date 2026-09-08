# baseline_ (easyPlace) 代码评估报告

> 评估对象：`baseline_/` —— **easyPlace**，上海科技大学 Ziang Ge / Yikai Liu 在 Pingqiang Zhou
> 教授指导下完成的 ePlace / ePlace-MS 重实现。
>
> 评估目的：判断该项目是否已良好覆盖本课程要求的全部开发任务，并识别可优化点。
>
> 评估方式：全量只读代码审查，未运行。所有结论均给出 `文件:行号` 证据。
>
> 撰写日期：2026-09-08

---

## 0. 结论摘要

**没有完全覆盖课程要求。任务 5、7 做得超出要求，任务 3 基本达标，但任务 4 和任务 6 各缺一半，
且任务 4 的核心代码中存在一个实质性 bug。**

| 维度 | 评价 |
| --- | --- |
| 算法深度 | ⭐⭐⭐⭐⭐ ePlace 主循环、FFT 泊松求解、Abacus 行内二次规划均忠实于论文与 RePlAce |
| 任务覆盖 | ⭐⭐⭐ 缺初始布局对比（2/3 未做）与 GDSII 导出 |
| 正确性 | ⭐⭐⭐ 发现 1 个实质 bug + 2 处存疑实现 |
| 性能 | ⭐⭐ 完全单线程；每轮重建 FFT；巨型 vector 按值传递 |
| 工程质量 | ⭐⭐ `-w` 压掉全部警告；无合法性检查；内存不释放；大段重复代码 |

**代码规模**：约 15.5k 行，扣除 Ooura FFT 第三方代码（`fftsg*.cpp` 共 4800 行）后自研约 10.7k 行。

---

## 1. 任务覆盖度对照

| 任务 | 课程要求 | easyPlace 实现 | 判定 |
| --- | --- | --- | --- |
| 任务 3 | BookShelf 六类文件解析 | `.aux/.scl/.nodes/.nets/.pl` 完整，**`.wts` 未解析** | ⚠️ 基本达标 |
| 任务 4 | 初始布局**至少三种**方法并对比分析 | **仅二次规划一种** | ❌ 缺 2/3 |
| 任务 5 | 全局布局 | ePlace 完整实现，含 mGP / FILLERONLY / cGP 三阶段 | ✅ 超额 |
| 任务 6 | CImg 出图 **+ GDSII 导出** | 仅 CImg（存 BMP），**无任何 GDSII 代码** | ❌ 缺一半 |
| 任务 7 | 布局合法化（选做） | Abacus + 宏单元模拟退火 + 详细布局 | ✅ 大幅超额 |

### 1.1 任务 3 · BookShelf 解析 —— 基本达标

`Parser/parser.h` 暴露的接口只有四个读取函数：

```cpp
// Parser/parser.h
int ReadFile(string file, PlaceDB &db);
int ReadPLFile(string file, PlaceDB &db, bool init);
private:
    int ReadSCLFile(string file, PlaceDB &db);
    int ReadNodesFile(string file, PlaceDB &db);
    int ReadNetsFile(string file, PlaceDB &db);
```

**`.wts` 未被解析**：`parser.cpp:22` 声明了 `char file_wts[500]`，`parser.cpp:27` 也从 `.aux`
里 `sscanf` 出了这个文件名，`parser.cpp:29` 还把它打印了出来——但**从头到尾没有 `ReadWtsFile`**，
`parser.cpp:32-35` 只调用了 SCL / Nodes / Nets / PL 四个读取函数。网络权重在全项目中完全未被使用。

> 对 adaptec 系列而言 `.wts` 本身是空文件，功能上无影响。但课程任务 2 明确列出了 `.wts`
> 作为六类文件之一，自己实现时应当至少把接口留出来。

**LEF/DEF 是空壳**：

```cpp
// Parser/parser.h
class LEFDEFParser
{
    // maybe in the future...
};
```

任务 3 只要求 BookShelf，所以这不算缺口，但如果想覆盖任务 2 提到的 LEF/DEF，需要从零做。

### 1.2 任务 4 · 初始布局 —— 最大缺口（仅完成 1/3）

课程要求"采用至少三种方法"并从连接保持能力、时间复杂度、面积控制、适用规模等方面对比分析。
easyPlace **只实现了二次规划（Kraftwerk2 B2B 模型）一种**。

**随机布局是死代码**。`PlaceDB` 中确有 `randomPlacment()`（注意作者的拼写笔误，
少了一个 `e`），定义在 `placedb.cpp:258`，声明在 `placedb.h:72`。全局搜索调用点的结果：

| 符号 | 定义 | 调用点 |
| --- | --- | --- |
| `randomPlacment()` | `placedb.cpp:258` | **无任何调用** |
| `setModuleLocation_2D_random()` | `placedb.cpp:222` | 仅 `eplace.cpp:141`、`eplace.cpp:748`，**用途是撒 filler** |

也就是说随机布局既没有接入主流程，也没有作为 CLI 选项暴露。

**聚类驱动布局完全不存在**。`grep -i cluster` 的全部命中都来自 Abacus 合法化中的
`AbacusCellCluster`（`legalizer.h:63`），与初始布局的簇聚类无关。

**唯一相关的是 `addNoise()`**（`main/ePlace_main.cpp:89-92`），在 mGP 前给位置加
`[-avgbinStep, avgbinStep]` 的随机扰动，这是一个扰动手段，不是独立的初始布局方法。

### 1.3 任务 5 · 全局布局 —— 完成度高

实现要素齐备，且与 ePlace 论文 / RePlAce 参考实现逐条对应：

| 要素 | 实现位置 | 备注 |
| --- | --- | --- |
| Filler 初始化 | `eplace.cpp:27` `fillerInitialization()` | whitespace → 缩放面积 → filler 总面积 → 数量 |
| Bin 网格初始化 | `eplace.cpp:164` `binInitialization()` | 含 terminalDensity 与 baseDensity 预计算 |
| 密度统计 | `eplace.cpp:867` `binNodeDensityUpdate()` | 含 local smooth 与宏单元密度缩放 |
| FFT 泊松求解 | `eplace.cpp:459` `densityGradientUpdate()` + `FFT/` | Ooura FFT，DCT/DST |
| WA 线长梯度 | `eplace.cpp:375` `wirelengthGradientUpdate()` | 另有 LSE 模型可选（`-LSE`） |
| γ 自适应 | `eplace.cpp:384-408` | 与论文式 (38) 一致，含 τ>1.0 / τ<0.1 的分段缩放 |
| 密度溢出率 τ | `eplace.cpp:358` `densityOverflowUpdate()` | 正确排除了 fillerDensity |
| λ 初始化 | `eplace.cpp:683` `penaltyFactorInitilization()` | 梯度绝对值之比 |
| λ 动态更新 | `eplace.cpp:708` `updatePenaltyFactor()` | 上下界 1.05 / 0.95，跟随 Xplace 与 RePlAce |
| Nesterov 优化器 | `Optimization/nesterov.hpp` | 另有 adam / momentum 备选 |
| 三阶段流程 | `mGP → FILLERONLY → cGP` | `eplace.cpp:744` `switch2FillerOnly()`、`:754` `switch2cGP()` |

**这部分是整个项目最有价值的地方，值得逐行读懂。**

### 1.4 任务 6 · 可视化 —— 缺一半

`Plot/plot.cpp` 提供两个函数：`plotCurrentPlacement()`（`plot.cpp:17`）与
`plotEPlace_2D()`（`plot.cpp:89`）。二者均以 CImg 绘制矩形并 `img.save_bmp()` 保存。

**问题**：

1. **完全没有 GDSII 导出**。全项目 `grep -i "gds"` 零命中。任务 6 明确要求的第二种展示方式缺失。
2. **存的是 BMP 不是 PNG**。BMP 无压缩，全芯片图动辄数 MB。
3. **没有 GIF 合成**，`-fullPlot` 只是逐迭代输出独立 BMP 文件。
4. **无分层信息**，标准单元、宏、filler、terminal 只靠颜色区分（Red / Orange / Green / Blue），
   无法交互式开关。

### 1.5 任务 7 · 合法化 —— 大幅超额

| 组件 | 位置 |
| --- | --- |
| Abacus 标准单元合法化 | `legalizer.h:105` `AbacusLegalizer`，`legalizer.cpp:3` |
| 宏单元模拟退火合法化 | `legalizer.h:141` `SAMacroLegalizer`，`legalizer.cpp:469` |
| 详细布局（课程未要求） | `DetailedPlacement/detailed.cpp`，含 ISM 独立集匹配、局部重排、全局交换 |
| 外部合法化器接口 | `main/ePlace_main.cpp:188` 调用 ntuplace3 |

课程任务 7 只是选做，这里连详细布局都做了，是明显的加分项。

**但缺少验证**：全项目搜索 `checkLegal` / `legalityCheck` / `verifyPlacement` / `overlapCheck`
**零命中**。合法化跑完后没有任何代码验证结果真的合法（无重叠、对齐到 row/site、不越界）。

---

## 2. 缺陷清单

### 2.1 【P0】二次布局 Y 方向边界判定复制粘贴错误

**位置**：`QPlace/qplace.cpp:157`

X 方向的判定正确（`qplace.cpp:115`）：

```cpp
if (pin1 == curNet->boundPinXmin || pin1 == curNet->boundPinXmax ||
    pin2 == curNet->boundPinXmin || pin2 == curNet->boundPinXmax)
```

四个条件覆盖「pin1 或 pin2 中任一为 X 方向边界引脚」。

Y 方向是复制过去改的，改漏了（`qplace.cpp:157`）：

```cpp
if (pin2 == curNet->boundPinYmin || pin1 == curNet->boundPinYmax ||
    pin2 == curNet->boundPinYmin || pin2 == curNet->boundPinYmax)
//  ^^^^ 第 1 项                      ^^^^ 第 3 项与第 1 项完全重复
//  缺失：pin1 == curNet->boundPinYmin
```

**影响分析**：

外层循环是 `for (j...)` / `for (k = j+1...)`，`pin1 = netPins[j]`、`pin2 = netPins[k]`，
所以 `pin1` 恒为索引较小者。当 Y 方向最小边界引脚在 net 的引脚列表中排在另一引脚之前时，
该配对被跳过——平均丢失约一半的「Ymin ↔ 内部引脚」配对。

B2B 模型的正确性依赖 2P−3 项伸缩相消后精确等于 \(y_{max} - y_{min}\)。丢失配对后该恒等式不成立，
Y 方向的二次目标不再准确拟合 HPWL，对底部边界引脚的吸引被系统性削弱。

**隐蔽性高**：不会崩溃，不会报错，只表现为 Y 方向初始布局质量偏差。

> **对本项目的启示**：X/Y 两个方向的处理**必须抽成一个函数传方向参数**，
> 严禁复制粘贴。这类 bug 在布局器中极其常见，且极难通过观察结果发现。

### 2.2 【P1】filler 数量可能为负导致内存崩溃

**位置**：`EPlace/eplace.cpp:91` 与 `:127`

```cpp
// eplace.cpp:91
totalFillerArea = whitespaceArea * targetDensity - nodeAreaScaled;
// ...
// eplace.cpp:127
int fillerCount = (int)(totalFillerArea / fillerArea + 0.5);
// eplace.cpp:129
ePlaceFillers.resize(fillerCount);
```

当设计较拥挤（`whitespaceArea × targetDensity < nodeAreaScaled`，即目标密度设得偏低
或电路本身利用率高）时 `totalFillerArea` 为负，`fillerCount` 随之为负。
`vector::resize()` 参数是 `size_t`，负数会被转成约 \(2^{64}\) 的巨值 → `std::length_error` 或 OOM。

**全程无任何保护**。修复方式：`fillerCount = max(0, ...)`，并在为 0 时给出告警。

### 2.3 【P1】宏单元密度缩放在梯度计算中缺失

**位置**：`EPlace/eplace.cpp:560-562`（对比 `:950-953`）

统计密度时，宏单元乘了 `targetDensity`（`eplace.cpp:952`）：

```cpp
if (curNode->isMacro)
{
    bins[i][j]->nodeDensity += localSmoothLengthScale.x * localSmoothLengthScale.y
                             * targetDensity * overlapArea;
}
```

但计算密度梯度（受力）时没乘，且作者自己在旁边留下了一串问号（`eplace.cpp:560`）：

```cpp
//! ????????????????????? watch out: do we need macro density scaling here?
//!                       it seems RePlAce didn't do that
densityGradient[index].x += overlapArea * bins[i][j]->E.x;
densityGradient[index].y += overlapArea * bins[i][j]->E.y;
```

按 ePlace-MS 式 (16) \(\partial D / \partial x_i = -q_i \xi_{i_x}\)，电荷量 \(q_i\)
对宏单元同样应当是缩放后的面积。**场是缩放的、力却不缩放**，混合尺寸模式下宏单元
对密度场的贡献与其所受的力不自洽。

> 注：adaptec1 的 macro 数为 0，此问题只在 adaptec4 等含宏单元的用例中显现。

### 2.4 【P2】`getGradient()` / `getPosition()` 存在无返回值路径

**位置**：`EPlace/eplace.cpp:622` 与 `:638`

```cpp
vector<VECTOR_3D> EPlacer_2D::getGradient()
{
    if (placementStage == mGP)          { return totalGradient; }
    else if (placementStage == FILLERONLY) { return fillerGradient; }
    else if (placementStage == cGP)     { return cGPGradient; }
    // 无 else —— 落到这里是未定义行为
}
```

三个 `if/else if` 没有 `else` 兜底。编译器本应给出 `control reaches end of non-void function`
警告，但被 `-w` 全局压掉了（见 §4.1）。

### 2.5 【P2】中间百分位与论文不一致

**位置**：`EPlace/eplace.cpp:109-110`

```cpp
int minIdx = (int)(0.05 * (float)nodeCount); //! for calculating average area of the middle 80% ...
int maxIdx = (int)(0.95 * (float)nodeCount);
```

`5% ~ 95%` 是**中间 90%**，而 ePlace 论文 §3.2 原文是
"the average size of the **mid-80%** of movable cells"（即 10%~90%）。

且代码自身注释矛盾：`:109` 写 "middle 80%"，`:119` 写 "middle 90%"。

> 这一处同时解释了课程资料的来源，详见 [附录](#附录课程资料勘误的代码级实锤)。

---

## 3. 性能问题

### 3.1 完全单线程

`CMakeLists.txt` 的编译选项里挂了 `-fopenmp`，`main/CMakeLists.txt` 也链接了 `-lgomp`，
但**全项目 `#pragma omp` 数量为 0**。

唯一的并行是 `QPlace/qplace.cpp:32` 给 Eigen 设的 `setNbThreads(8)`。

ePlace 主循环中最重的两块——线长梯度（遍历全部 pin）与密度统计（遍历全部 node+filler）——
都是天然可并行的，却完全没有利用。

### 3.2 每轮迭代重建 FFT 对象

**位置**：`EPlace/eplace.cpp:465`

```cpp
void EPlacer_2D::densityGradientUpdate()
{
    replace::FFT_2D fft(binDimension.x, binDimension.y, binStep.x, binStep.y);
    // ...
}
```

构造函数会调用 `FFT_2D::init()`（`FFT/fft.cpp:48`），其中：

- `new` 出 4 个 `binCntX × binCntY` 的 float 二维数组（`fft.cpp:49-58`）
- 重建 `csTable_`、`wx_`、`wxSquare_`、`wy_`、`wySquare_`、`workArea_` 等查找表（`fft.cpp:69+`）

对 512×512 网格，这是**每轮约 4 MB 的分配 + 三角函数表重算**，而全局布局要跑数百轮。
析构时再全部释放。这部分开销纯属浪费。

**修复**：在 `binInitialization()` 中构造一次并作为成员持有，每轮只调 `updateDensity()` + `doFFT()`。

### 3.3 巨型 vector 按值传递与返回

**位置**：`EPlace/eplace.h:159-161`、`eplace.cpp:854`

```cpp
vector<VECTOR_3D> getGradient();                        // 按值返回，~40 万元素
vector<VECTOR_3D> getPosition();                        // 按值返回
void setPosition(vector<VECTOR_3D>);                    // 按值传参
private:
    vector<VECTOR_3D> getModulePositions(vector<Module *>);  // 按值传参 + 按值返回
```

`getModulePositions(vector<Module *> modules)` 的参数是按值的——每次调用拷贝一个约 40 万元素的
指针数组。Nesterov 每轮迭代都要取梯度和位置，这些拷贝累计相当可观。

**修复**：参数改 `const&`，返回值改为写入调用方提供的缓冲，或依赖 RVO 但避免参数拷贝。

### 3.4 baseDensity 计算是双重全遍历

**位置**：`EPlace/eplace.cpp:323-343`

```cpp
for (int i = 0; i < binDimension.x; i++)
    for (int j = 0; j < binDimension.y; j++)
        for (SiteRow curRow : db->dbSiteRows)   // 注意：按值拷贝 SiteRow
            curBinAvailableArea += getOverlapArea_2D(...);
```

adaptec1 是 512×512 网格 × 890 行 ≈ **2.3 亿次矩形求交**。虽然只在初始化时跑一次，
但完全可以按 y 坐标为 SiteRow 建索引，只遍历与该 bin 的 y 区间相交的行，降到近线性。

另外内层 `for (SiteRow curRow : ...)` 是**按值遍历**，每次迭代拷贝一个 `SiteRow` 对象
（其中还含 `vector<Interval>`）。同样的按值遍历也出现在 `eplace.cpp:48`。

---

## 4. 工程质量问题

### 4.1 `-w` 压掉全部编译警告

**位置**：`CMakeLists.txt:4`

```cmake
SET(CMAKE_CXX_FLAGS "-w -m64 -O3 -fpermissive -std=c++1y -fPIC -fopenmp
                     -funroll-loops -ffast-math -Dcimg_display=1")
```

`-w` 关闭**所有**警告，§2.4 那类问题正是这么被掩盖的。`-fpermissive` 进一步放宽了合法性检查。

`-ffast-math` 在 DCT 与迭代求解这类数值敏感场景中值得警惕：它允许编译器重排浮点运算、
假设无 NaN/Inf，可能掩盖或引入数值问题。建议开发调试期关闭，性能调优阶段单独评估。

### 4.2 无合法性检查

搜索 `checkLegal` / `legalityCheck` / `verifyPlacement` / `overlapCheck` **零命中**。

合法化与详细布局跑完后，没有任何代码验证结果真的合法。这正好印证了设计文档
[决策 6](01-架构设计与技术选型.md) 的判断：**检查器必须作为独立组件存在**。

### 4.3 内存管理

全项目使用裸 `new` 且基本不 `delete`：

- `PlaceDB` —— `main/ePlace_main.cpp:15`
- `Bin_2D` —— `eplace.cpp:238`，512×512 = 26 万个对象
- filler `Module` —— `eplace.cpp:137`
- `QPPlacer` / `EPlacer_2D` / 各类 legalizer —— `ePlace_main.cpp:83, 99, 132, 199, 219`

靠进程退出兜底。作为一次性运行的批处理工具尚可接受，但如果要接 GUI
（同一进程内反复运行布局）就会成为真实的内存泄漏。

### 4.4 大段重复代码

- `Plot/plot.cpp:17` 的 `plotCurrentPlacement()` 与 `:89` 的 `plotEPlace_2D()`，
  两个函数约 80 行**几乎逐字重复**，只差 filler 绘制那一段。
- `EPlace/eplace.cpp:769-852` 躺着 **85 行注释掉的** `plotCurrentPlacement` 实现，
  与 `plot.cpp` 中的版本又是重复。
- `QPlace/qplace.cpp` 中 X 方向（`:115-156`）与 Y 方向（`:157-198`）两段逻辑结构完全相同，
  正是 §2.1 那个 bug 的温床。

### 4.5 构建系统组织混乱

- 根 `CMakeLists.txt` 用 `ADD_SUBDIRECTORY` 引入 QPlace / Parser / EPlace / Optimization /
  Legalization / main，但 `main/CMakeLists.txt` 又把所有 `.cpp` **直接列进同一个可执行文件**，
  子目录的 CMakeLists 实际只用来构建各自的测试程序。没有形成真正的库分层。
- `main/CMakeLists.txt` 把 `.hpp` 文件（`nesterov.hpp`、`opt.hpp`）也列进了源文件列表，
  这是无效的。
- `Optimization/optimizer.cpp`、`adam.cpp`、`momentum.cpp` 与 `Legalization/ePlaceAbacus.cpp`
  **不在构建中**（后者本身是另一个独立的 `main()` 测试驱动）。
- `Library/` 同时存放了 `eigen-3.4.0` 与 `eigen-git-mirror` 两份 Eigen（共 38 MB），
  而 include 路径指向的是 `eigen-git-mirror`。CImg 又占 24 MB。

### 4.6 作者自留的疑虑标记

代码中有 56 处 `!!!` / `??` / `?????` 标记，是作者自己对实现不确定的地方。典型如：

```cpp
// eplace.cpp:84
//??? macro area should *= target density when calculating Am in(13)?
//    see RePlAce code opt.cpp line 86 But terminal area wasn't *= target density
//    when calculating Aws??? implement as this for now

// eplace.cpp:715
if ((deltaHPWL) < 0.0) //?? what if (curHPWL - lastHPWL)<0????? never considered before 2024.5.19

// eplace.cpp:882
bool localSmooth = false;  //! local smooth is applied only to std cells, is this right?

// eplace.cpp:958
//? does filler need localSmooth?
```

这些是阅读时的重点关注对象——作者标注疑虑的地方，往往正是与论文存在偏差之处。

---

## 5. 值得借鉴的地方

抛开缺陷，这份代码有几处设计确实值得学习，也印证了设计文档中的判断：

| 做法 | 位置 | 评价 |
| --- | --- | --- |
| ISPD 风格 CLI | `PlaceCommon/arghandler.cpp` | 与设计文档的产品形态一致 |
| 阶段跳过开关 | `-noQP` `-nomGP` `-nomLG` `-nocGP` `-noLegal` | 印证"阶段可独立启停"决策的必要性 |
| 内核单独测试 | `parser_test.cpp` `qplace_test.cpp` `binDensity_test.cpp` `ePlace_test.cpp` | 每个含独立 `main()`，可单独驱动 |
| 参考实现逐行标注 | 遍布全文的 `see RePlAce xxx.cpp line NNN` | 极大方便对照论文与参考实现，值得效仿 |
| 优化器可插拔 | `Optimization/` 下 nesterov / adam / momentum 并存 | 便于做优化器对比实验 |
| 依赖随仓库分发 | `Library/` 内置 CImg 与 Eigen | clone 即可编译，无需 apt |
| 三阶段流程设计 | mGP → FILLERONLY → cGP | 来自 ePlace-MS，比单阶段效果好 |

---

## 6. 对本项目的结论与建议

### 6.1 定位：参考基准，而非起点

**建议把 easyPlace 用作三件事：**

1. **任务 5 / 7 的算法参考**。ePlace 主循环、FFT 泊松求解、Abacus 行内二次规划这些硬骨头，
   它的实现忠实于论文与 RePlAce，且逐行标注了出处，值得读懂。
2. **正确性对拍基准**。自己实现完 ePlace 后，用同一 benchmark 跑双方，
   比较 HPWL 与 overflow 曲线。这比对着论文猜有效得多。
3. **反面教材清单**。上文所有缺陷都是自己实现时要主动规避的具体项。

**不建议在其基础上改造**，理由有三：

- 任务 4 与任务 6 的缺口恰好落在课程评分中"设计与对比分析"占比较重的部分，无论如何都得从零做；
- AoS 结构 + 零并行 + 每轮重建 FFT，决定了它跑不快，而这正是我们架构要改进的方向；
- 无合法性检查，结果无从验证。

> 另外 `baseline_/` 已在 `.gitignore` 中，本项目应独立实现，不直接复用其代码。

### 6.2 必须自建的部分

| 缺口 | 工作量 | 说明 |
| --- | --- | --- |
| 随机初始布局 | 极小 | 区域内随机撒点，作为 baseline |
| 聚类驱动初始布局 | 中 | 连接强度图 + 迭代合并 + 簇内网格摆放 |
| 三方法对比框架 | 小 | `run_matrix.py` + 统一 metrics 输出 |
| GDSII 导出 | 中 | 建议分层输出（见设计文档 §6.1） |
| 合法性检查器 | 小 | 独立二进制，不复用布局器几何代码 |
| `.wts` 解析 | 极小 | 至少留出接口 |

### 6.3 实现时的具体规避项

1. **X/Y 方向逻辑抽成带方向参数的函数**，严禁复制粘贴（§2.1）。
2. **filler 数量加下界保护**（§2.2）。
3. **密度场与密度梯度的缩放策略必须一致**，并在代码注释中锁死约定（§2.3）。
4. **打开 `-Wall -Wextra`**，不要用 `-w`；`-ffast-math` 调试期关闭（§4.1）。
5. **FFT / 求解器等重对象构造一次复用**（§3.2）。
6. **热路径全部 `const&`**，配合扁平数组布局（§3.3，对应设计文档决策 3）。
7. **线长梯度与密度统计上 OpenMP**（§3.1）。
8. **合法性检查器独立开发**（§4.2，对应设计文档决策 6）。

---

## 附录：课程资料勘误的代码级实锤

审查过程中发现，课程资料/笔记中的两处表述可以追溯到本 baseline 代码，且均为误读或与论文不符。

### A.1 filler 数量公式被误加括号

`note/B 全局布局相关.md` 写作：

> 填充节点个数 = 总填充面积 / (单个填充节点面积 + 0.5)

而代码是（`eplace.cpp:127`）：

```cpp
int fillerCount = (int)(totalFillerArea / fillerArea + 0.5);
```

按 C++ 运算符优先级，这是 `(totalFillerArea / fillerArea) + 0.5`，即**除法结果再加 0.5 后取整**，
是标准的**四舍五入**写法。资料把 `+ 0.5` 括进了分母，是误读。

两者数值差异巨大：设 `totalFillerArea = 1e6`、`fillerArea = 100`，
正确结果是 10000，误读公式给出 9950。

### A.2 "中间 90%" 源自代码而非论文

`note/B` 称 filler 尺寸取"中间 90% 的节点的面积的平均值"。

代码确实是 90%（`eplace.cpp:109-110`，取 5%~95%），但 **ePlace 论文 §3.2 原文是
mid-80%**（取 10%~90%）。代码自身注释也自相矛盾：`:109` 写 "middle 80%"，`:119` 写 "middle 90%"。

**结论**：资料中的"90%"来源是这份 baseline 实现，而非论文。实现时应以论文的 80% 为准，
或至少明确记录自己采用了哪一种。
