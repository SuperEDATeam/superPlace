// 方法二：聚类驱动布局（课程任务 4-2）。
//
// 分而治之：先把 21 万单元聚成几百个簇，在簇的层面决定空间位置，再在簇内摆放成员。
// 连接紧密的单元被聚到同一簇 => 天然靠得近，因此 HPWL 显著优于随机布局。
//
// 两种合并策略（这是同一方法的两个变体，不是两种方法）：
//   FC  First-Choice —— 一轮扫描，每个簇各自并向自己的最佳邻居；多轮直到达标
//   BC  Best-Choice  —— 全局优先队列，每次只合并当前权重最大的那一对（课程资料的描述）
#pragma once

#include "init/initial_placer.h"

namespace sp {

enum class MergeStrategy { kFirstChoice, kBestChoice };

class ClusterPlacer : public IInitialPlacer {
public:
    explicit ClusterPlacer(MergeStrategy s) : strategy_(s) {}

    void place(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override {
        return strategy_ == MergeStrategy::kFirstChoice ? "cluster_fc" : "cluster_bc";
    }

private:
    MergeStrategy strategy_;
};

}  // namespace sp
