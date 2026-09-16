// HPWL 与 net 包围盒计算。
//
// 放在 db/ 而非 numeric/：它需要 net/pin 语义，若放进 numeric/ 会违反铁律 1。
// M3 若需要 GPU 版本，再按决策 7 提升为 HpwlBackend——届时接口只收裸数组
// （pos / flat_net2pin / net2pin_start / pin_offset / pin2node），仍不违反铁律 1。
#pragma once

#include <vector>

namespace sp {

class PlaceDB;

/// 半周长线长：Σ_net ((maxX - minX) + (maxY - minY))。
/// degree < 2 的 net 贡献 0。累加用 double，避免 1e8 量级的精度损失。
double computeHPWL(const PlaceDB& db);

/// 每个 net 的引脚包围盒。四个输出数组长度均须为 numNets。
/// degree == 0 的 net 输出 0。
void computeNetBoundBoxes(const PlaceDB& db, std::vector<float>& minX, std::vector<float>& maxX,
                          std::vector<float>& minY, std::vector<float>& maxY);

/// 每个 net 在 x/y 方向取到极值的引脚下标（B2B 模型的边界引脚）。
/// 四个输出数组长度均须为 numNets；degree == 0 的 net 输出 -1。
///
/// 并列极值时统一取**下标最小**者，保证结果可复现（铁律 7）。
void computeNetBoundPins(const PlaceDB& db, std::vector<int>& minPinX, std::vector<int>& maxPinX,
                         std::vector<int>& minPinY, std::vector<int>& maxPinY);

/// 均匀网格上的密度统计，用于任务 4 的「面积控制」维度对比。
struct DensityStats {
    float  maxDensity = 0.f;   // 最拥挤网格的占用率
    float  meanDensity = 0.f;
    float  stdDensity = 0.f;   // 越小说明分布越均匀
    double overflowArea = 0.0; // Σ max(binArea*density - binArea*target, 0)
};

/// 在 dim×dim 网格上统计可移动单元的面积分布。
DensityStats computeDensityStats(const PlaceDB& db, int dim, float targetDensity);

}  // namespace sp
