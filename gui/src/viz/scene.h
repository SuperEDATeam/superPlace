// 查看器的内存数据模型。
//
// 这里只描述"一个布局长什么样"，不关心它从哪来——离线 bundle 和实时引擎
// 都把数据填成这几个结构。UI 与渲染层只认它们。
#pragma once

#include <cstdint>
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
/// 用指针而不是 vector：实时源每帧都在双缓冲里换，拷一份 330k×2 的 float
/// 虽然只要几十微秒，但没必要；离线源直接指向自己的缓存。
struct FrameView {
    const float* x = nullptr;
    const float* y = nullptr;
    int          count = 0;
    int          index = 0;
    std::string  stage;   ///< input / init / mGP / FILLERONLY / cGP
    std::string  label;
    bool valid() const { return x && y && count > 0; }
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

}  // namespace viz
