#include "gp/filler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/rng.h"

namespace sp {
namespace {

/// 删除已有 filler（重复调用 initializeFillers 时）
void clearFillers(PlaceDB& db) {
    if (db.numFillers == 0) return;
    const size_t keep = static_cast<size_t>(db.numNodes);
    db.node_x.resize(keep);
    db.node_y.resize(keep);
    db.node_w.resize(keep);
    db.node_h.resize(keep);
    db.node_flags.resize(keep);
    db.node_orient.resize(keep);
    db.node_name.resize(keep);
    db.numFillers = 0;
}

}  // namespace

void scatterFillers(PlaceDB& db, const Config& cfg, uint64_t seedOffset) {
    if (db.numFillers == 0) return;
    Rng rng(cfg.random_seed + seedOffset);
    const Rect& core = db.coreRegion;
    for (int i = db.numNodes; i < db.totalNodes(); ++i) {
        const float hw = 0.5f * db.node_w[i];
        const float hh = 0.5f * db.node_h[i];
        const float lo_x = core.lx + hw, hi_x = core.hx - hw;
        const float lo_y = core.ly + hh, hi_y = core.hy - hh;
        db.node_x[i] = (hi_x > lo_x) ? rng.uniform(lo_x, hi_x) : core.cx();
        db.node_y[i] = (hi_y > lo_y) ? rng.uniform(lo_y, hi_y) : core.cy();
    }
}

FillerInfo initializeFillers(PlaceDB& db, const Config& cfg) {
    clearFillers(db);

    FillerInfo info;
    const float target = cfg.target_density;

    // ---- 1. 空白面积 = 可放置行总面积 − 固定节点与行的重叠
    double overlap = 0.0;
    for (int i = db.numMovable; i < db.numNodes; ++i) {
        if (db.isNI(i)) continue;                 // 零面积 IO 引脚不占位
        const Rect box = db.box(i);
        for (const PlaceDB::SiteRow& row : db.rows) {
            // 行按 y 单调，越过即可停（rows 来自 .scl，本就按 y 递增）
            if (row.ly >= box.hy) break;
            if (row.hy() <= box.ly) continue;
            overlap += static_cast<double>(overlapArea(box, row.rect()));
        }
    }
    info.whitespaceArea = std::max(0.0, db.totalRowArea - overlap);

    // ---- 2. 可移动节点的缩放面积
    double cellArea = 0.0, macroArea = 0.0;
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.isMacro(i)) macroArea += db.area(i);
        else cellArea += db.area(i);
    }
    info.nodeAreaScaled = cellArea + macroArea * static_cast<double>(target);

    // ---- 3. filler 总面积与单个尺寸
    info.totalFillerArea = info.whitespaceArea * static_cast<double>(target) - info.nodeAreaScaled;

    if (db.numMovable == 0) {
        SP_INFO("filler: no movable nodes, skip");
        return info;
    }

    // 单个 filler 面积取**中间 80%** 节点的平均面积。
    // ePlace 论文 §3.2 原文是 "the average size of the mid-80% of movable cells"，
    // 即剔除最大最小各 10% 作为噪声。easyPlace 取的是 5%~95%（即 90%），
    // 课程笔记的"90%"说法来源于此——以论文为准。
    std::vector<double> areas;
    areas.reserve(static_cast<size_t>(db.numMovable));
    for (int i = 0; i < db.numMovable; ++i) areas.push_back(db.area(i));
    std::sort(areas.begin(), areas.end());
    const int lo = static_cast<int>(0.10 * db.numMovable);
    const int hi = static_cast<int>(0.90 * db.numMovable);
    double avg = 0.0;
    if (hi > lo) {
        for (int i = lo; i < hi; ++i) avg += areas[static_cast<size_t>(i)];
        avg /= (hi - lo);
    } else {
        avg = areas[static_cast<size_t>(db.numMovable / 2)];
    }

    info.height = (db.rowHeight > 0.f) ? db.rowHeight : 1.f;
    info.width = static_cast<float>(avg) / info.height;
    if (!(info.width > 0.f)) info.width = 1.f;
    const double singleArea = static_cast<double>(info.width) * info.height;

    // ---- 4. 数量
    //
    // 注意括号：是 (总面积 / 单个面积) + 0.5，即**四舍五入**，
    // `+0.5` 不在分母上。课程笔记此处误加括号（已勘误）。
    //
    // 且必须有下界保护：拥挤设计下 totalFillerArea 为负，
    // 直接拿负数去 resize 会被转成巨大的 size_t 而崩溃（easyPlace 无此保护）。
    const double raw = info.totalFillerArea / std::max(singleArea, 1e-9);
    info.count = std::max(0, static_cast<int>(raw + 0.5));

    if (info.count == 0) {
        SP_WARN("filler: whitespace*%.2f (%.0f) <= node area (%.0f), no filler inserted", target,
                info.whitespaceArea * target, info.nodeAreaScaled);
        return info;
    }

    // ---- 5. 插入并随机撒点
    const int base = db.numNodes;
    db.node_x.reserve(static_cast<size_t>(base + info.count));
    for (int i = 0; i < info.count; ++i) {
        db.addNode("filler" + std::to_string(i), info.width, info.height, F_FILLER);
    }
    db.numFillers = info.count;
    // node2pin_start 需要延长——filler 没有引脚，全部指向末尾
    db.node2pin_start.resize(static_cast<size_t>(db.totalNodes()) + 1, db.numPins);

    scatterFillers(db, cfg);

    SP_INFO("filler: %d cells of %.2fx%.2f (whitespace %.0f, nodeScaled %.0f, fillerArea %.0f)",
            info.count, info.width, info.height, info.whitespaceArea, info.nodeAreaScaled,
            info.totalFillerArea);
    (void)base;
    return info;
}

}  // namespace sp
