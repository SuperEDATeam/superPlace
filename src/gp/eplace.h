// ePlace 全局布局主体（05 §5.5）。
//
// 把 M2′ 的泊松求解、WA 线长梯度、Bin 密度场与 Nesterov 优化器装配成一个
// 完整的静电布局器：单元视作电荷，密度场即电势场，密度力把拥挤处的单元推开，
// 线长梯度把相连的单元拉近，λ 在两者之间做配平。
//
// 本文件是全项目符号最易错的地方，两条不变量写在实现里并有断言保护：
//   * 密度项在总梯度中前面是【减号】（∇D = −qξ）
//   * 电荷缩放只经由 BinGrid::chargeWeight，统计侧与梯度侧不得分叉
#pragma once

#include <functional>
#include <memory>
#include <vector>

namespace sp {

class PlaceDB;
class MetricsSink;
struct Config;

/// 四阶段流程中的全局布局阶段（05 §5.5.1）。
/// 三者的差异只有两处：哪些节点参与优化、以什么阈值停止。
enum class GpStage {
    kMGP,         ///< 混合尺寸全局布局：标准单元 + 可移动宏 + filler
    kFillerOnly,  ///< 宏合法化后重撒 filler，仅优化 filler，固定轮数
    kCGP,         ///< 标准单元全局布局：宏已固定，只动标准单元 + filler
};

const char* toString(GpStage s);

struct GpResult {
    int    iterations = 0;
    double hpwl       = 0.0;
    float  overflow   = 0.f;
    bool   converged  = false;   ///< 是否因 τ 达标而停（false 表示耗尽迭代上限）
};

/// 一次全局布局的执行体。构造时完成 bin 网格初始化等一次性工作，
/// 可对同一个 db 依次跑多个阶段。
class EPlace {
public:
    EPlace(PlaceDB& db, const Config& cfg, MetricsSink& sink);
    ~EPlace();

    EPlace(const EPlace&) = delete;
    EPlace& operator=(const EPlace&) = delete;

    /// 执行一个阶段，结束时 db 中的坐标即为该阶段的最终解。
    GpResult run(GpStage stage);

    /// 逐迭代回调，用于出图等旁路观测。
    ///
    /// 做成回调而不是让 gp/ 直接调用 plot/：绘图属于表现层，全局布局器不该知道
    /// PNG 的存在。调用方（stages/）自己决定画不画、多久画一张。
    using IterCallback = std::function<void(GpStage stage, int iter, const PlaceDB& db)>;
    void setIterCallback(IterCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sp
