#include "init/cluster_common.h"

#include <algorithm>
#include <cmath>
#include <queue>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {

void CoarseGraph::buildClusterIndex() {
    cluStart.assign(static_cast<size_t>(numClusters) + 1, 0);
    for (int m : netMembers) cluStart[static_cast<size_t>(m) + 1]++;
    for (int c = 0; c < numClusters; ++c)
        cluStart[static_cast<size_t>(c) + 1] += cluStart[static_cast<size_t>(c)];
    cluNets.resize(netMembers.size());
    std::vector<int> cursor(cluStart.begin(), cluStart.end() - 1);
    for (int k = 0; k < numNets(); ++k)
        for (int t = netStart[k]; t < netStart[k + 1]; ++t)
            cluNets[static_cast<size_t>(cursor[static_cast<size_t>(netMembers[t])]++)] = k;
}

int bestFreeNeighbor(const CoarseGraph& g, int c, MatchScratch& s, float* outW) {
    s.touched.clear();
    for (int t = g.cluStart[static_cast<size_t>(c)]; t < g.cluStart[static_cast<size_t>(c) + 1]; ++t) {
        const int k = g.cluNets[static_cast<size_t>(t)];
        const int m = g.netSize(k);
        if (m < 2) continue;
        const float w = 1.0f / static_cast<float>(m - 1);
        for (int q = g.netStart[k]; q < g.netStart[k + 1]; ++q) {
            const int o = g.netMembers[q];
            if (o == c || s.matched[static_cast<size_t>(o)]) continue;
            if (s.weight[static_cast<size_t>(o)] == 0.f) s.touched.push_back(o);
            s.weight[static_cast<size_t>(o)] += w;
        }
    }
    int best = -1;
    float bestW = 0.f;
    for (int o : s.touched) {
        const float w = s.weight[static_cast<size_t>(o)];
        if (w > bestW || (w == bestW && best >= 0 && o < best)) {
            bestW = w;
            best = o;
        }
    }
    for (int o : s.touched) s.weight[static_cast<size_t>(o)] = 0.f;
    if (outW) *outW = bestW;
    return best;
}

namespace {

/// 由 PlaceDB 构造第 0 层：簇即单元，net 去掉固定节点后去重。
CoarseGraph buildLevel0(const PlaceDB& db, int maxDegree) {
    CoarseGraph g;
    g.numClusters = db.numMovable;
    g.netStart.push_back(0);

    std::vector<int> stamp(static_cast<size_t>(db.numMovable), -1);
    std::vector<int> members;
    for (int k = 0; k < db.numNets; ++k) {
        const int b = db.net2pin_start[k], e = db.net2pin_start[k + 1];
        const int deg = e - b;
        // 超大 net 跳过：clique 展开是 O(deg²)，adaptec1 最大 deg 2271
        if (deg < 2 || deg > maxDegree) continue;

        members.clear();
        for (int t = b; t < e; ++t) {
            const int node = db.pin2node[db.flat_net2pin[t]];
            if (node >= db.numMovable) continue;                  // 固定节点不参与聚类
            if (stamp[static_cast<size_t>(node)] == k) continue;  // 同簇去重
            stamp[static_cast<size_t>(node)] = k;
            members.push_back(node);
        }
        if (members.size() < 2) continue;
        for (int m : members) g.netMembers.push_back(m);
        g.netStart.push_back(static_cast<int>(g.netMembers.size()));
    }
    g.buildClusterIndex();
    return g;
}

/// 用 newId 映射把当前层压成下一层
CoarseGraph rebuild(const CoarseGraph& g, const std::vector<int>& newId, int numNew) {
    CoarseGraph ng;
    ng.numClusters = numNew;
    ng.netStart.push_back(0);

    std::vector<int> stamp(static_cast<size_t>(numNew), -1);
    std::vector<int> members;
    const int nn = g.numNets();
    for (int k = 0; k < nn; ++k) {
        members.clear();
        for (int t = g.netStart[k]; t < g.netStart[k + 1]; ++t) {
            const int c = newId[static_cast<size_t>(g.netMembers[t])];
            if (stamp[static_cast<size_t>(c)] == k) continue;
            stamp[static_cast<size_t>(c)] = k;
            members.push_back(c);
        }
        // 成员被合并到一起后 net 退化成单点，不再提供连接信息
        if (members.size() < 2) continue;
        for (int m : members) ng.netMembers.push_back(m);
        ng.netStart.push_back(static_cast<int>(ng.netMembers.size()));
    }
    ng.buildClusterIndex();
    return ng;
}

struct ClusterBox {
    int   cluster;
    float lx, ly, hx, hy;
};

}  // namespace

// ----------------------------------------------------------------------------
void ClusterPlacer::place(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    Timer timer;
    const int n = db.numMovable;
    if (n == 0) return;

    // 默认目标：每簇约 kCellsPerCluster 个单元。
    //
    // 该值经实测选定（adaptec1 上扫描 459/2000/8000/20000/50000，8000 附近最优，
    // 对应每簇约 26 个单元）。存在甜点是因为两股力的拉锯：
    //   簇太少 -> 单个簇的框很大，簇内只能按任意顺序铺格子，位置信息被稀释
    //   簇太多 -> 聚类没来得及捕捉到连接关系，结果趋近随机
    // 注意这个最优值与簇层布局方式（BFS 序 + shelf packing）相关，不是聚类本身的普适性质。
    constexpr int kCellsPerCluster = 25;
    int target = cfg.cluster_target_count;
    if (target <= 0) target = std::max(1, n / kCellsPerCluster);
    target = std::min(target, n);

    // ------------------------------------------------------------ 逐层粗化
    CoarseGraph g = buildLevel0(db, cfg.ignore_net_degree);
    std::vector<int> nodeCluster(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) nodeCluster[static_cast<size_t>(i)] = i;

    MatchScratch scratch;
    std::vector<int> partner, newId;

    int level = 0;
    while (g.numClusters > target) {
        scratch.reset(g.numClusters);
        const int merges = matcher_->match(g, g.numClusters - target, partner, scratch);
        if (merges == 0) break;   // 图已不连通，无法继续合并

        // 粗化到后期，网表逐渐失去连接（net 成员塌缩成单簇即被丢弃），
        // 剩下的簇之间可能根本没有边。此时再磨下去只是空转，及早收手。
        const bool stalled = (merges * 50 < g.numClusters);

        // 重编号：配对的两簇共用新号，未配对的各自保留
        newId.assign(static_cast<size_t>(g.numClusters), -1);
        int numNew = 0;
        for (int c = 0; c < g.numClusters; ++c) {
            if (newId[static_cast<size_t>(c)] >= 0) continue;
            const int id = numNew++;
            newId[static_cast<size_t>(c)] = id;
            const int p = partner[static_cast<size_t>(c)];
            if (p >= 0) newId[static_cast<size_t>(p)] = id;
        }

        for (int i = 0; i < n; ++i)
            nodeCluster[static_cast<size_t>(i)] =
                newId[static_cast<size_t>(nodeCluster[static_cast<size_t>(i)])];
        g = rebuild(g, newId, numNew);
        ++level;
        if (stalled || level > 128) break;
    }

    const int numClusters = g.numClusters;

    // ------------------------------------------------ 收集簇成员、面积、计数
    std::vector<int> memberStart(static_cast<size_t>(numClusters) + 1, 0);
    for (int i = 0; i < n; ++i)
        memberStart[static_cast<size_t>(nodeCluster[static_cast<size_t>(i)]) + 1]++;
    for (int c = 0; c < numClusters; ++c)
        memberStart[static_cast<size_t>(c) + 1] += memberStart[static_cast<size_t>(c)];
    std::vector<int> members(static_cast<size_t>(n));
    {
        std::vector<int> cursor(memberStart.begin(), memberStart.end() - 1);
        for (int i = 0; i < n; ++i)
            members[static_cast<size_t>(cursor[static_cast<size_t>(
                nodeCluster[static_cast<size_t>(i)])]++)] = i;
    }
    std::vector<double> cArea(static_cast<size_t>(numClusters), 0.0);
    for (int i = 0; i < n; ++i) cArea[static_cast<size_t>(nodeCluster[static_cast<size_t>(i)])] += db.area(i);

    // ------------------------------------------------------------- BFS 排序
    // 聚类只回答了"谁和谁该挨着"，还需把簇映射到二维空间。
    // 在簇连接图上做 BFS 得到一维序列——BFS 保证强连接的簇在序列中相邻——
    // 再沿该序列做面积加权的 shelf packing，把序列相邻转化为空间相邻。
    std::vector<int>  order;
    std::vector<char> visited(static_cast<size_t>(numClusters), 0);
    order.reserve(static_cast<size_t>(numClusters));
    std::vector<int> nbr;
    for (int seed = 0; seed < numClusters; ++seed) {
        if (visited[static_cast<size_t>(seed)]) continue;
        std::queue<int> q;
        q.push(seed);
        visited[static_cast<size_t>(seed)] = 1;
        while (!q.empty()) {
            const int c = q.front();
            q.pop();
            order.push_back(c);
            nbr.clear();
            for (int t = g.cluStart[static_cast<size_t>(c)];
                 t < g.cluStart[static_cast<size_t>(c) + 1]; ++t) {
                const int k = g.cluNets[static_cast<size_t>(t)];
                for (int s = g.netStart[k]; s < g.netStart[k + 1]; ++s) {
                    const int o = g.netMembers[s];
                    if (o != c && !visited[static_cast<size_t>(o)]) nbr.push_back(o);
                }
            }
            std::sort(nbr.begin(), nbr.end());   // 固定顺序，保证可复现
            nbr.erase(std::unique(nbr.begin(), nbr.end()), nbr.end());
            for (int o : nbr) {
                if (visited[static_cast<size_t>(o)]) continue;
                visited[static_cast<size_t>(o)] = 1;
                q.push(o);
            }
        }
    }

    // ------------------------------------------ 面积加权 shelf packing
    double totalArea = 0.0;
    for (int c : order) totalArea += cArea[static_cast<size_t>(c)];
    if (totalArea <= 0.0) totalArea = 1.0;

    const Rect& core = db.coreRegion;
    const int numShelves =
        std::max(1, static_cast<int>(std::round(std::sqrt(static_cast<double>(order.size())))));
    const double areaPerShelf = totalArea / numShelves;

    std::vector<ClusterBox> boxes;
    boxes.reserve(order.size());

    size_t idx = 0;
    float shelfY = core.ly;
    for (int s = 0; s < numShelves && idx < order.size(); ++s) {
        const bool lastShelf = (s == numShelves - 1);
        const size_t begin = idx;
        double shelfArea = 0.0;
        while (idx < order.size() && (lastShelf || shelfArea < areaPerShelf)) {
            shelfArea += cArea[static_cast<size_t>(order[idx])];
            ++idx;
        }
        if (shelfArea <= 0.0) shelfArea = 1.0;

        const float shelfH = lastShelf ? (core.hy - shelfY)
                                       : static_cast<float>(shelfArea / totalArea) * core.height();

        // 蛇形（boustrophedon）排布：奇数行反向铺，
        // 使换行处前后两个簇在空间上仍然相邻，避免"行尾跳回行首"的长距离断裂
        std::vector<size_t> seq;
        seq.reserve(idx - begin);
        for (size_t t = begin; t < idx; ++t) seq.push_back(t);
        if (s % 2 == 1) std::reverse(seq.begin(), seq.end());

        float x = core.lx;
        for (size_t si = 0; si < seq.size(); ++si) {
            const int c = order[seq[si]];
            const bool lastInShelf = (si + 1 == seq.size());
            const float wBox = lastInShelf
                                   ? (core.hx - x)
                                   : static_cast<float>(cArea[static_cast<size_t>(c)] / shelfArea) *
                                         core.width();
            boxes.push_back(ClusterBox{c, x, shelfY, x + wBox, shelfY + shelfH});
            x += wBox;
        }
        shelfY += shelfH;
    }

    // ---------------------------------------------------------- 簇内摆放
    for (const ClusterBox& box : boxes) {
        const int c = box.cluster;
        const int b = memberStart[static_cast<size_t>(c)], e = memberStart[static_cast<size_t>(c) + 1];
        const int cnt = e - b;
        if (cnt == 0) continue;
        const int side = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(cnt)))));
        const float cw = std::max(1e-3f, (box.hx - box.lx) / static_cast<float>(side));
        const float ch = std::max(1e-3f, (box.hy - box.ly) / static_cast<float>(side));

        for (int t = b; t < e; ++t) {
            const int node = members[static_cast<size_t>(t)];
            const int slot = t - b;
            const int gx = slot % side;
            const int gy = std::min(slot / side, side - 1);
            db.node_x[node] = box.lx + (static_cast<float>(gx) + 0.5f) * cw;
            db.node_y[node] = box.ly + (static_cast<float>(gy) + 0.5f) * ch;
            db.clampToCore(node);
        }
    }

    IterMetrics m;
    m.iter = 0;
    m.hpwl = computeHPWL(db);
    m.elapsed_ms = timer.elapsedMs();
    sink.push(m);
    SP_INFO("%s: %d clusters (target %d, %d levels), HPWL = %.6g  (%.1f ms)", name(), numClusters,
            target, level, m.hpwl, m.elapsed_ms);
}

}  // namespace sp
