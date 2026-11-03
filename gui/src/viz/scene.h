// 查看器的内存数据模型。
//
// 这里只描述"一个布局长什么样"，不关心它从哪来——离线 bundle 和实时引擎
// 都把数据填成这几个结构。UI 与渲染层只认它们。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace viz {

/// 节点标志位。数值必须与算法侧 src/db/place_db.h 的 NodeFlag 一致。
enum NodeFlag : uint8_t {
    F_MACRO  = 1u << 0,
    F_FIXED  = 1u << 1,
    F_FILLER = 1u << 2,
    F_NI     = 1u << 3,   ///< 零面积 IO 引脚，渲染与统计都要跳过
};

/// 渲染与图层开关用的类别。顺序即图例顺序。
enum class Kind : uint8_t { Filler = 0, Fixed = 1, Cell = 2, Macro = 3, Count = 4 };

inline Kind kindOf(uint8_t flags) {
    if (flags & F_FILLER) return Kind::Filler;
    if (flags & F_FIXED)  return Kind::Fixed;
    if (flags & F_MACRO)  return Kind::Macro;
    return Kind::Cell;
}

struct Rect {
    float lx = 0.f, ly = 0.f, hx = 0.f, hy = 0.f;
    float width()  const { return hx - lx; }
    float height() const { return hy - ly; }
    float cx() const { return 0.5f * (lx + hx); }
    float cy() const { return 0.5f * (ly + hy); }
};

/// 整场不变的元信息。
struct SceneInfo {
    std::string benchmark;
    int   nodeCount = 0;
    Rect  core;        ///< 可布局区域（所有 SiteRow 的外包）
    Rect  chip;        ///< 整芯片外包 = core ∪ 全部固定节点。**视野按它 fit**
    float rowHeight = 0.f;
    int   rows = 0;
    int   countByKind[static_cast<int>(Kind::Count)] = {0, 0, 0, 0};
};

/// 整场不变的几何。纯 SoA——和算法侧同样的理由：上传 GPU 时是一次 memcpy。
struct NodeGeometry {
    std::vector<float>   w, h;
    std::vector<uint8_t> flags;
    std::vector<uint8_t> kind;      ///< 由 flags 预计算，渲染每帧都要用
    std::vector<std::string> name;  ///< 可选，仅检视面板用；为空表示 bundle 未带
};

/// 一帧的位置。**中心坐标**，不是左下角（与算法侧"铁律 2"一致）。
///
/// T15：数据所有权进契约。`data` 是整段 2N 浮点缓冲的 shared_ptr 引用计数
/// 所有权，`x`/`y` 只是从它派生的裸指针。BundleSource 的 LRU 缓存淘汰时只
/// 放掉缓存自己那份引用——任何还持有 `FrameView` 的地方（当前帧、T8 的两帧、
/// 懒计算）数据都活着，悬垂问题整类消失，且零拷贝。
struct FrameView {
    /// 2N：前半 x[0..N-1]、后半 y[0..N-1]。空表示该帧无数据。
    std::shared_ptr<const std::vector<float>> data;
    const float* x = nullptr;
    const float* y = nullptr;
    int          count = 0;
    int          index = 0;
    std::string  stage;   ///< input / init / mGP / FILLERONLY / cGP
    std::string  label;
    bool valid() const { return data && x && y && count > 0; }
};

/// 一帧的元数据（**不含位置**）。`IPlacementSource::frameInfo()` 只返回它，
/// 绝不触发位置文件 I/O——T5 建映射、进度条刻度、T14 时间轴都只读它。
/// 字段语义见 work_report/06 §3.2。
struct FrameMeta {
    int         index = -1;
    std::string stage;
    std::string label;
    int         iter = -2;          ///< >=0 阶段内迭代号；-1 明确里程碑（停留）；-2 缺字段（普通帧）
    double      t = 0.0;            ///< 全局进度（累计轮数），单调不减
    bool        breakBefore = false; ///< 与上一帧硬切，不得插值
};

/// metrics.csv。列名动态，因为算法侧可能加列。
struct MetricsTable {
    std::vector<std::string>        columns;
    std::vector<std::string>        stage;    ///< 第 0 列（字符串）单独存
    std::vector<std::vector<float>> values;   ///< values[列][行]，画曲线时要的是列
    int rowCount = 0;

    int columnIndex(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i] == name) return static_cast<int>(i);
        return -1;
    }
};

/// summary.json 的一个标量值。算法侧写出来的有的是数字、有的是字符串，
/// 前端不假设类型，两种都留着，显示时按 isString 选择。
struct SummaryValue {
    bool isString = false;
    std::string text;     ///< isString 时的原始字符串（原样展示，不做数值换算）
    double number = 0.0;
};

/// summary.json 的健壮表示。
///
/// ⚠️ 键会随算法演进增减（06 §T1）：这里【不写死键名清单】，按"有则显示"处理，
/// 未知键原样列出，缺键不报错，文件损坏/缺失时整个表为空而不是崩溃。
struct SummaryTable {
    /// 顶层标量键值对，保持 JSON 里的出现顺序。
    std::vector<std::pair<std::string, SummaryValue>> entries;
    /// `durations_ms` 子对象（各阶段耗时），单独摊平成键值对。
    std::vector<std::pair<std::string, double>> durationsMs;

    bool empty() const { return entries.empty() && durationsMs.empty(); }

    const SummaryValue* find(const std::string& key) const {
        for (const auto& e : entries)
            if (e.first == key) return &e.second;
        return nullptr;
    }
};

}  // namespace viz
