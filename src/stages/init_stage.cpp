#include "stages/init_stage.h"

#include <stdexcept>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "init/initial_placer.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {

void InitStage::run(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    std::unique_ptr<IInitialPlacer> placer = makeInitialPlacer(cfg.init_method);
    if (!placer) {
        std::string msg = "unknown --init-method '" + cfg.init_method + "', expected one of:";
        for (const std::string& n : initialPlacerNames()) msg += " " + n;
        throw std::runtime_error(msg);
    }

    const double hpwlBefore = computeHPWL(db);
    SP_INFO("init method: %s  (HPWL before: %.6g)", placer->name(), hpwlBefore);

    Timer timer;
    placer->place(db, cfg, sink);
    const double elapsed = timer.elapsedMs();

    const double hpwlAfter = computeHPWL(db);
    const DensityStats ds = computeDensityStats(db, cfg.density_stat_dim, cfg.target_density);

    // 越界检查：所有可移动单元必须完整落在 coreRegion 内
    int outOfCore = 0;
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.llx(i) < db.coreRegion.lx - 1e-3f || db.urx(i) > db.coreRegion.hx + 1e-3f ||
            db.lly(i) < db.coreRegion.ly - 1e-3f || db.ury(i) > db.coreRegion.hy + 1e-3f)
            ++outOfCore;
    }
    if (outOfCore > 0) SP_WARN("%d movable cells lie outside coreRegion", outOfCore);

    sink.setSummary("init_method", std::string(placer->name()));
    sink.setSummary("init_hpwl", hpwlAfter);
    sink.setSummary("init_hpwl_before", hpwlBefore);
    sink.setSummary("init_time_ms", elapsed);
    sink.setSummary("init_max_density", ds.maxDensity);
    sink.setSummary("init_mean_density", ds.meanDensity);
    sink.setSummary("init_std_density", ds.stdDensity);
    sink.setSummary("init_overflow_area", ds.overflowArea);
    sink.setSummary("init_out_of_core", outOfCore);

    SP_INFO("init done: HPWL %.6g  maxDensity %.3f  stdDensity %.3f  (%.1f ms)", hpwlAfter,
            ds.maxDensity, ds.stdDensity, elapsed);
}

}  // namespace sp
