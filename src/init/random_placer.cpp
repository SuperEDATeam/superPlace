// 方法一：随机布局。
// 最简单的基线——区域内均匀撒点，不考虑连接关系也不考虑重叠。
// 它的价值在于给另外两种方法提供一个"什么都不做"的对照基准。
#include "init/random_placer.h"

#include "db/hpwl.h"
#include "db/place_db.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/rng.h"
#include "util/timer.h"

namespace sp {

void RandomPlacer::place(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    Timer timer;
    // 铁律 7：随机源统一走 Rng(seed)，禁止 rand()/random_device/时间种子
    Rng rng(cfg.random_seed);

    const Rect& core = db.coreRegion;
    for (int i = 0; i < db.numMovable; ++i) {
        const float hw = 0.5f * db.node_w[i];
        const float hh = 0.5f * db.node_h[i];
        // 取中心的可行区间，保证整个单元落在 core 内
        const float lo_x = core.lx + hw, hi_x = core.hx - hw;
        const float lo_y = core.ly + hh, hi_y = core.hy - hh;
        db.node_x[i] = (hi_x > lo_x) ? rng.uniform(lo_x, hi_x) : core.cx();
        db.node_y[i] = (hi_y > lo_y) ? rng.uniform(lo_y, hi_y) : core.cy();
    }

    IterMetrics m;
    m.iter = 0;
    m.hpwl = computeHPWL(db);
    m.elapsed_ms = timer.elapsedMs();
    sink.push(m);
    SP_INFO("random: HPWL = %.6g  (%.1f ms)", m.hpwl, m.elapsed_ms);
}

}  // namespace sp
