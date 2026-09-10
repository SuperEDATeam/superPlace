// PlaceDB：纯 SoA + CSR 的布局数据库。
// 规范依据：work_report/05-开发执行规范.md §3.2，铁律 2（坐标唯一约定）、铁律 3（纯 SoA）
//
// 坐标约定：node_x / node_y 存的是【中心坐标】。
// 左下角/右上角只能经由 llx()/lly()/urx()/ury() 派生获得。
// 与 BookShelf 左下角坐标的换算只允许发生在 io/bookshelf_reader|writer 中。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/geometry.h"

namespace sp {

enum NodeFlag : uint8_t {
    F_MACRO  = 1u << 0,  // 宏单元（不可移动的终端不置此位）
    F_FIXED  = 1u << 1,  // 固定：.nodes 中标 terminal，或 .pl 中带 /FIXED
    F_FILLER = 1u << 2,  // 填充单元（全局布局阶段生成，M1 不产生）
    F_NI     = 1u << 3,  // terminal_NI：零面积 IO 引脚，密度与绘图均需跳过
};

class PlaceDB {
public:
    // ---------------------------------------------------------- 节点几何（SoA）
    std::vector<float>       node_x, node_y;   // 中心坐标
    std::vector<float>       node_w, node_h;
    std::vector<uint8_t>     node_flags;
    std::vector<uint8_t>     node_orient;      // BookShelf 方向枚举，原样回写
    std::vector<std::string> node_name;        // 仅 IO/调试，禁止进热路径

    // ------------------------------------------------------------------- 引脚
    std::vector<float> pin_offset_x, pin_offset_y;  // 相对所属节点【中心】
    std::vector<int>   pin2node;

    // -------------------------------------------------------------- 超图 CSR
    // net k 的引脚：flat_net2pin[net2pin_start[k] .. net2pin_start[k+1])
    std::vector<int>   flat_net2pin, net2pin_start;
    // node i 的引脚：flat_node2pin[node2pin_start[i] .. node2pin_start[i+1])
    std::vector<int>   flat_node2pin, node2pin_start;
    std::vector<float> net_weight;             // .wts 缺省全 1.0
    std::vector<std::string> net_name;

    // --------------------------------------------------------------- 布局行
    struct SiteRow {
        float ly = 0.f, height = 0.f;
        float lx = 0.f, step = 1.f;
        int   numSites = 0;
        float hx() const { return lx + step * static_cast<float>(numSites); }
        float hy() const { return ly + height; }
        Rect  rect() const { return {lx, ly, hx(), hy()}; }
    };
    std::vector<SiteRow> rows;

    // --------------------------------------------------------------- 区域信息
    Rect  coreRegion{};      // 所有 SiteRow 的最小外包
    Rect  chipRegion{};      // coreRegion ∪ 所有固定节点，仅用于绘图
    float rowHeight    = 0.f;
    double totalRowArea = 0.0;

    // --------------------------------------------------------------- 分区索引
    // 节点按 [可移动 | 固定 | filler] 分段排列，热路径可直接切片
    int numMovable = 0;   // [0, numMovable)
    int numNodes   = 0;   // [numMovable, numNodes) 为固定节点
    int numFillers = 0;   // [numNodes, numNodes + numFillers)
    int numPins    = 0;
    int numNets    = 0;

    int totalNodes() const { return numNodes + numFillers; }

    // ------------------------------------------- 派生查询（唯一的中心↔角点换算处）
    float llx(int i) const { return node_x[i] - 0.5f * node_w[i]; }
    float lly(int i) const { return node_y[i] - 0.5f * node_h[i]; }
    float urx(int i) const { return node_x[i] + 0.5f * node_w[i]; }
    float ury(int i) const { return node_y[i] + 0.5f * node_h[i]; }
    Rect  box(int i) const { return {llx(i), lly(i), urx(i), ury(i)}; }
    double area(int i) const {
        return static_cast<double>(node_w[i]) * static_cast<double>(node_h[i]);
    }

    bool isMacro(int i)  const { return (node_flags[i] & F_MACRO)  != 0; }
    bool isFixed(int i)  const { return (node_flags[i] & F_FIXED)  != 0; }
    bool isFiller(int i) const { return (node_flags[i] & F_FILLER) != 0; }
    bool isNI(int i)     const { return (node_flags[i] & F_NI)     != 0; }

    /// 引脚绝对坐标 = 所属节点中心 + 偏移
    float pinX(int p) const { return node_x[pin2node[p]] + pin_offset_x[p]; }
    float pinY(int p) const { return node_y[pin2node[p]] + pin_offset_y[p]; }

    int netDegree(int k)  const { return net2pin_start[k + 1] - net2pin_start[k]; }
    int nodeDegree(int i) const { return node2pin_start[i + 1] - node2pin_start[i]; }

    // ------------------------------------------------------- 构建（仅 io 层调用）
    void reserveNodes(int n);
    int  addNode(std::string name, float w, float h, uint8_t flags);

    /// 依据 pin2node 与 pin 所属 net 建立双向 CSR。两趟法，禁止中间 vector<vector>。
    /// pinNet 长度必须等于 numPins，元素为该 pin 所属的 net 下标。
    void finalizeCSR(const std::vector<int>& pinNet);

    /// 计算 coreRegion / chipRegion / rowHeight / totalRowArea。须在 rows 与节点就绪后调用。
    void computeRegions();

    /// 把节点重排为 [可移动 | 固定]，同步更新 pin2node。返回 old->new 映射。
    std::vector<int> partitionNodes();

    /// 将节点中心夹回 coreRegion 内（保持整体在区域内）。
    void clampToCore(int i);
};

}  // namespace sp
