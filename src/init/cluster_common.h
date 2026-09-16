// 聚类驱动布局的共享基础设施。
//
// 两种合并策略（First-Choice / Best-Choice）共用这里的全部内容——
// 多层粗化框架、粗粒度网表的构建与重建、簇层空间布局——
// 唯一的差异被隔离在 IClusterMatcher 的实现里，各自独立成文件：
//     cluster_fc.cpp   First-Choice
//     cluster_bc.cpp   Best-Choice
//
// 这样既保持了"策略可插拔"，又避免把 80% 的公共代码复制两份。
// 要加第三种策略（比如 Heavy-Edge Matching），只需新增一个文件并在工厂里注册。
#pragma once

#include <memory>
#include <vector>

#include "init/initial_placer.h"

namespace sp {

// ============================================================================
// 粗粒度网表。
//
// 关键设计：每合并一轮就重建一张更小的网表，下一层在新图上工作。
// 反面教材：若始终在原始网表上、通过遍历簇的全部成员来找最佳邻居，
// 簇越合越大、每次扫描越贵，21 万节点下总量级是 O(n²)，实测跑不出结果。
// 逐层粗化后每层开销正比于当前图规模，而规模按几何级数缩小，总开销回到 O(P)。
// ============================================================================
struct CoarseGraph {
    int numClusters = 0;

    // net -> 成员簇（CSR，已去重，只保留成员数 >= 2 的 net）
    std::vector<int> netStart, netMembers;
    // cluster -> 关联 net（CSR）
    std::vector<int> cluStart, cluNets;

    int numNets() const { return static_cast<int>(netStart.size()) - 1; }
    int netSize(int k) const { return netStart[k + 1] - netStart[k]; }

    /// 由 net -> cluster 反建 cluster -> net（两趟计数法）
    void buildClusterIndex();
};

/// 邻居扫描用的复用缓冲。放在外面是为了避免每次调用都重新分配。
struct MatchScratch {
    std::vector<float> weight;   // 稠密数组，长度 = numClusters
    std::vector<int>   touched;  // 本次被碰过的下标，便于按需清零
    std::vector<char>  matched;  // 本层内是否已配对

    void reset(int numClusters) {
        weight.assign(static_cast<size_t>(numClusters), 0.f);
        matched.assign(static_cast<size_t>(numClusters), 0);
        touched.clear();
        touched.reserve(1024);
    }
};

/// 扫描簇 c 的邻居，返回权重最大且**尚未配对**的那个；outW 可选接收权重。
/// 并列时取簇号最小者，保证可复现（铁律 7）。
///
/// 权重用 clique 模型归一化：成员数为 m 的 net 对其中每一对贡献 1/(m-1)，
/// 即 net 越"松散"单对权重越小。
int bestFreeNeighbor(const CoarseGraph& g, int c, MatchScratch& s, float* outW);

// ============================================================================
// 一层内的配对策略。两种聚类方法的唯一差异点。
// ============================================================================
class IClusterMatcher {
public:
    virtual ~IClusterMatcher() = default;

    /// 在 g 上做一次匹配（每个簇至多配对一次），最多合并 maxMerges 对。
    /// partner[c] = 配对对象，未配对为 -1。返回实际合并的对数。
    virtual int match(const CoarseGraph& g, int maxMerges, std::vector<int>& partner,
                      MatchScratch& scratch) = 0;

    virtual const char* name() const = 0;
};

std::unique_ptr<IClusterMatcher> makeFirstChoiceMatcher();
std::unique_ptr<IClusterMatcher> makeBestChoiceMatcher();

// ============================================================================
/// 聚类驱动布局：多层粗化 + BFS 排序 + 面积加权 shelf packing。
/// 合并策略由构造时注入的 matcher 决定。
class ClusterPlacer : public IInitialPlacer {
public:
    ClusterPlacer(std::unique_ptr<IClusterMatcher> matcher, const char* name)
        : matcher_(std::move(matcher)), name_(name) {}

    void place(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return name_; }

private:
    std::unique_ptr<IClusterMatcher> matcher_;
    const char* name_;
};

}  // namespace sp
