// Weighted-Average (WA) 线长模型与梯度（05 §5.5.6）。
//
// HPWL 用了 max/min，不可导，没法直接交给梯度法。WA 用 softmax 平滑替代：
//   x_max ≈ Σ x_i·e^{x_i/γ} / Σ e^{x_i/γ}
//   x_min ≈ Σ x_i·e^{−x_i/γ} / Σ e^{−x_i/γ}
// γ 越小越逼近真实 HPWL，但梯度越尖锐；ePlace 让 γ 随密度溢出率 τ 下降而收紧。
#pragma once

#include <vector>

namespace sp {

class PlaceDB;

/// γ 调度（05 §5.5.6）：γ = 8·w_b·10^{(20/9)(τ−0.1)−1.0}
/// 另加分段：τ > 1.0 时 γ×10；τ < 0.1 时 γ×0.1。
/// binStep 为 bin 步长 w_b。
float computeGamma(float tau, float binStep);

/// WA 线长与梯度。
///
/// 输出 grad 长度为 2*numMovable：前 numMovable 个是 x 分量，后 numMovable 个是 y 分量。
/// 只有可移动节点有线长梯度；固定节点的引脚仍参与 b±/c± 的累加（它们锚定了 net 的位置）。
///
/// 语义：返回的是【数学梯度 ∂WL/∂x】，可直接作为总梯度的线长项。
class WaWirelength {
public:
    /// 预分配内部缓冲。numPins / numNets / numMovable 取自 db。
    void prepare(const PlaceDB& db);

    /// 计算 WA 线长与梯度。gamma 为平滑参数（不是 1/γ）。
    /// grad 必须已分配 2*numMovable 个 float；本函数会先清零。
    void compute(const PlaceDB& db, float gamma, int ignoreNetDegree, double* outWl, float* grad);

private:
    // 每个 pin 的指数项（两趟之间复用，避免重复调用 exp）
    std::vector<float> expPosX_, expNegX_, expPosY_, expNegY_;
    // 每个 net 的分母与（中心化的）分子
    std::vector<float> bPosX_, bNegX_, cPosX_, cNegX_;
    std::vector<float> bPosY_, bNegY_, cPosY_, cNegY_;
    // 每个 net 的引脚坐标极值。第二趟需要它来算 (x_i − x_max)，
    // 从指数项反解会损失精度，故直接缓存。
    std::vector<float> maxX_, minX_, maxY_, minY_;
};

}  // namespace sp
