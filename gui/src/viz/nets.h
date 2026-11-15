// T10 网表（§3.9）——纯增量可选数据，首次用到才加载。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace viz {

struct NetList {
    int netCount = 0;
    int pinCount = 0;
    bool hasNames = false;
    bool hasWeights = false;

    // net_start[netCount+1]：net k 的引脚在 net_pins[start[k]..start[k+1])
    std::vector<int> netStart;
    // 按 net 分组的引脚下标
    std::vector<int> netPins;
    // 引脚 p 所属节点（[0,nodeCount)）
    std::vector<int> pinNode;
    // pinOffset 交错 dx,dy（第 0 帧相对节点中心），长度 2*pinCount
    std::vector<float> pinOffset;
    std::vector<std::string> netNames;   // hasNames
    std::vector<float> netWeight;        // hasWeights

    // 偏移稀疏更新：{frame, file, count}
    struct OffsetUpdate {
        int frame = -1;
        int count = 0;
        std::string file;
    };
    std::vector<OffsetUpdate> offsetUpdates;

    // ---- 派生（加载时两趟计数 O(P) 建 CSR）----
    // node→pins：nodePinStart[nodeCount+1]，pins 在 nodePins[]
    std::vector<int> nodePinStart;
    std::vector<int> nodePins;
    // pin→net：pinNet[pin] = net 下标
    std::vector<int> pinNet;

    // 返回只读引用（同一 f 复用缓存，不每帧拷贝）；失败返回 nullptr。
    const std::vector<float>* pinOffsetForFrame(int f, const std::string& bundleDir) const;

    // T26：一个单元所连的去重 net 列表（按 net 下标升序）。
    struct NodeNetRef {
        int net = -1;
        int pinCount = 0;   // 该单元在这个 net 上的引脚数
    };
    std::vector<NodeNetRef> netsOfNode(int node) const;

    // 内部缓存
    mutable int cachedFrame_ = -1;
    mutable std::vector<float> cachedOffset_;
    mutable std::vector<std::vector<char>> cachedBlobs_;
    mutable bool blobsLoaded_ = false;
    mutable bool loadFailed_ = false;
};

}  // namespace viz
