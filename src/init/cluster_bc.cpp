// Best-Choice 合并策略。
//
// 全局优先队列：每次取当前权重最大的可用簇对合并，直到本层达标。
// 这是课程资料描述的做法（"迭代地选择权重最大的簇对，将二者合并成一个"）。
//
// 来源：布局领域的 Best-Choice 聚类，通常追溯到 Alpert 等人的
// semi-persistent clustering（ISPD 2005）。
//
// 相对 First-Choice 的取舍：
//   + 每步都是全局最优的贪心，簇更"紧"，实测 HPWL 好约 10%
//   - 需要维护优先队列与失效条目，常数更大（实测约 2.5 倍耗时）
#include <queue>
#include <vector>

#include "init/cluster_common.h"

namespace sp {
namespace {

class BestChoiceMatcher final : public IClusterMatcher {
public:
    int match(const CoarseGraph& g, int maxMerges, std::vector<int>& partner,
              MatchScratch& scratch) override {
        const int m = g.numClusters;
        partner.assign(static_cast<size_t>(m), -1);
        if (maxMerges <= 0) return 0;

        std::priority_queue<Entry> pq;
        for (int c = 0; c < m; ++c) {
            float w = 0.f;
            const int b = bestFreeNeighbor(g, c, scratch, &w);
            if (b >= 0) pq.push(Entry{w, c, b});
        }

        // 每个簇最多允许重算 kMaxRetry 次，避免病态输入下反复入队
        constexpr int kMaxRetry = 3;
        std::vector<int> retry(static_cast<size_t>(m), 0);

        int merges = 0;
        while (!pq.empty() && merges < maxMerges) {
            const Entry e = pq.top();
            pq.pop();
            if (scratch.matched[static_cast<size_t>(e.from)]) continue;

            if (scratch.matched[static_cast<size_t>(e.to)]) {
                // 对端已被抢走：就地重算一次当前最佳可用邻居再入队（懒删除）
                if (retry[static_cast<size_t>(e.from)]++ >= kMaxRetry) continue;
                float w = 0.f;
                const int b = bestFreeNeighbor(g, e.from, scratch, &w);
                if (b >= 0) pq.push(Entry{w, e.from, b});
                continue;
            }

            partner[static_cast<size_t>(e.from)] = e.to;
            scratch.matched[static_cast<size_t>(e.from)] = 1;
            scratch.matched[static_cast<size_t>(e.to)] = 1;
            ++merges;
        }
        return merges;
    }

    const char* name() const override { return "best-choice"; }

private:
    struct Entry {
        float w;
        int   from, to;
        /// 大顶堆。tie-break 必须用 (权重, 簇号) 双键——
        /// 否则同权重时弹出顺序由堆的内部实现决定，结果不可复现（铁律 7）。
        bool operator<(const Entry& o) const {
            if (w != o.w) return w < o.w;
            if (from != o.from) return from > o.from;
            return to > o.to;
        }
    };
};

}  // namespace

std::unique_ptr<IClusterMatcher> makeBestChoiceMatcher() {
    return std::make_unique<BestChoiceMatcher>();
}

}  // namespace sp
