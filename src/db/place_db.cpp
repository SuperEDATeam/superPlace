#include "db/place_db.h"

#include <algorithm>
#include <cassert>
#include <map>
#include <numeric>

namespace sp {

void PlaceDB::reserveNodes(int n) {
    node_x.reserve(n);
    node_y.reserve(n);
    node_w.reserve(n);
    node_h.reserve(n);
    node_flags.reserve(n);
    node_orient.reserve(n);
    node_name.reserve(n);
}

int PlaceDB::addNode(std::string name, float w, float h, uint8_t flags) {
    const int idx = static_cast<int>(node_x.size());
    node_x.push_back(0.f);
    node_y.push_back(0.f);
    node_w.push_back(w);
    node_h.push_back(h);
    node_flags.push_back(flags);
    node_orient.push_back(0);
    node_name.push_back(std::move(name));
    return idx;
}

std::vector<int> PlaceDB::partitionNodes() {
    const int n = static_cast<int>(node_x.size());
    // 稳定分段：先可移动后固定，段内保持原始文件顺序（保证可复现，铁律 7）
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        if (!isFixed(i)) order.push_back(i);
    numMovable = static_cast<int>(order.size());
    for (int i = 0; i < n; ++i)
        if (isFixed(i)) order.push_back(i);

    std::vector<int> oldToNew(n);
    for (int newIdx = 0; newIdx < n; ++newIdx) oldToNew[order[newIdx]] = newIdx;

    auto permute = [&](auto& vec) {
        using V = std::decay_t<decltype(vec)>;
        V tmp;
        tmp.resize(vec.size());
        for (int newIdx = 0; newIdx < n; ++newIdx) tmp[newIdx] = std::move(vec[order[newIdx]]);
        vec.swap(tmp);
    };
    permute(node_x);
    permute(node_y);
    permute(node_w);
    permute(node_h);
    permute(node_flags);
    permute(node_orient);
    permute(node_name);

    for (int& nodeIdx : pin2node) nodeIdx = oldToNew[nodeIdx];

    numNodes = n;
    return oldToNew;
}

void PlaceDB::finalizeCSR(const std::vector<int>& pinNet) {
    assert(static_cast<int>(pinNet.size()) == numPins);
    assert(static_cast<int>(pin2node.size()) == numPins);

    // ---- net -> pin：两趟法（计数 -> 前缀和 -> 填充），禁止中间 vector<vector>
    net2pin_start.assign(numNets + 1, 0);
    for (int p = 0; p < numPins; ++p) net2pin_start[pinNet[p] + 1]++;
    for (int k = 0; k < numNets; ++k) net2pin_start[k + 1] += net2pin_start[k];

    flat_net2pin.resize(numPins);
    {
        std::vector<int> cursor(net2pin_start.begin(), net2pin_start.end() - 1);
        for (int p = 0; p < numPins; ++p) flat_net2pin[cursor[pinNet[p]]++] = p;
    }

    // ---- node -> pin：同样两趟法
    const int n = numNodes;
    node2pin_start.assign(n + 1, 0);
    for (int p = 0; p < numPins; ++p) node2pin_start[pin2node[p] + 1]++;
    for (int i = 0; i < n; ++i) node2pin_start[i + 1] += node2pin_start[i];

    flat_node2pin.resize(numPins);
    {
        std::vector<int> cursor(node2pin_start.begin(), node2pin_start.end() - 1);
        for (int p = 0; p < numPins; ++p) flat_node2pin[cursor[pin2node[p]]++] = p;
    }
}

void PlaceDB::computeRegions() {
    // ---- 行高众数
    std::map<float, int> histogram;
    for (const SiteRow& r : rows) histogram[r.height]++;
    rowHeight = 0.f;
    int best = -1;
    for (const auto& [h, cnt] : histogram) {
        if (cnt > best) {
            best = cnt;
            rowHeight = h;
        }
    }

    // ---- coreRegion = 所有 SiteRow 的最小外包
    if (rows.empty()) {
        coreRegion = Rect{};
    } else {
        coreRegion = rows.front().rect();
        totalRowArea = 0.0;
        for (const SiteRow& r : rows) {
            coreRegion.expand(r.rect());
            totalRowArea += static_cast<double>(r.hx() - r.lx) * static_cast<double>(r.height);
        }
    }

    // ---- chipRegion = coreRegion ∪ 所有固定节点（跳过零面积的 terminal_NI）
    chipRegion = coreRegion;
    for (int i = 0; i < numNodes; ++i) {
        if (!isFixed(i) || isNI(i)) continue;
        chipRegion.expand(box(i));
    }
}

void PlaceDB::clampToCore(int i) {
    constexpr float kEps = 1e-4f;
    const float hw = 0.5f * node_w[i];
    const float hh = 0.5f * node_h[i];
    float x = node_x[i];
    float y = node_y[i];
    if (x - hw < coreRegion.lx) x = coreRegion.lx + hw + kEps;
    if (x + hw > coreRegion.hx) x = coreRegion.hx - hw - kEps;
    if (y - hh < coreRegion.ly) y = coreRegion.ly + hh + kEps;
    if (y + hh > coreRegion.hy) y = coreRegion.hy - hh - kEps;
    node_x[i] = x;
    node_y[i] = y;
}

}  // namespace sp
