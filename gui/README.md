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

完整的 14 条工作清单见文档 §8.2。

## 装依赖（一次性）

```bash
sudo apt install -y libglfw3-dev libgl1-mesa-dev libx11-dev \
                    libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
echo 'export GALLIUM_DRIVER=d3d12' >> ~/.bashrc && source ~/.bashrc
```

**不需要** FFTW / Eigen / libpng / OpenMP——那些是算法侧的依赖。
ImGui 与 ImPlot 已 vendor 在 `third_party/`，构建不需要联网。

> `GALLIUM_DRIVER=d3d12` 不是可选项。WSL2 默认走 `llvmpipe` 软件光栅化，
> 33 万个矩形会卡成幻灯片；切过去之后是 RTX 4060 的 D3D12 硬件加速。

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

## 换真实数据

```bash
# 在仓库根目录
python3 scripts/make_viz.py --aux benchmarks/ispd2006/newblue1/newblue1.aux \
                            --result results/audit/newblue1 --out /tmp/viz/newblue1
cd gui && ./build/superplace-viz /tmp/viz/newblue1 --frame -1
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
