#include "stages/parse_stage.h"

#include <cstdio>

#include "db/db_stats.h"
#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"

namespace sp {

void ParseStage::run(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    const DbStats s = DbStats::compute(db);
    std::fputs(s.toString().c_str(), stdout);

    sink.setSummary("benchmark", cfg.benchmark_name);
    sink.setSummary("core_lx", s.coreLx);
    sink.setSummary("core_ly", s.coreLy);
    sink.setSummary("core_hx", s.coreHx);
    sink.setSummary("core_hy", s.coreHy);
    sink.setSummary("core_area", s.coreArea);
    sink.setSummary("row_height", s.rowHeight);
    sink.setSummary("row_count", s.rowCount);
    sink.setSummary("movable_area", s.movableArea);
    sink.setSummary("fixed_area", s.fixedArea);
    sink.setSummary("fixed_area_in_core", s.fixedAreaInCore);
    sink.setSummary("placement_util", s.placementUtil());
    sink.setSummary("core_density", s.coreDensity());
    sink.setSummary("num_movable", s.nodeCount);
    sink.setSummary("num_objects", s.objectCount);
    sink.setSummary("num_fixed", s.fixedCount);
    sink.setSummary("num_macro", s.macroCount);
    sink.setSummary("num_nets", s.netCount);
    sink.setSummary("num_pins", s.pinCount);
    sink.setSummary("max_net_degree", s.maxNetDegree);
}

std::unique_ptr<PlacementStage> makeStage(const std::string& name) {
    if (name == "parse") return std::make_unique<ParseStage>();
    return nullptr;
}

}  // namespace sp
