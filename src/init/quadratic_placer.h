// 方法三：二次解析布局（Kraftwerk2 Bound2Bound 模型）。
//
// 三种初始布局里唯一有实用价值的，也是 M3 全局布局的初值来源。
//
// 核心：用 B2B 权重把不可导的 HPWL 精确拟合成二次型，求驻点即解稀疏线性方程组。
//   w_pq = 1/(P-1) · 1/|x_p - x_q|   （仅当 p 或 q 是该 net 的边界引脚）
// 由于只有涉及边界引脚的对参与，每个 net 恰好贡献 2P-3 对；
// 这些项求和后伸缩相消（telescoping），二次型严格等于 x_max - x_min。
#pragma once

#include "init/b2b_model.h"
#include "init/initial_placer.h"

namespace sp {

class QuadraticPlacer : public IInitialPlacer {
public:
    void place(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "quadratic"; }
};

}  // namespace sp
