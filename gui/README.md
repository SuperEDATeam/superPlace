# superPlace 布局查看器

基于 Dear ImGui + GLFW + OpenGL 的交互式布局查看器。

**与算法部分零耦合**：这是一个独立的顶层 CMake 工程，根目录的 `CMakeLists.txt`
不引用它。依赖方向单向——gui 可以（可选地）去找 `libsuperplace`，
算法侧永远不知道 gui 存在。因此在这里做任何改动都不可能让算法侧构建或测试失败。

> 完整设计说明、数据契约规范、已踩过的坑与开发路线见
> [work_report/06-前端开发方案与接口契约.md](../work_report/06-前端开发方案与接口契约.md)。
> **接手前请先读那一份**，尤其是 **§8.2 当前实现状态**。

## 已实现（A 类 T1–T8、T13、T14、T15、T17）

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
| T14 | **连续回放**：按全局 t 线性插值、相邻 t 相等或 `breakBefore` 硬切、`iter==-1` 里程碑停留 0.5s、缺 t 退化为帧序；播放/暂停/调速（轮/秒）/循环/关键帧与阶段前后跳转/拖动 scrub；统计取最近关键帧、停下按插值重算并诚实标注；热力图固定标尺 |
| T15 | **FrameView 共享零拷贝所有权**：`shared_ptr<const vector<float>>` 持数据，LRU 淘汰只放缓存引用不影响持帧者；`frameInfo()` 只读元数据不读位置文件，消除启动读盘 |
| T17 | **后台预读**：worker 线程按播放方向预读 1–3 帧，缓存访问统一锁，析构可靠 join；无头单测覆盖预读正确与反复开/关不崩 |

**已完成（本轮）**：T14、T15、T17，以及 **T16**——整体移除旧实时集成模式
（旧内嵌引擎数据源、对应可执行目标与其无头测试、配套 fixture、以及把算法构建目录
指进来的 CMake 分支与轮询接口），本工程始终零算法依赖。
**注意：百万级（1M 节点）持续播放经 LOD 跳插值 + 关键帧密度后台预计算优化后 warm 段已达标**
（≈5.4ms/帧，冷启动预热如实计入），详见后文「性能」段。

**未做（仍属授权边界之外）**：T9 names.txt 名称生成（依赖算法/导出端产出 names.txt）、
T10 网表连接性与 net 高亮/着色（无 `nets.bin` 时显式禁用、不伪造）。

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
# 在仓库根目录。GLFW 若装在用户前缀（如 ~/.local），需指定 CMAKE_PREFIX_PATH：
cmake -S gui -B gui/build -DCMAKE_PREFIX_PATH=/home/user/.local -DCMAKE_BUILD_TYPE=Release
cmake --build gui/build -j4

# 运行（GLFW 在 ~/.local 时还需 LD_LIBRARY_PATH）
export LD_LIBRARY_PATH=/home/user/.local/lib:$LD_LIBRARY_PATH
cd gui && ./build/superplace-viz

# 无头单测（T13/T15/T17，零依赖、不连 GL 窗口）
./build/viz_tests
ctest --test-dir gui/build --output-on-failure

# 连续回放基准（T14：真正循环播放 + 插值，不是逐帧硬跳）
./build/superplace-viz <bundle> --bench 150

# 脚本化截图（无交互）：--frame 指定关键帧，--shot-after N 秒后自动截图退出
./build/superplace-viz <bundle> --frame 30 --shot-after 4 --screenshot /tmp/f30.ppm
```

| 参数 | 用途 |
| --- | --- |
| `<bundle 目录>` | 加载指定 bundle |
| `--frame N` | 启动时跳到第 N 个关键帧（`-1` = 最后一帧） |
| `--bench N` | 微基准：不开 ImGui，只连续 N 帧画布矩形/LOD 渲染并报告中位/p95 |
| `--bench-ui N` | **真实 UI 端到端基准**：跑完整 drawUi/ImGui/LOD/热图/面板 N 帧并循环，报告 mean/p50/p95/>33ms 帧数/循环数 |
| `--bench-csv FILE` | 配合 `--bench-ui`，把每帧 frame,render_ms,swap_ms,globalT,lo,hi,alpha,playing 写入 FILE 供独立复算 |
| `--screenshot out.ppm` | 保存截图（PPM，可转 PNG 查看） |
| `--shot-after 秒` | 跑 UI 到点自动截图退出，用于脚本化出图 |
| `--compare` | 自检：启动即开启两帧对比（A=0, B=最后一帧），便于无交互验证 T8 |

操作：左键拖拽平移，滚轮以光标为锚缩放，单击节点点选（点空白取消），
"适应窗口"按 chip 重新 fit；右侧"控制"面板切图层/着色模式/回放（播放暂停、
关键帧/阶段跳转、循环、速度滑条、阶段色块），底部"收敛曲线"看分阶段
HPWL/overflow 并双向联动游标、"帧对比"做 A/B 位移。

## 数据与目录

**测试数据**：上游 `test_data/` 已随仓库入库（以仓库实际内容为准）；`results/`、截图/日志在
`.gitignore` 里，不提交。内置合成样本 `sample-data/synthetic`（14,681 节点 / 40 帧）
覆盖全部节点形态。真实 bundle（如 newblue1）由导出侧提供；离线生成走 `scripts/make_viz.py --aux ...`。

| 路径 | 职责 |
| --- | --- |
| `src/viz/source.h` | `IPlacementSource` —— 前端与数据来源之间**唯一的接缝**；新能力带默认实现 |
| `src/viz/bundle_source.cpp` | 离线数据源：读 viz bundle、summary、scene.json 的 iter/t/breakBefore、T17 预读线程 |
| `src/viz/playback.{h,cpp}` | T14 回放控制器（纯逻辑、无头可测）：全局 t 插值/硬切/里程碑停留/速度缩放 |
| `src/viz/summary.{h,cpp}` | T1：summary.json 健壮解析、友好键名、告警判定 |
| `src/viz/spatial_index.{h,cpp}` | T4：chip 均匀网格 CSR 空间索引，真实矩形命中、面积最小优先、懒重建 |
| `src/viz/density.{h,cpp}` | T6：节点与 bin 真实面积重叠累加（可视化密度，非优化器平滑） |
| `src/viz/frame_map.{h,cpp}` | T5：metrics 行↔抽样帧诚实映射（阶段+iter 最近邻，比例兜底） |
| `src/render/canvas.{h,cpp}` | 相机 + OpenGL 实例化渲染、连续色表、热图纹理、LOD |
| `src/render/scale_tick.h` | T3：1/2/5×10ⁿ niceStep 纯逻辑 |
| `src/render/gl_loader.{h,cpp}` | 极简 GL 函数加载器 |
| `src/app.{h,cpp}` | 主循环、停靠布局、全部面板接线、T14 回放集成 |
| `test/` | T13/T15/T17 零依赖单测 `viz_tests`（相机/类别/bundle/索引/密度/帧映射/比例尺/FrameView 生命周期/回放/预读） |
| `sample-data/` | 合成样本，覆盖全部节点形态，可用 `make_viz.py --synthetic` 重新生成 |
| `third_party/` | vendor 的 ImGui（docking 分支）与 ImPlot |

## 已知缺口 / 边界

- **不伪造 net 数**：T10 的 `nets.bin` 缺失，"按 net 数"着色在组合框里禁用并标注。
- **T16 已移除旧实时集成模式**：旧内嵌引擎数据源、实时可执行目标、轮询接口与
  "把算法构建目录指进来"的 CMake 分支整体删除；grep `gui/` 上述旧符号零命中。
- **T15 防串帧**：FrameView 持 `shared_ptr`，取帧后再挤出 ≥25 帧缓存数据不变（有红测）；
  启动/取标签只读 `frameInfo()` 元数据，不读位置文件。
- **T14 统计诚实**：播放/拖动中懒统计锚定最近关键帧并标注；停下按插值位置重算（`statsFromInterp_`）；
  热力图用固定标尺 `heatmapScale_=1.0`，不随每帧 maxNorm 自适应（否则颜色盖过真实扩散）。
- **名称待 T9**：bundle 无 `names.txt` 时检视面板显示"待 T9"，不编造名字。
- **类型/大小健壮**：先校验 geom.bin/flags.bin 实际字节与声明一致、并按 `kMaxNodes` 封顶后才分配；
  关闭的图层取消选中且不参与点选。

## 实现状态 vs 验收状态（分开陈述）

- **已实现并有自动证据**：T1、T3–T8、T13、T14、T15、T16、T17；无头单测 **590 项 0 失败**
  （设 `VIZ_REF_BUNDLE`/`VIZ_REF_JSON` 带密度真值时 **606 项 0 失败**）、
  ctest 1/1 通过；干净目录重建（删 build 后重新 configure）成功。
- **T15 红测**：旧实现取第 3 帧后再取 26 帧挤出 24 帧 LRU，断言原数据逐位不变——
  旧实现失败（读出 `25000+i` 而非 `3000+i`，证明缓冲被别帧复用）；修复后 0 失败。
  红测源码在 `gui/test/test_frameview_lifetime.cpp`。
- **真实数据验证**：newblue1（63 帧/330,474 节点/t=0..598/breakBefore@1,51）截图帧 30/61
  形态明显不同、中文 Noto CJK 渲染正常；密度真值对照帧 3/20/30/61 的 maxNorm/nonzero/top bin
  与独立 JSON 完全一致（非截图 md5）。
- **性能优化机制（相对优化前对照）**：优化前旧版 2.2M 节点 mean≈69.9ms/p95≈92ms（≈14fps），
  其中密度重建 mean≈44.6ms 占约 69%（独立 instrumented 对照 CSV）。本轮：
  1. 关键帧密度后台预计算为独立可测类 `viz::DensityCache`：每关键帧 128×128 场≈64KB，
     预算上限 1024 帧≈64MiB，超限退化当场 build；mutex 发布、worker 异常/失败帧不 terminate、
     stop+join 可靠，预热成本如实计入冷启动段。
  2. 位置始终 materialize（保证暂停/点选/近景精确），仅 LOD 且播放时跳 GL 上传并置脏，
     近景同帧补传（`positionsValid_` 帧加载失败时不画旧 buffer、状态提示"帧加载失败"）。
  3. profiler 口径：CSV 列 frameAcq(读帧含 LRU 等待)/lerp(插值循环)/upload(GL setPositions)/
     density/draw/swap，字段名与代码段一一对应。
- **实测（llvmpipe 软渲染 --bench-ui，独立最终证据 results/gui-playback-optimization/review_final/，
  未覆写任何 baseline/final CSV）**：
  - newblue1 4000 帧 mean 8.49ms/p95 10.21ms（基线 9.93ms，无退化）。
  - 1M 合成 1000 帧 37 循环：**ALL mean 17.94ms（≈56fps）、p95 26.80ms、max 94.24ms，
    18/1000 帧 >33.3ms，playing 100%——达持续播放 ≥30fps 目标**。
    - COLD（前 49 帧预热）mean 30.30ms/p95 88.52ms，15 长帧；预热 UI 观测累计 1.511s、
      ready=30 首次在第 50 帧——如实计入未藏。
    - WARM（951 帧）mean 17.30ms/p95 25.54ms/max 38.69ms，仅 3 个 IO 长帧，软磁盘环境残余风险。
    分相 frameAcq（17MB/帧磁盘 IO）为主成本，lerp/upload/density(warm)≈0；统计语义保持最近关键帧
    锚定、停止按插值重算，未降样。真实 GPU 目标见文档 §9。

## 边界

**并发与 TSan**：`DensityCache` 的字段/ready/done 由 mutex 保护，worker 异常/失败帧不 terminate、
stop+join 可靠；`bundle_source` 的帧缓存态统一由 mu_ 保护。当前 GCC11 TSan 在 condition_variable
wait_for 路径报告 double-lock；该告警在「仅加锁 stop」的纯 std 最小程序中同样复现，判为工具/环境限制，
普通测试与锁协议人工复核通过，不宣称 TSan 全绿。

**只改 `gui/` 下的文件。** 不碰算法侧 `src/`、`test/`、根 `CMakeLists.txt`、`configs/`、
`scripts/make_viz.py`、`work_report/`。本工程始终零算法依赖（FFTW/Eigen/PNG/OpenMP 一概不链）。
