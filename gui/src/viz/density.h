// T6 可视化密度场：由当前帧位置在查看器里现算的 bin 面积重叠。
//
// ⚠️ 这【不是】优化器用的密度：不做 local smoothing、不做密度缩放规则、
// 与引擎的 τ / 电荷密度没有对应关系（06 §T6 明确要求写清楚，免得日后有人
// 拿它和引擎 τ 对不上而困惑）。这里只是"每个 bin 里堆了多少单元面积"的
// 可视化统计，供远景 LOD 热力图与 T7 局部着色使用。
#pragma once

#include <vector>

#include "viz/scene.h"

namespace viz {

class DensityField {
public:
    /// 由当前帧位置累加每个节点与 bin 的【重叠面积】。
    /// @param chip 芯片外包，bin 划分范围
    /// @param x,y 当前帧中心坐标；geom 静态几何
    /// @param cols/rows bin 网格数
    void build(const Rect& chip, const float* x, const float* y,
               const NodeGeometry& geom, int cols = 128, int rows = 128);

    int   gridCols() const { return cols_; }
    int   gridRows() const { return rows_; }
    const Rect& chip() const { return chip_; }
    /// 归一化密度（bin 内单元面积 / bin 面积，0..1+），按行优先存储。
    const std::vector<float>& field() const { return norm_; }
    float maxNorm() const { return maxNorm_; }
    /// 每次 build() 自增，供调用方判断场是否刷新过（避免每帧重复上传纹理）。
    unsigned long generation() const { return generation_; }

    /// 采样某世界坐标处的归一化密度（越界返回 0）。
    float sample(float wx, float wy) const;

private:
    Rect chip_{};
    int cols_ = 0, rows_ = 0;
    std::vector<float> overlap_;   // 累计重叠面积
    std::vector<float> norm_;      // 归一化（面积占比）
    float binArea_ = 0.f;
    float maxNorm_ = 0.f;
    unsigned long generation_ = 0;
};

}  // namespace viz
