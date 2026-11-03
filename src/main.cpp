// superplace-cli：薄 CLI 前端。
// 只做三件事：解析参数 -> 按 --stage 组装 pipeline -> 依次 run 并落快照。
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "db/place_db.h"
#include "io/bookshelf_reader.h"
#include "io/bookshelf_writer.h"
#include "io/viz_writer.h"
#include "plot/cimg_renderer.h"
#include "stage.h"
#include "util/config.h"
#include "util/logger.h"
#include "util/metrics.h"
#include "util/timer.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    sp::Config cfg;
    try {
        cfg = sp::Config::fromArgs(argc, argv);
    } catch (const std::invalid_argument&) {
        std::fputs(sp::Config::usage().c_str(), stdout);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n\n%s", e.what(), sp::Config::usage().c_str());
        return 2;
    }

    if (cfg.verbose) sp::Logger::get().setLevel(sp::LogLevel::kDebug);
#ifdef _OPENMP
    if (cfg.num_threads > 0) omp_set_num_threads(cfg.num_threads);
#endif

    try {
        // ---- 输出目录：<out>/<benchmark>/
        const fs::path outDir = fs::path(cfg.out_dir) / cfg.benchmark_name;
        const fs::path plotDir = outDir / "plots";
        fs::create_directories(plotDir);

        sp::MetricsSink sink;
        sink.setOutputDir(outDir.string());
        sink.setSummary("benchmark", cfg.benchmark_name);
        sink.setSummary("aux_path", cfg.aux_path);

        // ---- 解析
        sp::PlaceDB db;
        {
            sp::Timer t;
            sp::BookshelfReader reader;
            reader.read(cfg.aux_path, db);
            sink.recordDuration("read", t.elapsedMs());
            SP_INFO("parse done in %.1f ms", t.elapsedMs());
        }

        // ---- 录制（可选）：给前端回放用的 viz bundle
        //
        // 必须在任何阶段运行之前打开：几何与标志位在这里取一次，而 mLG 会把宏置为
        // F_FIXED，之后再取的话宏在前端会被当成固定终端着色。
        sp::VizBundleWriter viz;
        if (cfg.frame_stride > 0) {
            viz.open((outDir / "viz").string(), cfg.benchmark_name, db, cfg.frame_stride);
            sink.setFrameHook([&viz, &db](const sp::FrameMark& m) { viz.append(db, m); });
            sink.markFrame({"input", -1, 0.0, false, "输入（.pl 初始位置）"});
        }

        // ---- 组装并执行 pipeline
        for (const std::string& stageName : cfg.stages) {
            std::unique_ptr<sp::PlacementStage> stage = sp::makeStage(stageName);
            if (!stage) {
                std::fprintf(stderr, "error: unknown stage '%s'\n", stageName.c_str());
                return 2;
            }

            sp::Timer t;
            sink.beginStage(stage->name());
            stage->run(db, cfg, sink);
            const double ms = t.elapsedMs();
            sink.endStage(ms);
            SP_INFO("stage '%s' finished in %.1f ms", stage->name(), ms);

            // 每个阶段结束补一个里程碑帧。阶段内部没录逐轮帧时（如二次布局），
            // 这一帧与上一帧之间是一次跳变，前端不得插值。gp 阶段自己会录收尾帧，
            // 这里再录的那一帧位置完全相同，会被写出器按重复帧跳过。
            sink.markFrame({stage->name(), -1, viz.lastT(), true,
                            std::string(stage->name()) + " 结束"});

            // 每阶段自动落 .pl 快照
            const fs::path plPath = outDir / (cfg.benchmark_name + "." + stage->name() + ".pl");
            sp::BookshelfWriter::writePl(plPath.string(), db);
            SP_INFO("placement written: %s", plPath.c_str());

            // 以及一张 PNG
            if (cfg.plot) {
                const fs::path png = plotDir / (std::string(stage->name()) + ".png");
                sp::CimgRenderer::plot(png.string(), db, cfg,
                                       cfg.benchmark_name + " - " + stage->name());
            }
        }

        sink.writeSummary((outDir / "summary.json").string());
        viz.finish(outDir.string());
        SP_INFO("results in %s", outDir.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
