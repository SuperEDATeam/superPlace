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

    // 第 f 帧引脚 p 的偏移：基础 + 所有 frame<=f 更新累积。
    // 返回交错 dx,dy（长度 2*pinCount）。更新文件首次访问即读入并缓存。
    // 失败返回 false。
    bool pinOffsetForFrame(int f, std::vector<float>& out, const std::string& bundleDir) const;
};

}  // namespace viz
