#include "stages/lg_stage.h"

#include "db/hpwl.h"
#include "lg/abacus.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {

void LegalizeStage::run(PlaceDB& db, const Config&, MetricsSink& sink) {
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.isMacro(i) && !db.isFixed(i)) {
            SP_ERROR("lg 需要在 gp 之后运行（宏尚未合法化）");
            return;
        }
    }
    Timer timer;
    const double before = computeHPWL(db);
    const auto result = legalizeCells(db);
    const double after = computeHPWL(db);
    const double ms = timer.elapsedMs();
    sink.setSummary("lg_hpwl_before", before);
    sink.setSummary("lg_hpwl", after);
    sink.setSummary("lg_cells", result.cells);
    sink.setSummary("lg_failed", result.failed);
    sink.setSummary("lg_disp_mean", result.dispMean);
    sink.setSummary("lg_disp_max", result.dispMax);
    sink.setSummary("lg_rows", result.rows);
    sink.setSummary("lg_segments", result.segments);
    sink.setSummary("lg_time_ms", ms);
    sink.setSummary("lg_selfcheck_overlaps", result.selfcheckOverlaps);
    if (result.roundedWidths) SP_WARN("lg: %d 个单元宽度向上对齐 site", result.roundedWidths);
    if (result.selfcheckOffSite) SP_WARN("lg: %d 个单元未通过行段/site 自检", result.selfcheckOffSite);
    SP_INFO("lg: %d 个单元，HPWL %.6g -> %.6g (%+.2f%%)，位移 均值 %.3f / 最大 %.3f，失败 %d，耗时 %.3f s",
            result.cells, before, after, before > 0.0 ? (after / before - 1.0) * 100.0 : 0.0,
            result.dispMean, result.dispMax, result.failed, ms / 1000.0);
}

}  // namespace sp
