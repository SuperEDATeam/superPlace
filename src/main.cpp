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
        SP_INFO("results in %s", outDir.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
