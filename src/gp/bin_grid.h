// Bin 网格与静电密度场（05 §5.5.3 / §5.5.4 / §5.5.8）。
//
// 把布局区域离散成 m×m 网格，每个 bin 统计落入其中的单元面积（等价于电荷量），
// 再交给 PoissonBackend 求电势与电场。
#pragma once

#include <memory>
#include <vector>

#include "numeric/bin_grid_span.h"
#include "numeric/poisson_backend.h"

namespace sp {

class PlaceDB;

class BinGrid {
public:
    /// 初始化网格并预计算静态密度项。binDimOverride > 0 时跳过自动推算。
    /// chunks 为密度累加的局部网格块数（见 Config::density_chunks），<=0 取默认 8。
    void initialize(const PlaceDB& db, float targetDensity, int binDimOverride = 0,
                    int chunks = 0);

    /// 统计可移动节点与 filler 的密度贡献（每轮调用）。
    void accumulate(const PlaceDB& db);

    /// 解泊松方程，得到电势与电场。须在 accumulate 之后调用。
    void solveField();

    /// 密度溢出率 τ（05 §5.5.8）。须在 accumulate 之后调用。
    float overflow() const;

    /// 把电场采样到每个节点上，得到密度力 q·ξ。
    ///
    /// **语义锁死**：输出的是【力】不是梯度。由 ePlace 式 (16) 有 ∇D = −qξ，
    /// 因此上层组装总梯度时密度项前是减号（05 §5.5.7）。
    /// force 长度须为 2*(numNodes + numFillers)：前半 x、后半 y。
    void gatherForce(const PlaceDB& db, float* force) const;

    int   dim() const { return dim_; }
    float stepX() const { return stepX_; }
    float stepY() const { return stepY_; }
    float binArea() const { return stepX_ * stepY_; }

    /// 全部参与密度的节点的电荷之和。
    /// 不变量：它必须等于 Σ(nodeDensity + fillerDensity)——
    /// 二者若不相等，说明统计侧与梯度侧的缩放规则发生了分叉（easyPlace 的缺陷，02 §2.3）。
    double totalCharge(const PlaceDB& db) const;

    /// 供测试与调试读取
    const std::vector<float>& nodeDensity() const { return nodeDensity_; }
    const std::vector<float>& fillerDensity() const { return fillerDensity_; }
    const std::vector<float>& terminalDensity() const { return terminalDensity_; }
    const std::vector<float>& baseDensity() const { return baseDensity_; }

private:
    int   dim_ = 0;
    float stepX_ = 1.f, stepY_ = 1.f;
    float originX_ = 0.f, originY_ = 0.f;
    float targetDensity_ = 1.f;

    // 静态项：只在 initialize 中算一次
    std::vector<float> terminalDensity_;   // 固定终端的阻塞
    std::vector<float> baseDensity_;       // 不可放置区域的等效密度

    // 动态项：每轮 accumulate 重算
    std::vector<float> nodeDensity_;       // 标准单元 + 宏（宏已乘 targetDensity）
    std::vector<float> fillerDensity_;

    // 泊松求解的输入输出
    std::vector<float> rho_, phi_, fieldX_, fieldY_;
    std::unique_ptr<PoissonBackend> backend_;

    // 并行累加用的局部网格（铁律 7：禁原子加）。
    // 块数【固定】，不随线程数变——否则清零与规约的开销会随核数线性膨胀，
    // 且求和次序随线程数漂移。
    mutable std::vector<std::vector<float>> localNode_, localFiller_;

    // τ 的分母：cellArea + macroArea * targetDensity，初始化时算一次
    double movableAreaScaled_ = 0.0;

    /// 单元覆盖的 bin 区间 + local smoothing 后的矩形与缩放比
    struct Footprint {
        int   bx0, bx1, by0, by1;
        float lx, ly, hx, hy;
        float scale;
    };
    Footprint footprintOf(const PlaceDB& db, int i) const;

    /// 节点 i 的电荷缩放系数。**这是缩放规则的唯一真源**——
    /// accumulate 与 gatherForce 都必须经由它，否则两侧可能悄悄分叉。
    float chargeWeight(const PlaceDB& db, int i, float footprintScale) const;
};

}  // namespace sp
