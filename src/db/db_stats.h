// 数据库统计信息。用于与课程资料给出的 adaptec1 基准逐项对拍（05 §5.1.4）。
// 面积一律用 double 累加，避免 1e8 量级的大数精度损失（05 §6）。
#pragma once

#include <array>
#include <string>

namespace sp {

class PlaceDB;

struct DbStats {
    // 区域
    double coreLx = 0, coreLy = 0, coreHx = 0, coreHy = 0;
    double rowHeight = 0;
    int    rowCount  = 0;
    double siteStep  = 0;

    // 面积
    double coreArea       = 0;   // coreRegion 面积
    double movableArea    = 0;   // 全部可移动节点面积
    double fixedArea      = 0;   // 全部固定节点面积
    double fixedAreaInCore = 0;  // 固定节点与 coreRegion 的重叠面积
    double totalRowArea   = 0;

    // 计数
    int nodeCount    = 0;   // 可移动节点数
    int objectCount  = 0;   // 全部对象数（可移动 + 固定）
    int fixedCount   = 0;
    int macroCount   = 0;
    int netCount     = 0;
    int pinCount     = 0;
    int maxNetDegree = 0;

    /// 精确的 net degree 分布：[deg==1, deg==2, 3..9, 10..99, >=100]
    ///
    /// 说明：课程资料（05 §5.1.4）给出的参考行标注为
    ///   "2 (117104)  3-10 (86566)  11-100 (17470)  100- (2)"
    /// 但实测 adaptec1 的真实分布为
    ///   deg1=1348  deg2=117104  deg3-9=85218  deg10-99=17470  deg>=100=2
    /// 可见参考实现的分箱边界其实是 <10 / <100 / >=100，标签整体差一位，
    /// 且 degree==1 的网络被并入了标着 "3-10" 的那一箱（85218 + 1348 = 86566）。
    /// 这里保留精确的五箱统计，另由 referenceBins() 复现参考口径以便逐项对拍。
    std::array<int, 5> degreeHistogram{{0, 0, 0, 0, 0}};

    /// 复现课程资料的参考分箱口径：[deg==2, deg==1 或 3..9, 10..99, >=100]
    std::array<int, 4> referenceBins() const {
        return {degreeHistogram[1],
                degreeHistogram[0] + degreeHistogram[2],
                degreeHistogram[3],
                degreeHistogram[4]};
    }

    // 派生比例
    double cellAreaRatio() const { return coreArea > 0 ? movableArea / coreArea : 0.0; }
    double fixedAreaRatio() const { return coreArea > 0 ? fixedArea / coreArea : 0.0; }
    double fixedInCoreRatio() const { return coreArea > 0 ? fixedAreaInCore / coreArea : 0.0; }
    /// 可移动面积 / 可用空白（= 行总面积 - 固定占用）
    double placementUtil() const {
        const double freeSites = totalRowArea - fixedAreaInCore;
        return freeSites > 0 ? movableArea / freeSites : 0.0;
    }
    /// (可移动面积 + 核内固定面积) / 核心面积
    double coreDensity() const {
        return coreArea > 0 ? (movableArea + fixedAreaInCore) / coreArea : 0.0;
    }

    static DbStats compute(const PlaceDB& db);

    /// 打印为验收门要求的格式
    std::string toString() const;
};

}  // namespace sp
