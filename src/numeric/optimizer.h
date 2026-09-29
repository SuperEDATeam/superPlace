// 一阶优化器的抽象契约（决策 8）。
//
// 铁律 6：优化器只知道三件事——待优化向量、求目标与梯度的回调、
// 把解投影回可行域的约束函数。它**不得** include gp/ 或 db/，
// 不知道什么是线长、密度、filler 或宏单元。
//
// 这样做的收益是优化器可插拔：换 Adam / 共轭梯度只是换一个工厂调用。
// 反面教材见 02 §3.3——easyPlace 的优化器接口按值传递巨型 vector，
// adaptec1 上每轮白白拷贝约 50 MB。本契约一律传裸指针。
#pragma once

#include <functional>
#include <memory>

namespace sp {

/// 目标函数与梯度。pos/grad 长度均为 n（调用方决定布局，通常是前半 x、后半 y）。
///
/// **语义锁死**：grad 是【数学梯度 ∇f】，优化器执行 pos -= α·grad。
/// 若上层持有的是"力"而非梯度（如密度力 qξ），必须由上层取负后再填入。
using ObjAndGradFn = std::function<void(const float* pos, float* grad, double* obj)>;

/// 把解投影回可行域（如夹回 coreRegion）。原地修改。
using ConstraintFn = std::function<void(float* pos)>;

struct OptConfig {
    int   max_iter = 1000;
    bool  use_bb   = true;      ///< Barzilai-Borwein 步长
    float init_step = 0.f;      ///< <=0 表示用扰动法自动估计首步步长
    float init_perturb = 0.1f;  ///< 扰动法的相对扰动幅度
};

class Optimizer {
public:
    virtual ~Optimizer() = default;

    Optimizer(const Optimizer&) = delete;
    Optimizer& operator=(const Optimizer&) = delete;

    /// 执行一步迭代，就地更新 pos。
    virtual void step(float* pos, int n) = 0;

    virtual int    iteration() const = 0;
    virtual double objective() const = 0;   ///< 最近一次求得的目标值
    virtual float  stepSize() const = 0;     ///< 最近一次使用的 α
    virtual float  gradNorm() const = 0;     ///< 最近一次梯度的 L2 范数

protected:
    Optimizer() = default;
};

/// Nesterov 加速梯度（ePlace 变体，05 §5.5.10）。
std::unique_ptr<Optimizer> makeNesterov(ObjAndGradFn f, ConstraintFn c, const OptConfig& cfg);

}  // namespace sp
