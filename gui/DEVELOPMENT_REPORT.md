# superplace-viz 开发交付报告

面向：组长。范围：`gui/` 下原生 C++17（Dear ImGui docking + GLFW + OpenGL3.3 + ImPlot）
布局可视化查看器。本轮（T11）在授权范围内改动 `gui/src/viz/live_source.cpp` 与
`gui/CMakeLists.txt`、新增 `gui/test/test_live_stages.cpp` 与迷你夹具；未触碰算法侧
`src/`、`test/`、根 `CMakeLists.txt`、`configs/`、`scripts/make_viz.py`、`work_report/`。

## 一、已完成（A 类）

| 任务 | 内容 |
| --- | --- |
| T1 总览 | `summary.json` 健壮解析：合法字段/未知键原样展示、异常宏数标红、分段耗时；损坏/类型错/空文件优雅跳过，无总览显示“不含总览”不崩 |
| T2 resize | 中心坐标不动、短边比例缩放、不重新 fit 的逻辑（静态核对通过） |
| T3 比例尺/坐标轴 | 1/2/5×10ⁿ niceStep 刻度、Y 向上；底部收敛曲线带上方保留区，比例尺/X 刻度不压曲线 |
| T4 空间索引+点选 | 懒更新真实矩形索引、与暴力扫描对拍、重叠小面积优先、零面积/F_NI 不入桶；隐藏图层不命中也不挡可见物体；无 names 时降级为矩形点选 |
| T5 收敛曲线+帧映射 | 多阶段分色；帧↔行映射分 Exact/Nearest/Approx/Unavailable 四级诚实标注；iter 只认词元、长数字饱和；未知/空阶段不跨跳不继承；缺失值 NaN 断线不补 0 |
| T6 密度热图/LOD | 按重叠面积现算密度（与独立脚本对拍），远景按屏幕像素阈值切热力图；LOD 属另一画面模式，非性能优化器 |
| T7 属性着色 | 类别/局部密度/位移即时切换与图例；按 net 数因无网表显式禁用、不伪造 |
| T8 两帧对比 | dx/dy 对拍、颜色=位移大小、矢量从 A 指向 B 带箭头头、双帧缓存；最大/平均位移真实 |
| T13 单测 | 零依赖无头测试，不引入 gtest |
| T11 实时四阶段 | `live_source.cpp` 内照搬算法侧权威序列：`mGP`→`mLG`(一次跳变单帧)→`scatterFillers`→`FILLERONLY`→`cGP`；每阶段新建 EPlace；UI/接缝零改动 |

横切：中心坐标/Y 向上/chip fit/分类优先级 Filler>Fixed>Macro>Cell/零面积规则保持；
新增 `IPlacementSource` 方法带默认实现，既有 LiveSource 不变成抽象类。

## 二、未做（本轮授权边界外，非仅缺样本）

- **T9 names.txt 名称生成**：依赖算法/导出端产出 names.txt，未实施；检视面板在无名称时显示占位、不编造。
- **T10 网表连接性与 net 高亮/着色**：无 nets.bin，相关着色项在界面显式禁用。
- **T12 暂停/单步/停止**：`EPlace::run()` 无中断机制，需算法侧先加 abort 路径，未做。

## 三、测试结果

- 产品无头单测 `viz_tests`（纯 viz_core，不链算法库）：**460 项检查 0 失败**；`ctest` 1/1。
- 集成无头测试 `viz_live_tests`（链 libsuperplace，T11 四阶段序列）：**821 项检查 0 失败**。
- 独立 review 断言（检查者另建，单独编译）：**138 项 0 失败**（含非法 iter 严格批）。
- 26 个畸形/边界夹具（坏 JSON、类型错、超大声明、ragged metrics、空阶段等）全部优雅
  拒绝或降级，无段错误/中止/坏分配；超大 nodeCount 分配前做文件大小一致性校验并按
  节点数上限封顶，整数解析饱和不溢出。

## 四、性能（历史记录，保留风险）

R3 在软渲染下同矩形绘制路径 `--bench 150` ABAB：大样本中位新旧接近（+1.63%），但
p95 尾延迟两轮持续偏高约 13~18%，合成样本中位同向偏高。软渲染波动下因果未定，
**保留尾延迟风险，不宣称性能完全通过**；后续轮次未重跑 bench，历史结果不冒充新数据。

## 五、终轮受限/留口（如实说明）

- T2 窗口拖拽像素级 resize、关层后选中清除的真实桌面手工复验受环境限制，仅静态/
  单测覆盖，未做完整手工验收。
- T11 已在真实原版 `newblue1`（64 个可移动宏）上端到端跑通四阶段并与同配 CLI 对拍：
  最终 HPWL live 75167609.794921875 vs CLI 75167609.79，相对误差约 7e-11（≤1e-6）；
  阶段帧 mGP/mLG/FILLERONLY/cGP 顺序正确，mLG 一次跳变单帧，64 宏 mLG 后移动数为 0。
- MMS（多宏）样本仍缺，仅影响 T8 两帧对比的额外样本覆盖；newblue1 已为混合尺寸验收数据。
- 可用性残留：首末帧对比时位移向量密集会遮挡局部布局，可关闭向量显示或改选相邻帧。

## 六、构建、运行与测试（仓库根执行）

```bash
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release
cmake --build gui/build -j4

# 运行（默认加载内置合成样本）
cd gui && ./build/superplace-viz
# 参数：[bundle] [--frame N] [--compare] [--bench N] [--shot-after s --screenshot f.ppm]

# 产品无头测试
gui/build/viz_tests
ctest --test-dir gui/build --output-on-failure
```

独立 review 套件为检查者私有断言，不并入产品测试，由其独立编译运行。

## 七、依赖与边界

仅依赖系统 GLFW/OpenGL 与仓库 vendor ImGui/ImPlot（随仓库保留），不新增第三方依赖；
根 CMake 不引用 gui，对算法侧构建零影响。改动全部位于 `gui/`。
