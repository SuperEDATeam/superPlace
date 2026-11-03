// T4 均匀网格空间索引：点选的前提。
//
// 为什么不能线性扫：33 万节点每次点击全扫不可接受（06 §T4）。按 chip 划成
// ~256×256 桶，CSR 两趟构建（与算法侧 PlaceDB::finalizeCSR 同一套路），
// 点选只查鼠标所在桶及其 8 邻域。
//
// 位置每帧都变，索引必须跟着重建；但【不要每帧建】——只在切帧后第一次点选时
// 懒重建（App 负责置 dirty）。
//
// 命中判定用【真实矩形】（中心 ± w/2, h/2），不是渲染时被亚像素保护放大后的
// 矩形，否则全芯片视角下点哪儿都"命中"一片。重叠时选真实面积最小者，
// 否则压在宏上的标准单元永远选不中。
#pragma once

#include <vector>

#include "viz/scene.h"

namespace viz {

class SpatialIndex {
public:
    /// 建索引。零面积 / F_NI 节点不入桶（它们本来也不渲染）。
    /// @param chip      芯片外包，桶划分范围
    /// @param x, y      当前帧中心坐标（长度 n）
    /// @param geom      静态几何（w/h/flags）
    /// @param gridCols/gridRows 桶数，默认 256×256
    void build(const Rect& chip, const float* x, const float* y,
               const NodeGeometry& geom, int gridCols = 256, int gridRows = 256);

    /// 点选：返回命中的原始节点下标；未命中返回 -1。
    /// 重叠时选真实矩形面积最小者。
    /// @param visible 长度为 Kind::Count 的可见掩码；nullptr 表示全部可见。
    ///                关闭的图层不参与命中，也不会挡住可见的大物体。
    int pick(float wx, float wy, const bool* visible = nullptr) const;

    bool valid() const { return cols_ > 0 && rows_ > 0; }

    /// 诊断/测试：桶内记录数（越界桶返回 0）。
    int bucketCount(int col, int row) const;

private:
    Rect chip_{};
    int cols_ = 0, rows_ = 0;
    float cellW_ = 0.f, cellH_ = 0.f;

    // CSR：start_[b]..start_[b+1] 是桶 b 在 items_ 里的区间
    std::vector<int> start_;
    std::vector<int> items_;

    // build 时拷一份命中所需几何，不依赖调用方帧缓存的生命周期
    std::vector<int>   orig_;    // 原始节点下标
    std::vector<uint8_t> kd_;    // 预计算类别（可见掩码过滤用）
    std::vector<float> cx_, cy_, rw_, rh_;
};

}  // namespace viz
