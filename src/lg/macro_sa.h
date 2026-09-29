// 宏单元合法化：模拟退火（05 §5.6.2，课程任务 7 的第一部分）。
//
// 目标是消除宏之间的重叠，同时控制线长增量。mGP 结束时宏的位置是连续优化的产物，
// 彼此可以任意重叠；后续的标准单元合法化（Abacus，M5）要求宏已经就位且不再移动，
// 所以必须先把宏摆到互不重叠的合法位置上。
//
// 本阶段只处理【宏与宏】以及【宏与固定阻挡】的重叠。宏与标准单元的重叠不在此处理——
// 标准单元会在 cGP 与 Abacus 中流动绕开宏，提前约束它们只会白白劣化线长。
#pragma once

#include <cstdint>

namespace sp {

class PlaceDB;
class MetricsSink;
struct Config;

struct MacroSaResult {
    int    numMacros     = 0;
    int    moves         = 0;   ///< 尝试的移动次数
    int    accepted      = 0;   ///< 被接受的次数
    int    repaired      = 0;   ///< SA 后仍需贪心修复的宏个数
    double overlapBefore = 0.0;
    double overlapAfter  = 0.0;
    double hpwlBefore    = 0.0;
    double hpwlAfter     = 0.0;
};

/// 对 db 中所有可移动宏做合法化，结束后把它们置为 F_FIXED。
/// 无可移动宏时直接返回（这是 ISPD2005/2006 原版数据上的正常情况）。
MacroSaResult legalizeMacros(PlaceDB& db, const Config& cfg, MetricsSink& sink);

}  // namespace sp
