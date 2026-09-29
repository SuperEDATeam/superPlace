// Filler（填充单元）初始化（05 §5.5.2）。
//
// Filler 不是可有可无的装饰，它有三个不可替代的作用：
//   1. 让密度场成为连续介质——没有它，密度集中在少数 bin，Poisson 解出的电势充满尖峰，
//      电场方向剧烈震荡，节点左右乱跳不收敛
//   2. 稀释真实节点占比——真实单元只占芯片一小部分，没有 filler 时 99% 的区域密度为 0、
//      梯度为 0，节点感知不到力
//   3. 避免节点卡在 bin 边界的密度断层里形成死区
#pragma once

#include <cstdint>

namespace sp {

class PlaceDB;
struct Config;

struct FillerInfo {
    int    count = 0;
    float  width = 0.f;
    float  height = 0.f;
    double whitespaceArea = 0.0;
    double nodeAreaScaled = 0.0;
    double totalFillerArea = 0.0;
};

/// 计算并插入 filler 到 db 的 [numNodes, numNodes + numFillers) 区间。
/// 重复调用会先清除已有 filler。
FillerInfo initializeFillers(PlaceDB& db, const Config& cfg);

/// 重新随机撒布已有 filler 的位置（FILLERONLY 阶段用）。
void scatterFillers(PlaceDB& db, const Config& cfg, uint64_t seedOffset = 0);

}  // namespace sp
