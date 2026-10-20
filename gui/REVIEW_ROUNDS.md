# 前端查看器 A 类任务（T1–T8、T13）交接与轮次记录

> 本轮为**初版（Round 1）**，交独立检查者审查；后续 review 意见按轮次追加在文末。
> 代码全部留在 `gui/`，未 commit/push/PR/merge；未改根 `src/`、`test/`、根
> `CMakeLists.txt`、`configs/`、`scripts/make_viz.py`、`work_report/`、
> `gui/src/viz/live_source.cpp`。

## 1. 可构建点（检查者直接复现）

```bash
# 仓库根目录
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release
cmake --build gui/build -j4

# 无头单测（T13，零依赖、不开窗口）
gui/build/viz_tests                 # 373 项检查，0 失败
ctest --test-dir gui/build --output-on-failure   # 1/1 通过

# 交互运行（默认合成样本 14681 节点 / 40 帧）
cd gui && ./build/superplace-viz
```

自检参数（均为只读、不改数据）：

| 命令 | 用途 |
| --- | --- |
| `./build/superplace-viz --frame 39 --shot-after 2 --screenshot /tmp/x.ppm` | 跳到末帧自动出图 |
| `./build/superplace-viz --compare --frame 39 --shot-after 2 --screenshot /tmp/x.ppm` | 启动即开 T8 两帧对比（A=0,B=末帧） |
| `./build/superplace-viz --bench 60` | 性能回归（合成样本中位 6.66ms/p95 11.09ms，llvmpipe 软渲染） |
| `./build/superplace-viz /tmp/synsum ...` | 带 summary.json 的临时夹具验 T1（检查者勿依赖该临时目录） |

操作：左键拖拽平移、滚轮以光标为锚缩放、单击节点点选（点空白取消）、
"适应窗口"按 chip 重新 fit；右侧"控制"切图层/着色/时间轴，底部"收敛曲线"
看分阶段曲线、"帧对比"做 A/B。

## 2. 任务落地对照（06 §9 验收口径）

- **T1 总览**：`src/viz/summary.{h,cpp}` 健壮解析；缺键不崩、损坏降级 null、
  未知键原样展示、`cgp_macros_moved` 非 0 标红、`durations_ms` 分段；无
  summary 显示"该 bundle 不含总览"。`IPlacementSource::summary()` 带默认
  `nullptr`，LiveSource 零改动兼容。
- **T2 resize 保持视野**：`Camera::rescaleOnResize`（中心不动、短边比例），
  不重新 fit；`test_camera.cpp` 覆盖。
- **T3 比例尺**：`render/scale_tick.h` 的 1/2/5×10ⁿ niceStep；左下比例尺 +
  底部/左侧刻度，Y 向上。
- **T4 点选**：`spatial_index.{h,cpp}` chip 均匀网格 CSR，真实矩形命中、
  面积最小优先、零面积/F_NI 跳过、切帧懒重建；无 names.txt 降级显示。
- **T5 曲线**：`frame_map.{h,cpp}` metrics 行↔抽样帧按阶段+iter 最近邻，
  无 iter 走阶段内比例兜底，不伪造一一对应；分阶段分色、时间轴游标、点曲线跳帧。
- **T6 密度热图 + LOD**：`density.{h,cpp}` 节点与 bin 真实重叠面积累加
  （注释明确这是可视化面积占比，**非**优化器 τ/local smoothing）；远景按行高
  屏幕像素阈值自动切 LOD 热力图；静止视角不重复上传纹理（generation 缓存）。
- **T7 着色/图例**：按类别 / 按局部密度 / 按帧间位移连续色表 + 色带图例；
  **按 net 数因 T10 无 nets.bin 在组合框明确禁用并标注，不伪造、不扩格式**。
- **T8 两帧对比**：A/B 选择、位移着色、抽样位移向量（≤约 4000 支）、
  最大/平均位移。
- **T13 零依赖测试**：`gui/test/` 七组用例，373 检查全绿，ctest 通过。

## 3. 已目视验证（真实桌面截图，非设计图）

- 近景（frame39）：红标准单元铺开、橙宏阵列、蓝 pad 渲染正确。
- 远景 fit：LOD 蓝→绿→红密度热图正确，固定 pad 边框可见。
- T1：临时 summary 夹具下中文键名、`cGP 被移动宏数=1` 标红、未知键原样、
  阶段耗时分段均显示。
- T8：`--compare` 下位移向量、对比面板 A/B 滑杆、最大/平均位移显示。
- T5：mGP 红 / cGP 蓝分段 HPWL、overflow 右轴、图例。

## 4. 已知缺口（非本轮范围，供检查者判断）

- T9 names.txt 名称生成（生成端）、T10 网表连接性与 net 高亮/着色（无 nets.bin 时
  显式禁用、不伪造）、T11 实时四阶段、
  T12 暂停/停止：均属本轮授权边界之外，未实施（非仅缺样本）。
- 合成样本无 summary.json，T1 仅逻辑单测 + 临时夹具目视。
- LOD 阈值按标准单元行高屏幕像素（15px）；密度场固定 128×128。
- 大样本 357217 路径待检查者提供后补 bench150 同命令对比。

## 5. 轮次记录

### Round 1（初版，待审）
- 构建/测试全绿；上述任务落地；清理了全部临时调试脚手架（无 DEBUG/品红/
  强制 LOD/逐帧错误打印残留）。

### Round 2（修复 R1-1 … R1-6）
复现方式（无头探针，不开窗口；探针只在 /tmp，不进仓库）：
```
g++ -std=c++17 -I gui/src -I third_party probe.cpp \
    gui/src/viz/bundle_source.cpp gui/src/viz/summary.cpp -o /tmp/probe
/tmp/probe <fixture-dir>      # 优雅拒绝 rc=1，不再 Aborted(134)/bad_alloc
```
- **R1-1 T5 映射诚实度**：`frame_map.{h,cpp}` 引入 Exact/Nearest/Approx/
  Unavailable 四级质量；`parseIter` 只认 `iter N` 词元（文件名/newblue1 数字不取），
  长数字 strtol 饱和不溢出；未知阶段/无本阶段帧 → Unavailable 返回 -1，
  **不跨阶段全局比例猜测**；同名阶段非连续出现（中间夹别的阶段）按各自段独立切分，
  iter 搜索覆盖同名各段、比例兜底只在本阶段行并集内取，不串段；游标按质量着色
  （精确黑/最近邻蓝/近似橙）并文字标注，Unavailable 不画游标、点击不跳帧。
- **R1-2 标量类型错**：`bundle_source.cpp` 全部标量走 safeInt/safeFloat/safeString/
  safeBool 类型守卫，`nodeCount="twelve"` 等类型不符优雅拒绝，不外抛 type_error(134)。
- **R1-3 恶意大 nodeCount**：先 `fs::file_size` 校验 geom.bin/flags.bin 与声明一致，
  再按 `kMaxNodes=64×1024×1024`（67,108,864 节点数上限，非内存 MiB）封顶，通过后
  才分配，杜绝 2e9 先分配后 bad_alloc。
- **R1-4 隐藏图层可点选**：`SpatialIndex::pick(wx,wy,visible)` 接收类别可见掩码，
  关闭图层不参与命中，隐藏小物体不再挡住可见大物体；app 接线 `canvas_.layerVisible`。
- **R1-5 ragged metrics**：缺列填 **NaN 断线（不补 0、不造数）**，维度恒为
  列数×rowCount，ImPlot 在 NaN 处断线，绘图/tooltip 不越界、不画成 0。
  （缺 stage 的处理在 R2-2 改为未知空串，不再继承。）
- **R1-6 长 iter 溢出**：strtol + 饱和，无连乘 UB。

自测：`gui/build/viz_tests` 397 项检查 0 失败，`ctest` 1/1；检查者 fixtures
f5c/f17/f8b/f9/f7/f2/f3/f4/f6/f8 等均 rc=1 优雅拒绝，f20/f14 维度一致、
缺失为 NaN 而非 0，f1_ok summary 正常。

### Round 3（修复 R2-1 / R2-2 + 性能核查 + 视觉修复）
- **R2-1 关层残留选中**：图层 Checkbox 关闭时若 selected_ 属于该类立即
  `selected_=-1`；高亮黄框与检视面板再加 `layerVisible` 防御掩码，隐藏层不画
  残留黄框、不展示过时检视。
- **R2-2 缺 stage 诚实化**：缺/空阶段名存空串（未知），**不继承上一阶段**；
  无 stage 列的旧源全部记未知；空阶段行/帧在 FrameMap 返回 Unavailable，不参与
  映射；曲线面板空段绘中性灰"（未知阶段）"并加提示，不冒充具名分段。
- **性能核查（保留风险，非宣称提速）**：静态确认 Kind 基准路径不调 setContinuousValues、
  不上传 bufValue_、不重建密度/索引；新增 `valueActive_` 按模式门控，类别模式跳过
  无效 n 浮点上传；Density 着色复用成员 scratch 替代每帧堆分配。终轮独立同命令
  ABAB（大样本 357217/6 帧，--bench 150，软渲染）：中位 A=172.92 / B=175.74 ms
  （+1.63%，接近）；p95 A=188.48/182.20、B=212.79/214.42 ms，两轮持续偏高约
  +12.90%/+17.68%。中位接近但 p95 尾延迟偏高约 13~18%，因果未定，保留性能风险，
  不宣称提速，也不宣称已消除退化。
- **视觉修复（比例尺越界）**：底部曲线/对比面板是钉底 250px 浮动窗，中央 dock
  节点却占满全高；从画布矩形扣减 250px 保留带，比例尺/横纵刻度只在实际画布
  矩形内绘制，不再压到收敛曲线坐标。
- **nit**：T8 位移向量端点加小箭头头。

自测：`gui/build/viz_tests` 419 项检查 0 失败，`ctest` 1/1。
实现状态与验收状态分开：代码/单测已完成；T2 局部 resize 像素级、正式点选/关层/
曲线跳转的真实桌面交互复验受当前环境限制，待独立检查者完成。

### Round 4（迭代号边界诚实化 + 清未使用常量）
- **R4 rowIter 三态**：iter 列值以 double 与 INT_MAX 比较（不用 float(INT_MAX)，其
  自身舍入为 2147483648）；仅有限、非负、整数、≤INT_MAX 才转换，否则判明确非法。
  1e30 / float(INT_MAX) / 负数 / 小数 / NaN / ±Inf 全部 Unavailable，不截断、不越界。
- **R4-1 非法行不进 Approx**：frameToRow/rowToFrame 的顺序兜底候选 flat 剔除明确非法行；
  全非法 → Unavailable；合法/非法混合只在合法或缺 metadata 行里选；缺 iter 列/缺行
  （缺 metadata）才允许 Approx。
- **清 kTok**：删除 frame_map.cpp 未使用静态常量，消除 unused-variable 警告。
- R4 未重跑 bench；R3 性能历史风险保留，R4 二进制不冒充新实测。
自测：`viz_tests` 460 项检查 0 失败，ctest 1/1；独立 review 138 项 0 失败
（默认与 STRICT=1）。
