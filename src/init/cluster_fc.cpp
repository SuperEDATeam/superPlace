// First-Choice 合并策略。
//
// 一轮扫描所有簇，每个簇各自并向当前仍空闲的最佳邻居；已配对者跳过。
// 每层最多把簇数减半，因此从 21 万降到几百约需 9~13 层。
//
// 来源：多层超图划分文献中的 FC 粗化（hMETIS，Karypis & Kumar）。
//
// 相对 Best-Choice 的取舍：
//   + 无需优先队列，实现简单，常数小（实测约为 BC 的 1/2.5 耗时）
//   - 配对顺序由簇号决定而非权重，质量略逊（实测 HPWL 高约 10%）
#include <vector>

#include "init/cluster_common.h"

namespace sp {
namespace {

class FirstChoiceMatcher final : public IClusterMatcher {
public:
    int match(const CoarseGraph& g, int maxMerges, std::vector<int>& partner,
              MatchScratch& scratch) override {
        const int m = g.numClusters;
        partner.assign(static_cast<size_t>(m), -1);
        if (maxMerges <= 0) return 0;

        int merges = 0;
        for (int c = 0; c < m && merges < maxMerges; ++c) {
            if (scratch.matched[static_cast<size_t>(c)]) continue;

            // bestFreeNeighbor 只在"尚未配对"的簇中挑选，因此最佳邻居被占用时
            // 会自动退而求其次。初版曾在这里直接放弃，导致大量簇每轮空转、
            // 64 轮后仍远未达到目标簇数。
            const int b = bestFreeNeighbor(g, c, scratch, nullptr);
            if (b < 0) continue;

            partner[static_cast<size_t>(c)] = b;
            scratch.matched[static_cast<size_t>(c)] = 1;
            scratch.matched[static_cast<size_t>(b)] = 1;
            ++merges;
        }
        return merges;
    }

    const char* name() const override { return "first-choice"; }
};

}  // namespace

std::unique_ptr<IClusterMatcher> makeFirstChoiceMatcher() {
    return std::make_unique<FirstChoiceMatcher>();
}

}  // namespace sp
