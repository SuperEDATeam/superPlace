# 07 · 大规模 Benchmark 测试报告（2026-10-24）

> 范围：ISPD2005/2006 全套 16 设计的 CLI 矩阵跑批 + 标准构建修复 + 算法 CTest + GUI/T11 四阶段对拍。
> 本报告只记录精炼事实，不改产品 / 测试 / 结果文档；原始产物在 `results/full_matrix_2026-10-24/`（已 gitignore，不随报告提交）。
> 独立事实核验已通过（见文末「遗留 WARN」）。

---

## 1. 环境与依赖

本轮在干净依赖前缀下完成标准构建。系统侧编译器与构建链：

| 组件 | 版本 | 用途 |
| --- | --- | --- |
| g++ | 11.4.0（C++17，OpenMP 4.5 随附） | 编译 |
| CMake / Ninja / ccache | 系统包 | 构建后端；ccache 自动启用 |
| libpng / zlib | 1.6.37 / 1.2.11 | 可视化 PNG 落盘 |
| FFTW3（单精度 `fftw3f`） | 3.3.10 | DCT / 泊松求解（M2′、M3 ePlace 密度项） |
| Eigen3 | 3.4.0 | 稀疏矩阵 + BiCGSTAB（M2 二次布局） |
| GLFW | 3.3.9（官方 release tag 源码构建） | GUI 窗口与 OpenGL 上下文；本机无系统 `libglfw3-dev`，按 Home 前缀自建 |

依赖前缀（复现时按本机 Home 路径设置，不写绝对路径）：

```bash
export CMAKE_PREFIX_PATH="$HOME/.local:$HOME/.local/eigen"
export PKG_CONFIG_PATH="$HOME/.local/fftw-single/lib/pkgconfig"
export LD_LIBRARY_PATH="$HOME/.local/fftw-single/lib:$HOME/.local/lib"
```

仓库自带（vendor，无需安装）：`third_party/CImg.h`、`third_party/nlohmann/json.hpp`。

---

## 2. 标准构建修复

- **失败现象**：干净树首次 `cmake --build` 在 `src/db/hpwl.cpp` 编译期即断——`src/numeric/reduction.h` 用到 `size_t` 但未 `<include <cstddef>>`，gcc 11.4 报 `‘size_t’ does not name a type`（提示「did you forget to `#include <cstddef>`?」）。
- **一行修复**：在 `src/numeric/reduction.h` 顶部补 `#include <cstddef>`。提交 `e95fc2e fix(numeric): include cstddef for size_t`，已推 PR。
- **修复后**：标准构建 54/54 目标全部成功（`libsuperplace.a`、`superplace-cli`、`superplace-quiz`、11 个测试可执行文件均链接通过）。

复现命令（通用标准 build）：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cd build && ctest --output-on-failure
```

---

## 3. 算法 CTest

- 修复后 CTest：**11/12 通过**，唯一失败为 `test_csr`（Subprocess aborted）。
- **缺口**：`test_csr` 启动即抛 `cannot open: test_data/thin1/thin1.aux`——`test_data/thin1/` 测试数据未入库，非算法逻辑错误。补齐该夹具后即可恢复 12/12。
- 其余 11 项（parser / roundtrip / poisson / hpwl / b2b / nesterov / wirelength / density / filler / macro_sa / eplace）全部通过，断言计数见 `precheck/07_ctest_fixed.log`。

---

## 4. CLI 矩阵跑批口径

- 二进制：修复后标准构建产出的 `superplace-cli`；配置 `configs/default.json`。
- 命令（逐设计一致，严格串行，无并发）：

  ```bash
  ./build/superplace-cli --aux benchmarks/<suite>/<design>/<design>.aux \
      --config configs/default.json --out results/full_matrix_2026-10-24/_cli_m1 \
      --stage parse,init,gp --seed 1002 --threads 4 --no-plot
  ```

- 硬口径：`--stage parse,init,gp`、`--seed 1002`、`--threads 4`、`--no-plot`、wrapper 超时 **1800s**、严格串行。
- 实跑 16 设计 = ISPD2005 × 8 + ISPD2006 × 8，`.aux` 全部在位。
- **MMS 缺口**：`benchmarks/mms/`（混合尺寸重建数据集）在本轮环境中不存在，本轮矩阵未使用 MMS；含可移动宏的 3 个设计均来自官方 ISPD2005/2006 原始数据（newblue1=64、newblue2=3723、bigblue3=2485 个可移动宏）。bigblue3 类残留问题是否在真 MMS 数据上复现，属未覆盖项。

---

## 5. 矩阵总览

- **16/16 设计 exitcode=0**，无超时、无 OOM、无 out-of-core（`gp_out_of_core` 全 16 设计 = 0）。
- 总 wall = **3688.3 s ≈ 61.471 min**；最大峰值 RSS = **newblue7 1986.9 MB**；次大为 bigblue4 1752.9 MB / wall 1012.4 s（< 1800 s 阈值）。
- **口径说明**：16 个流程均 exit 0、产物齐备（`.pl` / `metrics.csv` / `summary.json` / `time.json` 等），但这**不等于「全部通过硬验收」**——bigblue3 的 mLG 残留重叠违反硬验收，见 §7。
- `metrics_rows` 列口径 = `metrics.csv` **数据行数（不含表头）**，已与原件逐行复核 16/16 一致（`metrics_total_lines = metrics_rows + 1`）。

### 5.1 十六设计逐行表

overflow 列：两阶段设计取 mGP 终值 τ，四阶段设计取 cGP 终值 τ（目标 ≤ 0.10；cGP 子目标 ≤ 0.07）。HPWL 为原始全精度值。

| 设计 | run | status | wall_s | RSS_MB | objects | nets | pins | 可移动宏 | 实际阶段 | initHPWL | finalHPWL | overflow | OOC | 产物 |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: | --- |
| adaptec1 | m1 | OK | 26.988 | 228.4 | 211,447 | 221,142 | 944,053 | 0 | init,mGP | 41,003,221.71 | 71,867,373.75 | 0.0978 | 0 | 齐 |
| adaptec2 | m1 | OK | 56.039 | 280.2 | 255,023 | 266,009 | 1,069,482 | 0 | init,mGP | 50,041,396.46 | 84,126,047.35 | 0.0958 | 0 | 齐 |
| bigblue1 | m1 | OK | 40.065 | 273.6 | 278,164 | 284,479 | 1,144,691 | 0 | init,mGP | 33,950,784.86 | 88,550,065.06 | 0.0975 | 0 | 齐 |
| newblue1 | run1 | OK | 55.446 | 286.4 | 330,474 | 338,901 | 1,244,342 | 64 | init,mGP,FILLERONLY,cGP | 12,206,819.10 | 75,167,609.79 | 0.0681 | 0 | 齐 |
| newblue2 | m1 | OK | 225.438 | 410.9 | 441,516 | 465,219 | 1,773,855 | 3,723 | init,mGP,FILLERONLY,cGP | 109,785,730.80 | 171,241,725.50 | 0.0687 | 0 | 齐 |
| adaptec3 | m1 | OK | 100.608 | 449.7 | 451,650 | 466,758 | 1,875,039 | 0 | init,mGP | 122,553,634.00 | 211,331,639.60 | 0.0955 | 0 | 齐 |
| adaptec4 | m1 | OK | 103.178 | 523.8 | 496,045 | 515,951 | 1,912,420 | 0 | init,mGP | 133,286,357.30 | 174,415,480.60 | 0.0985 | 0 | 齐 |
| newblue3 | m1 | OK | 71.962 | 556.2 | 494,011 | 552,199 | 1,929,892 | 0 | init,mGP | 391,002,971.30 | 284,345,639.50 | 0.0966 | 0 | 齐 |
| bigblue2 | m1 | OK | 97.780 | 485.0 | 557,866 | 577,235 | 2,122,282 | 0 | init,mGP | 105,508,372.70 | 135,478,396.20 | 0.0986 | 0 | 齐 |
| newblue4 | m1 | OK | 130.406 | 580.6 | 646,139 | 637,051 | 2,499,178 | 0 | init,mGP | 116,035,732.40 | 228,564,992.80 | 0.0956 | 0 | 齐 |
| adaptec5 | m1 | OK | 219.018 | 812.6 | 843,128 | 867,798 | 3,493,147 | 0 | init,mGP | 160,180,503.60 | 295,124,150.30 | 0.0975 | 0 | 齐 |
| bigblue3 | m1 | OK | 269.606 | 776.4 | 1,096,812 | 1,123,170 | 3,833,218 | 2,485 | init,mGP,FILLERONLY,cGP | 217,113,497.30 | 318,511,412.40 | 0.0693 | 0 | 齐 ⚠ |
| newblue5 | m1 | OK | 233.051 | 987.0 | 1,233,058 | 1,284,251 | 4,957,843 | 0 | init,mGP | 233,039,757.20 | 370,641,290.00 | 0.0982 | 0 | 齐 |
| newblue6 | m1 | OK | 575.134 | 1,070.7 | 1,255,039 | 1,288,443 | 5,307,594 | 0 | init,mGP | 285,757,210.70 | 587,613,611.50 | 0.0962 | 0 | 齐 |
| bigblue4 | m1 | OK | 1,012.373 | 1,752.9 | 2,177,353 | 2,229,886 | 8,900,078 | 0 | init,mGP | 660,450,064.80 | 758,520,094.70 | 0.0979 | 0 | 齐 |
| newblue7 | m1 | OK | 471.174 | 1,986.9 | 2,507,954 | 2,636,820 | 10,104,920 | 0 | init,mGP | 931,706,542.70 | 965,236,538.10 | 0.0987 | 0 | 齐 |

⚠ = exit 0、产物齐，但 mLG 残留重叠违反硬验收（§7）。

---

## 6. 四阶段执行情况（仅 3 个设计触发）

四阶段 = `init → mGP → mLG（宏合法化，一次性跳变）→ FILLERONLY → cGP`。其中 **mLG 是 mGP 与 FILLERONLY 之间的一次性 milestone**：它不逐轮回调、**不写 metrics.csv 行**，因此 metrics 的 stage 列只出现 `init / mGP / FILLERONLY / cGP` 四类；mLG 的结果以 summary 中的 `mlg_*` 键（重叠前后、位移、修复次数）单独落盘。本轮**仅 3 个设计**含可移动宏而走完四阶段；其余 **13 个设计 `movable_macros=0`**，mLG / FILLERONLY / cGP 按数据特性跳过（三自证：summary 中 `gp_movable_macros=0` + 日志「无可移动宏…跳过」行 + 无 `mlg_*` / `cgp_*` 键）。**这 13 个不宣称四阶段通过。**

| 设计 | 可移动宏 | mGP 轮 | mLG 前重叠 | mLG 后重叠 | FILLERONLY 轮 | cGP 轮 | cGP 终值 τ | cGP 期间宏位移 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| newblue1 | 64 | 471 | 46,812 | **0** | 20 | 101 | 0.0681（<0.07） | 0 |
| newblue2 | 3,723 | 320 | 1,138,416 | **0** | 20 | 98 | 0.0687（<0.07） | 0 |
| bigblue3 | 2,485 | 451 | 1,874,268 | **1,656** ⚠ | 20 | 77 | 0.0693（<0.07） | 0 |

- newblue1 HPWL 链：12,206,819.10（init）→ 92,969,492.21（mGP）→ 92,988,677.93（mLG）→ 75,167,609.79（cGP）。
- newblue1 首跑环境插曲：首次用系统 `superplace-cli` 启动即 exit 127，报 `libfftw3f.so.3: cannot open shared object file`；按 §1 补 `LD_LIBRARY_PATH` 后 run1 成功，矩阵终值取 run1。属环境依赖未就位，非算法缺陷，留痕于此。

---

## 7. 硬验收失败单列：bigblue3 mLG 残留重叠

- bigblue3 `mLG` 后宏重叠 **`mlg_overlap_after = 1656`**（前值 1,874,268 → 经贪心修复 885 次后残留 1656；宏平均位移 24.17 / 最大位移 8946.6）。
- 运行日志自披露该残留「占宏总面积 0.0180%」；后续 FILLERONLY(20) 与 cGP(77) 正常跑完，`cgp_macros_moved=0`、`cgp_τ=0.0693 < 0.07` 已收敛，**非崩溃、非静默吞错**。
- 但它违反 05 开发执行规范 §5.6.3「宏合法化后重叠 = 0」的硬验收判据。
- 对照：同为双高单元为主的 newblue2（3723 宏）mLG 后重叠压到 **0**；bigblue3 的 `core_density = 85.6%`，远高于 newblue2，密度/拥挤度差异可能是残留成因。
- **结论**：bigblue3 不能计入「四阶段硬验收通过」。需算法侧就高拥挤度下 mLG SA 贪心修复的残留给出解释或修法。newblue1 / newblue2 两个四阶段设计 mLG 后重叠均为 0。

---

## 8. GUI / T11 四阶段实时对拍

- **集成构建**：`superplace-gui` 与 `viz_live_tests` 在同一依赖前缀下配置 + 构建成功，`--help` 正常。
- **T11 对拍**：无头 harness（复用与 `superplace-gui` 同一份 `live_source.cpp`）在真实 newblue1（64 可移动宏）上跑四阶段，stride=10，seed=1002，threads=4。
  - EXIT=0，WALL=54.43 s，PEAK_RSS=542.1 MB。
  - 最终 HPWL：Live 全精度 = `75,167,609.794921875`；CLI summary = `75,167,609.79`；相对误差 **6.55e-11 ≤ 1e-6 → PASS**（实质逐位一致）。
  - Live frame counts（共 64 帧，符合 stride=10 抽样）：init=1、mGP=48（471 轮抽样）、mLG=1（一次性跳变，单帧、label 不含 iter）、FILLERONLY=2（20 轮抽样）、cGP=11（101 轮抽样）、done=1。帧序 mGP(1) < mLG(49) < FILLERONLY(50) < cGP(52) 正确。
  - **宏冻结**：宏节点数 = 64；mLG 帧 → done 帧宏位置移动数 = **0**（与 CLI `cgp_macros_moved=0` 一致）。
- **viz_live_tests**：CMake 版 821 项检查 / 0 失败（断言覆盖四阶段顺序、mLG 单帧、宏冻结、geom 索引逐节点一致）。该结果已经独立复跑确认（同一测试二进制与真实 `gui/test/fixtures/tiny_macros/` 夹具），821/0。

---

## 9. 原始产物位置与复现

- 本轮全部原始产物位于项目相对路径 `results/full_matrix_2026-10-24/`（含 16 个 `<design>__<run>/` 目录、`matrix_summary.csv`、`index_final.jsonl`、`precheck/`、`t11/`）。该目录已 gitignore，**不随报告提交大文件**。
- 复现：先按 §1 设依赖前缀、按 §2 标准 build，再按 §4 命令逐设计串行跑批；T11 对拍步骤见 `results/full_matrix_2026-10-24/t11/BASIS.md`。

---

## 10. 遗留 WARN（记录，不阻塞本轮）

- **W1（配置耦合警告）**：LiveSource 构造不加载 `configs/default.json`，而是走 `sp::Config` 结构体默认值；本轮逐项核对与 default.json 一致仅为「C++ 默认值 == JSON」的巧合性一致。日后若只改 JSON 调参，Live/GUI 侧会静默偏离 CLI。建议后续把 LiveSource 改为显式加载同一份配置。
- ~~W2（测试计数声明）~~ **已关闭**：`viz_live_tests` 821/0 已经独立复跑确认（同一测试二进制 + 真实 tiny 夹具），见 §8。

---

## 自查表

| 项 | 值 |
| --- | --- |
| 设计数 / exit 0 | 16 / 16 |
| 总 wall | 3688.3 s ≈ 61.471 min |
| 最大峰值 RSS | 1986.9 MB（newblue7） |
| out_of_core | 全 16 = 0 |
| 四阶段触发 | 3（newblue1、newblue2、bigblue3） |
| mLG 后重叠 = 0 | newblue1、newblue2 |
| 硬验收失败 | bigblue3（mLG 残留 1656） |
| 算法 CTest | 11/12（thin1 夹具缺失） |
| 标准构建 | 修复后 54/54，commit e95fc2e |
| T11 HPWL 相对误差 | 6.55e-11 ≤ 1e-6 PASS |
