# superPlace 布局查看器

基于 Dear ImGui + GLFW + OpenGL 的交互式布局查看器。

**与算法部分零耦合**：这是一个独立的顶层 CMake 工程，根目录的 `CMakeLists.txt`
不引用它。依赖方向单向——gui 可以（可选地）去找 `libsuperplace`，
算法侧永远不知道 gui 存在。因此在这里做任何改动都不可能让算法侧构建或测试失败。

> 完整设计说明、数据契约规范、已踩过的坑与开发路线见
> [work_report/06-前端开发方案与接口契约.md](../work_report/06-前端开发方案与接口契约.md)。
> **接手前请先读那一份**，尤其是 **§8.2 当前实现状态**。

## ⚠️ 这是骨架，不是成品

当前**功能完成度约两成**。已能用的是：bundle 加载、实例化渲染、平移缩放、
图层开关、时间轴播放、一张粗糙的收敛曲线、实时模式（**只跑 mGP 一个阶段**）。

**没做的占绝大多数**——点选与检视（连空间索引都没建）、网表高亮、密度热力图、
属性着色、帧对比、窗口 resize 自适应、快捷键，以及**一个测试都没有**。
还有两处横跨契约两侧的死代码（`summary.json` 从没被读过、`names.txt` 有读取端
没有生成端），适合拿来上手。

完整工作清单（13 个任务，每个都带做法、坑与验收标准）见文档 **§9**。

任务按**会动到什么**分三级，顺序也是按这个排的：

| 级别 | 含义 | 规则 |
| --- | --- | --- |
| **【A】** | 只动 `gui/` | 自行推进，不必打招呼 |
| **【B】** | 要改 `scripts/make_viz.py`（共享契约） | **改前知会**，`--verify` 把关 |
| **【C】** | 涉及算法语义，或要改算法侧代码 | **开发前必须与算法侧确认方案** |

前两批（T1–T8）全是【A】，可以完全自主推进一到两周。
**【C】类（T11 实时四阶段、T12 暂停/停止）请不要直接动手**——
尤其 T12：`EPlace::run()` 目前没有任何中断机制，必须算法侧先加 abort 路径。

## 装依赖（一次性）

```bash
sudo apt install -y libglfw3-dev libgl1-mesa-dev libx11-dev \
                    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
echo 'export GALLIUM_DRIVER=d3d12' >> ~/.bashrc && source ~/.bashrc
```

**不需要** FFTW / Eigen / libpng / OpenMP——那些是算法侧的依赖。
ImGui 与 ImPlot 已 vendor 在 `third_party/`，构建不需要联网。

> ⚠️ `GALLIUM_DRIVER=d3d12` **只对 WSL2 有效，原生 Linux 上不要设**。
> WSL2 默认走 `llvmpipe` 软件光栅化，33 万个矩形会卡成幻灯片，切过去才是硬件加速；
> 原生 Linux 有真正的 GPU 驱动，设了反而会去找不存在的 d3d12 后端。

还要装一个中文字体，否则界面中文显示为方块（程序会在 stderr 提示）：

```bash
sudo apt install fonts-noto-cjk               # Debian/Ubuntu
# Fedora: google-noto-sans-cjk-fonts   Arch: noto-fonts-cjk
# 或指定：export SUPERPLACE_FONT=/path/to/font.ttc
```

**其他发行版**：需要的只是 C++17 编译器、CMake ≥ 3.20、GLFW ≥ 3.3 开发包，
包名对照表见文档 §7.1b。本工程没有任何 WSL 专属依赖。

## 构建与运行

```bash
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release    # 在仓库根目录执行
cmake --build gui/build -j
cd gui && ./build/superplace-viz                        # 默认加载合成样本
```

| 参数 | 用途 |
| --- | --- |
| `<bundle 目录>` | 加载指定 bundle |
| `--frame N` | 启动时跳到第 N 帧（`-1` = 最后一帧） |
| `--bench N` | 渲染 N 帧并报告耗时后退出，用于性能回归 |
| `--screenshot out.ppm` | 保存截图 |
| `--shot-after 秒` | 跑 UI 到点自动截图退出，用于脚本化出图 |

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
| `src/viz/source.h` | `IPlacementSource` —— 前端与数据来源之间**唯一的接缝** |
| `src/viz/bundle_source.cpp` | 离线数据源：读 viz bundle |
| `src/viz/live_source.cpp` | 实时数据源：**唯一 include 算法侧头文件的文件** |
| `src/render/canvas.{h,cpp}` | 相机 + OpenGL 实例化渲染（一次 draw call 画完全部单元） |
| `src/render/gl_loader.{h,cpp}` | 极简 GL 函数加载器 |
| `src/app.{h,cpp}` | 主循环、停靠布局、面板 |
| `sample-data/` | 合成样本，覆盖全部节点形态，可用 `make_viz.py --synthetic` 重新生成 |
| `third_party/` | vendor 的 ImGui（docking 分支）与 ImPlot |

## 边界

**只改 `gui/` 下的文件。** `src/`、`test/`、`CMakeLists.txt`、`configs/`
属于算法侧，不要碰。`scripts/make_viz.py` 是双方共享的契约，改之前先商量。
