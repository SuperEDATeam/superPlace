# superPlace 布局查看器

基于 Dear ImGui + GLFW + OpenGL 的交互式布局查看器。

**与算法部分零耦合**：这是一个独立的顶层 CMake 工程，根目录的 `CMakeLists.txt`
不引用它。依赖方向单向——gui 可以（可选地）去找 `libsuperplace`，
算法侧永远不知道 gui 存在。因此在这里做任何改动都不可能让算法侧构建或测试失败。

> 完整设计说明、数据契约规范、已踩过的坑与开发路线见
> [work_report/06-前端开发方案与接口契约.md](../work_report/06-前端开发方案与接口契约.md)。
> **接手前请先读那一份**，尤其是 **§8.2 当前实现状态**。

## 已实现（A 类 T1–T8、T13）

独立版 `superplace-viz` 已落地以下能力（均在 `gui/` 内，不依赖算法侧库）：

| 任务 | 能力 |
| --- | --- |
| T1 | `summary.json` 健壮总览解析：缺键/损坏降级、未知键原样展示、`cgp_macros_moved` 非 0 标红、`durations_ms` 分段展示；无 summary 时显示提示不崩 |
| T2 | 窗口/面板尺寸变化时保持视野（中心不动、短边比例缩放），不重新 fit |
| T3 | 左下角 1/2/5×10ⁿ 自适应比例尺 + 底部/左侧坐标刻度（Y 向上） |
| T4 | 单击真实矩形点选（懒更新均匀网格空间索引、面积最小优先、零面积/F_NI 跳过），检视面板展示下标/类别/尺寸/中心/bin；无 `names.txt` 降级显示 |
| T5 | 按阶段分色收敛曲线、时间轴游标、点曲线跳帧；metrics 行↔抽样帧按 Exact/Nearest/Approx/Unavailable 四级诚实映射，未知阶段不跨跳，缺失值 NaN 断线不补 0 |
| T6 | 节点面积重叠密度热图（可视化面积占比，**不是**优化器 τ/local smoothing）；远景按行高像素阈值自动 LOD 切热力图 |
| T7 | 按类别 / 按局部密度 / 按帧间位移 连续色表着色 + 图例；**按 net 数因 T10 无 nets.bin 明确禁用**，不伪造 |
| T8 | 两帧 A/B 对比、位移着色、抽样位移向量（≤约 4000 支）、最大/平均位移 |
| T13 | 零依赖无头单测（`viz_tests`，不引入 gtest），覆盖相机/类别优先级/bundle 解析/空间索引/密度/帧映射/比例尺 |

**未做（本轮授权边界之外，非仅缺样本）**：T9 names.txt 名称生成（依赖算法/导出端产出
names.txt）、T10 网表连接性与 net 高亮/着色（无 `nets.bin` 时显式禁用、不伪造）、
T11 实时四阶段、T12 暂停/停止；实时模式仍只跑 mGP。

任务分级（A/B/C）与每个任务的验收标准见文档 **§9**；本轮只做 A 类。
**【C】类（T11 实时四阶段、T12 暂停/停止）不要直接动手**——
尤其 T12：`EPlace::run()` 目前没有任何中断机制，必须算法侧先加 abort 路径。

## 装依赖（一次性）

```bash
sudo apt install -y libglfw3-dev libgl1-mesa-dev libx11-dev \
                    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

> ⚠️ 不要设置 `GALLIUM_DRIVER=d3d12`：它只对 WSL2 有效，原生 Linux/软件渲染
> 环境设了反而去找不存在的 d3d12 后端。原生 Linux 有真正 GPU 驱动就用原生；
> 纯软件环境（llvmpipe）保持默认即可。

**不需要** FFTW / Eigen / libpng / OpenMP——那些是算法侧的依赖。
ImGui 与 ImPlot 已 vendor 在 `third_party/`，构建不需要联网。

还要装一个中文字体，否则界面中文显示为方块（程序会在 stderr 提示）：

```bash
sudo apt install fonts-noto-cjk               # Debian/Ubuntu
# Fedora: google-noto-sans-cjk-fonts   Arch: noto-fonts-cjk
# 或指定：export SUPERPLACE_FONT=/path/to/font.ttc
```

**其他发行版**：需要的只是 C++17 编译器、CMake ≥ 3.20、GLFW ≥ 3.3 开发包，
包名对照表见文档 §7.1b。本工程没有任何 WSL 专属依赖。

## 构建、运行与测试

```bash
# 在仓库根目录
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release
cmake --build gui/build -j4

# 运行（默认加载仓库自带合成样本）
cd gui && ./build/superplace-viz

# 无头单测（T13，零依赖、不连 GL 窗口）
./build/viz_tests
ctest --test-dir gui/build --output-on-failure

# 独立 review 套件（138 项，随验收证据包提供，不在源码包内）：
# bash gui/验收/测试记录/run_review.sh            # 默认
# STRICT=1 bash gui/验收/测试记录/run_review.sh   # 开启非法 iter 严格断言
```

| 参数 | 用途 |
| --- | --- |
| `<bundle 目录>` | 加载指定 bundle |
| `--frame N` | 启动时跳到第 N 帧（`-1` = 最后一帧） |
| `--bench N` | 渲染 N 帧并报告中位/p95 耗时后退出，用于性能回归 |
| `--screenshot out.ppm` | 保存截图（PPM） |
| `--shot-after 秒` | 跑 UI 到点自动截图退出，用于脚本化出图 |
| `--compare` | 自检：启动即开启两帧对比（A=0, B=最后一帧），便于无交互验证 T8 |

操作：左键拖拽平移，滚轮以光标为锚缩放，单击节点点选（点空白取消），
"适应窗口"按 chip 重新 fit；右侧"控制"面板切图层/着色模式/时间轴，
底部"收敛曲线"看分阶段 HPWL/overflow、"帧对比"做 A/B 位移。

## 测试数据：clone 下来只有合成样本

`test_data/`、`benchmarks/`、`results/` **全都在 `.gitignore` 里**
（125 MB / 2.7 GB / 1.2 GB）。新 clone 的仓库里唯一的数据就是
`sample-data/synthetic`，所以**实时模式一开始跑不起来**（没有任何 `.aux`）。

**第一档 · 仓库自带**：`sample-data/synthetic`，14,681 节点 / 40 帧。
覆盖全部节点形态（标准单元、可移动宏、core 外的固定 pad、零面积 `terminal_NI`、
filler）与一条完整收敛过程。**功能正确性在它上面验完基本就够了。**

**第二档 · 本地现造大样本**（测性能用，仍不需要 benchmark）：

```bash
python3 scripts/make_viz.py --synthetic --cells 160000 --macros 236 \
                            --frames 6 --out /tmp/viz/big
#  -> 357,217 节点，20 MB，1 秒内生成；实测渲染 1.55 ms/帧
cd gui && ./build/superplace-viz /tmp/viz/big --bench 150
```

**第三档 · 真实数据**：**自己下载不到**（ISPD 两个源都已失效），需要向
算法侧的人要。推荐让他直接给打包好的 bundle（newblue1 三帧 tar.gz 仅 4 MB），
而不是原始 benchmark（40 MB+）。只有要调试实时模式时才需要原始 `.aux`。

拿到真实 benchmark 后自己生成 bundle：

```bash
# 在仓库根目录
python3 scripts/make_viz.py --aux benchmarks/ispd2006/newblue1/newblue1.aux \
                            --result results/audit/newblue1 --out data/newblue1
cd gui && ./build/superplace-viz data/newblue1 --frame -1
```

## 实时模式（可选）

内嵌引擎、worker 线程逐轮推帧。前置条件有两个：算法侧已构建出
`build/libsuperplace.a`，以及它的传递依赖也已安装（静态库的依赖要由使用方链上）：

```bash
sudo apt install -y pkgconf libpng-dev libeigen3-dev libfftw3-dev

cmake -S gui -B gui/build-int -DCMAKE_BUILD_TYPE=Release -DSUPERPLACE_ROOT=$PWD/build
cmake --build gui/build-int -j
cd gui && ./build-int/superplace-gui ../test_data/adaptec1/adaptec1.aux --stride 10
```

UI 代码与独立版**完全相同**，差别只是注入了 `LiveSource` 而不是 `BundleSource`。

> ⚠️ 当前**只跑 mGP 一个阶段**，没有 mLG / FILLERONLY / cGP，也没有暂停与单步。

## 目录

| 路径 | 职责 |
| --- | --- |
| `src/viz/source.h` | `IPlacementSource` —— 前端与数据来源之间**唯一的接缝**；新能力带默认实现，兼容原有 LiveSource |
| `src/viz/bundle_source.cpp` | 离线数据源：读 viz bundle（含 summary.json） |
| `src/viz/live_source.cpp` | 实时数据源：**唯一 include 算法侧头文件的文件，本轮未改** |
| `src/viz/summary.{h,cpp}` | T1：summary.json 健壮解析、友好键名、告警判定 |
| `src/viz/spatial_index.{h,cpp}` | T4：chip 均匀网格 CSR 空间索引，真实矩形命中、面积最小优先、懒重建 |
| `src/viz/density.{h,cpp}` | T6：节点与 bin 真实面积重叠累加（可视化密度，非优化器平滑） |
| `src/viz/frame_map.{h,cpp}` | T5：metrics 行↔抽样帧诚实映射（阶段+iter 最近邻，比例兜底） |
| `src/render/canvas.{h,cpp}` | 相机 + OpenGL 实例化渲染、连续色表、热图纹理、LOD |
| `src/render/scale_tick.h` | T3：1/2/5×10ⁿ niceStep 纯逻辑 |
| `src/render/gl_loader.{h,cpp}` | 极简 GL 函数加载器 |
| `src/app.{h,cpp}` | 主循环、停靠布局、全部面板接线 |
| `test/` | T13 零依赖单测（相机/类别/bundle/索引/密度/帧映射/比例尺） |
| `sample-data/` | 合成样本，覆盖全部节点形态，可用 `make_viz.py --synthetic` 重新生成 |
| `third_party/` | vendor 的 ImGui（docking 分支）与 ImPlot |

## 已知缺口 / 边界

- **不伪造 net 数**：T10 的 `nets.bin` 缺失，"按 net 数"着色在组合框里禁用并标注。
- **T5 映射分级（诚实，不冒充精确）**：Exact（同阶段 iter 精确相等）/
  Nearest（同阶段 iter 最近邻）/ Approx（同阶段无 iter 时按顺序比例兜底）/
  Unavailable（未知阶段、无本阶段帧、解析不到）。Unavailable 返回 -1，**不跨阶段
  全局比例猜测**；游标按质量着色（精确黑/最近邻蓝/近似橙）并文字标注，无映射不画
  游标、点击不跳帧。`iter` 只认 "iter N" 词元，文件名/benchmark 编号数字不取。
- **metrics 缺失值不补 0（不造数）**：ragged 行缺列填 NaN，ImPlot 在 NaN 处断线，
  维度恒为列数×rowCount，绘图与 tooltip 不越界、不把缺失画成 0。
- **缺 stage = 未知，不继承**：空/缺阶段名存空串（未知），不沿用上一阶段、不臆造
  分段；无 `stage` 列的旧源全部记未知；空阶段行/帧不参与帧映射（Unavailable），
  曲线面板以中性灰"（未知阶段）"呈现并提示。
- **按模式跳过无效上传**：类别（Kind）模式不上传连续值缓冲 bufValue_；仅密度/位移
  着色模式上传，避免每帧无效 n 浮点传输。
- **类型/大小健壮**：`nodeCount` 等标量类型不符（如字符串）优雅拒绝；先校验
  geom.bin/flags.bin 实际字节与声明一致、并按 `kMaxNodes=64×1024×1024`（67,108,864，
  节点数上限，非内存 MiB）封顶后才分配，杜绝超大声明先分配再 bad_alloc；关闭的图层
  取消选中且不参与点选。
- **名称待 T9**：bundle 无 `names.txt` 时检视面板显示"待 T9"，不编造名字。
- **实时模式仍只跑 mGP**（T11/T12 未做，需算法侧配合）。
- LOD 阈值按标准单元行高的屏幕像素判定；密度场 128×128，固定分辨率，非多 mip。
- 合成样本无 `summary.json`，T1 总览需带该文件的 bundle 才能目视（逻辑已单测覆盖）。

## 实现状态 vs 验收状态（分开陈述）

- **已实现并有自动/静态证据**：T1、T3–T8、T13；产品无头单测 460 项 0 失败、
  ctest 1/1；独立 review 断言 138 项 0 失败（完整独立套件随验收证据包提供，
  源码包不内置旧版独立套件）；26 个畸形/边界夹具优雅拒绝；比例尺/刻度限定在画布矩形内。
- **R4 迭代号边界**：iter 列值用 double 与 INT_MAX 比较，仅有限非负整数可转换；
  超 INT_MAX/负数/小数/NaN/Inf 判明确非法并 Unavailable，不截断、不按比例兜底；
  混合时只在合法/缺 metadata 行选；未使用常量 kTok 已清除。
- **未完成的真实桌面交互复验**：T2 局部 resize 像素级锚点、R2-1 手工关层复验受当前
  原生桌面交互环境限制。
- **性能（R3 历史数据，保留风险，不宣称完全通过）**：R3 bench150 独立同命令 ABAB，
  大样本中位 A=172.92 / B=175.74 ms（+1.63%，接近），但 p95 A=188.48/182.20、
  B=212.79/214.42 ms，两轮持续偏高约 13~18%；合成中位同向偏高（+33.43%/+7.79%）。
  R4 未重跑 bench，R4 二进制不冒充新实测；软渲染波动因果未定，保留尾延迟风险。
- **可用性残留（非缺陷，本轮不改产品）**：对比首末帧位移向量密集会遮挡局部布局，
  可关闭向量显示或改选相邻帧比较缓解。

## 边界

**只改 `gui/` 下的文件。** `src/`、`test/`、`CMakeLists.txt`、`configs/`
属于算法侧，不要碰。`scripts/make_viz.py` 是双方共享的契约，改之前先商量。
