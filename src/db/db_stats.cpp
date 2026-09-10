#include "db/db_stats.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "db/place_db.h"

namespace sp {

DbStats DbStats::compute(const PlaceDB& db) {
    DbStats s;

    s.coreLx = db.coreRegion.lx;
    s.coreLy = db.coreRegion.ly;
    s.coreHx = db.coreRegion.hx;
    s.coreHy = db.coreRegion.hy;
    s.rowHeight = db.rowHeight;
    s.rowCount  = static_cast<int>(db.rows.size());
    s.siteStep  = db.rows.empty() ? 0.0 : db.rows.front().step;

    // coreArea 用 double 从边界重算，避免 float area() 的精度损失
    s.coreArea = (s.coreHx - s.coreLx) * (s.coreHy - s.coreLy);
    s.totalRowArea = db.totalRowArea;

    for (int i = 0; i < db.numNodes; ++i) {
        const double a = db.area(i);
        if (db.isFixed(i)) {
            s.fixedArea += a;
            ++s.fixedCount;
            // 固定节点与 coreRegion 的重叠（double 精度，不复用 float 版 overlapArea）
            const double dx = std::min<double>(db.urx(i), s.coreHx) - std::max<double>(db.llx(i), s.coreLx);
            const double dy = std::min<double>(db.ury(i), s.coreHy) - std::max<double>(db.lly(i), s.coreLy);
            if (dx > 0.0 && dy > 0.0) s.fixedAreaInCore += dx * dy;
        } else {
            s.movableArea += a;
            ++s.nodeCount;
            if (db.isMacro(i)) ++s.macroCount;
        }
    }

    s.objectCount = db.numNodes;
    s.netCount    = db.numNets;
    s.pinCount    = db.numPins;

    for (int k = 0; k < db.numNets; ++k) {
        const int deg = db.netDegree(k);
        s.maxNetDegree = std::max(s.maxNetDegree, deg);
        if (deg == 1)                    s.degreeHistogram[0]++;
        else if (deg == 2)               s.degreeHistogram[1]++;
        else if (deg >= 3 && deg <= 9)   s.degreeHistogram[2]++;
        else if (deg >= 10 && deg <= 99) s.degreeHistogram[3]++;
        else if (deg >= 100)             s.degreeHistogram[4]++;
    }

    return s;
}

std::string DbStats::toString() const {
    char buf[512];
    std::ostringstream os;

    auto line = [&](const char* fmt, auto... args) {
        std::snprintf(buf, sizeof(buf), fmt, args...);
        os << buf << '\n';
    };

    os << "\n<<<< DATABASE SUMMARY >>>>\n\n";
    line("       Core region: (%.0f, %.0f) - (%.0f, %.0f)", coreLx, coreLy, coreHx, coreHy);
    line("Row height / count: %.0f / %d  (site step %.6f)", rowHeight, rowCount, siteStep);
    line("         Core area: %.0f", coreArea);
    line("         Cell area: %.0f  (%.2f%%)", movableArea, cellAreaRatio() * 100.0);
    line("      Movable area: %.0f  (%.2f%%)", movableArea, cellAreaRatio() * 100.0);
    line("        Fixed area: %.0f  (%.2f%%)", fixedArea, fixedAreaRatio() * 100.0);
    line(" Fixed area in core: %.0f  (%.2f%%)", fixedAreaInCore, fixedInCoreRatio() * 100.0);
    line("   Placement util.: %.2f%%  (= move / freeSites)", placementUtil() * 100.0);
    line("      Core density: %.2f%%  (= usedArea / core)", coreDensity() * 100.0);
    line("             Nodes: %d", nodeCount);
    line("           Objects: %d  (fixed: %d, macro: %d)", objectCount, fixedCount, macroCount);
    line("              Nets: %d", netCount);
    line("              Pins: %d", pinCount);
    line("    Max net degree: %d", maxNetDegree);
    // 参考口径（与课程资料 05 §5.1.4 逐项可比；标签沿用其原文）
    const std::array<int, 4> ref = referenceBins();
    line("   Pin degree dist: 2:(%d)  3-10:(%d)  11-100:(%d)  100-:(%d)   [reference bins]",
         ref[0], ref[1], ref[2], ref[3]);
    // 精确口径（真实边界）
    line("   Exact degree   : 1:(%d)  2:(%d)  3-9:(%d)  10-99:(%d)  >=100:(%d)",
         degreeHistogram[0], degreeHistogram[1], degreeHistogram[2],
         degreeHistogram[3], degreeHistogram[4]);
    os << '\n';
    return os.str();
}

}  // namespace sp
