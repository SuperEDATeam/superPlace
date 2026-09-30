#include "stages/gp_stage.h"

#include <cstdio>
#include <string>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include <cmath>
#include <vector>

#include "gp/bin_grid.h"
#include "gp/eplace.h"
#include "gp/filler.h"
#include "lg/macro_sa.h"
#include "plot/cimg_renderer.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace sp {
namespace {

/// 逐迭代出图。帧按【全局序号】编号而不是各阶段自己从 0 数——
/// 四个阶段的帧要拼成一条连续的收敛动画，各自从 0 开始会互相覆盖。
struct FramePlotter {
    const Config& cfg;
    std::string   dir;
    int           frame = 0;

    void operator()(GpStage stage, int iter, const PlaceDB& db) {
        if (!cfg.full_plot || iter % cfg.plot_interval != 0) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "iter_%05d.png", frame++);
        const std::string caption =
            cfg.benchmark_name + " - " + toString(stage) + " iter " + std::to_string(iter);
        CimgRenderer::plot(dir + "/" + buf, db, cfg, caption);
    }
};

int countOutOfCore(const PlaceDB& db) {
    int n = 0;
    for (int i = 0; i < db.totalNodes(); ++i) {
        // 只检查参与优化的节点。固定终端本来就可以在 core 外——adaptec1 的 480 个
        // I/O pad 全都贴在 core 边界之外，把它们算进来会得到一个纯属虚惊的告警。
        if (db.isFixed(i) || db.isNI(i)) continue;
        if (db.llx(i) < db.coreRegion.lx - 1e-2f || db.urx(i) > db.coreRegion.hx + 1e-2f ||
            db.lly(i) < db.coreRegion.ly - 1e-2f || db.ury(i) > db.coreRegion.hy + 1e-2f)
            ++n;
    }
    return n;
}

}  // namespace

void GlobalPlaceStage::run(PlaceDB& db, const Config& cfg, MetricsSink& sink) {
    const double hpwlBefore = computeHPWL(db);

    int movableMacros = 0;
    for (int i = 0; i < db.numMovable; ++i)
        if (db.isMacro(i) && !db.isFixed(i)) ++movableMacros;

    const FillerInfo fi = initializeFillers(db, cfg);
    SP_INFO("gp: HPWL before %.6g，可移动宏 %d 个，filler %d 个", hpwlBefore, movableMacros,
            fi.count);

    FramePlotter plotter{cfg, sink.outputDir() + "/plots", 0};
    Timer timer;

    // ---- 阶段一：mGP（混合尺寸全局布局，标准单元与宏一起优化）
    GpResult mgp;
    {
        EPlace engine(db, cfg, sink);
        engine.setIterCallback(std::ref(plotter));
        mgp = engine.run(GpStage::kMGP);
    }
    sink.setSummary("mgp_hpwl", mgp.hpwl);
    sink.setSummary("mgp_overflow", mgp.overflow);
    sink.setSummary("mgp_iterations", mgp.iterations);

    double finalHpwl = mgp.hpwl;
    float finalOverflow = mgp.overflow;

    // 无可移动宏时后三个阶段无事可做：没有宏要合法化，也就没有"宏固定后重新收敛"
    // 这回事。ISPD2005/2006 原版数据全部落在这一支（8.14），这是数据特性不是 bug。
    if (movableMacros == 0) {
        SP_INFO("gp: 无可移动宏，mLG / FILLERONLY / cGP 三阶段跳过");
    } else {
        // mLG 前的宏位置，用于量化"宏被挪了多远"——这是理解 mLG 与 cGP
        // 之间取舍的关键量，见下面对 τ 的对照。
        std::vector<float> preX, preY;
        for (int i = 0; i < db.numMovable; ++i) {
            if (!db.isMacro(i)) continue;
            preX.push_back(db.node_x[static_cast<size_t>(i)]);
            preY.push_back(db.node_y[static_cast<size_t>(i)]);
        }

        // ---- 阶段二：mLG（宏模拟退火合法化，结束后宏被钉死）
        const MacroSaResult mlg = legalizeMacros(db, cfg, sink);

        // mLG 把宏钉死会改变密度场的构成（宏从可移动电荷变成固定阻挡），
        // 这一步之后的 τ 才是 cGP 真正要面对的初值，必须单独量出来。
        {
            BinGrid g;
            g.initialize(db, cfg.target_density, cfg.gp_bin_dim, cfg.density_chunks);
            g.accumulate(db);
            double sum = 0.0, mx = 0.0;
            size_t k = 0;
            for (int i = 0; i < db.numMovable; ++i) {
                if (!db.isMacro(i)) continue;
                const double d = std::hypot(db.node_x[static_cast<size_t>(i)] - preX[k],
                                            db.node_y[static_cast<size_t>(i)] - preY[k]);
                sum += d;
                mx = std::max(mx, d);
                ++k;
            }
            const double meanDisp = k ? sum / static_cast<double>(k) : 0.0;
            SP_INFO("mLG 后：τ=%.4f（cGP 的实际初值），宏位移 均值 %.1f / 最大 %.1f",
                    g.overflow(), meanDisp, mx);
            sink.setSummary("mlg_overflow_after", g.overflow());
            sink.setSummary("mlg_macro_disp_mean", meanDisp);
            sink.setSummary("mlg_macro_disp_max", mx);
        }
        sink.setSummary("mlg_overlap_before", mlg.overlapBefore);
        sink.setSummary("mlg_overlap_after", mlg.overlapAfter);
        sink.setSummary("mlg_hpwl", mlg.hpwlAfter);
        sink.setSummary("mlg_repaired", mlg.repaired);

        // 宏的位置此刻已定。记下来，用于验收"cGP 期间宏位置逐位不变"。
        std::vector<float> macroX, macroY;
        for (int i = 0; i < db.numMovable; ++i) {
            if (!db.isMacro(i)) continue;
            macroX.push_back(db.node_x[static_cast<size_t>(i)]);
            macroY.push_back(db.node_y[static_cast<size_t>(i)]);
        }

        // 宏被钉死后，它们的面积从"可移动电荷"变成"固定阻挡"，必须重建 bin 网格
        // 才能把这部分面积从 nodeDensity 转入 terminalDensity。因此下面两个阶段
        // 各自新建 EPlace——构造函数里就会重算全部静态密度项。

        // ---- 阶段三：FILLERONLY（重撒 filler，只优化 filler，固定轮数）
        //
        // 宏刚被挪动过，原来的 filler 分布是围着旧宏位置形成的，已经失效。
        // 重撒后只让 filler 跑几轮，把空白重新填匀，为 cGP 提供干净的密度初值。
        scatterFillers(db, cfg, /*seedOffset=*/1);
        GpResult fo;
        {
            EPlace engine(db, cfg, sink);
            engine.setIterCallback(std::ref(plotter));
            fo = engine.run(GpStage::kFillerOnly);
        }
        sink.setSummary("filleronly_iterations", fo.iterations);

        // ---- 阶段四：cGP（标准单元全局布局，宏不再参与）
        GpResult cgp;
        {
            EPlace engine(db, cfg, sink);
            engine.setIterCallback(std::ref(plotter));
            cgp = engine.run(GpStage::kCGP);
        }
        sink.setSummary("cgp_hpwl", cgp.hpwl);
        sink.setSummary("cgp_overflow", cgp.overflow);
        sink.setSummary("cgp_iterations", cgp.iterations);
        finalHpwl = cgp.hpwl;
        finalOverflow = cgp.overflow;

        // 验收：cGP 期间宏一步都不该动
        int moved = 0;
        size_t k = 0;
        for (int i = 0; i < db.numMovable; ++i) {
            if (!db.isMacro(i)) continue;
            if (db.node_x[static_cast<size_t>(i)] != macroX[k] ||
                db.node_y[static_cast<size_t>(i)] != macroY[k])
                ++moved;
            ++k;
        }
        sink.setSummary("cgp_macros_moved", moved);
        if (moved > 0)
            SP_ERROR("cGP 期间有 %d 个宏发生位移，它们本应已被钉死", moved);
        else
            SP_INFO("cGP 期间 %zu 个宏位置逐位不变", macroX.size());
    }

    const double elapsed = timer.elapsedMs();
    const int outOfCore = countOutOfCore(db);
    if (outOfCore > 0) SP_WARN("gp: %d 个节点越出 coreRegion", outOfCore);

    sink.setSummary("gp_hpwl_before", hpwlBefore);
    sink.setSummary("gp_hpwl", finalHpwl);
    sink.setSummary("gp_overflow", finalOverflow);
    sink.setSummary("gp_num_fillers", fi.count);
    sink.setSummary("gp_movable_macros", movableMacros);
    sink.setSummary("gp_out_of_core", outOfCore);
    sink.setSummary("gp_time_ms", elapsed);
    sink.setSummary("gp_frames", plotter.frame);

    SP_INFO("gp done: HPWL %.6g -> %.6g  τ=%.4f  (%.1f ms)", hpwlBefore, finalHpwl, finalOverflow,
            elapsed);
}

}  // namespace sp
