// Paired full-pipeline experiment: share unchanged QP/mGP, then deep-copy the
// complete database and run reference/current mLG -> FILLERONLY -> cGP.
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>

#include "db/hpwl.h"
#include "db/place_db.h"
#include "gp/bin_grid.h"
#include "gp/eplace.h"
#include "gp/filler.h"
#include "io/bookshelf_reader.h"
#include "io/bookshelf_writer.h"
#include "lg/macro_sa.h"
#include "stages/init_stage.h"
#include "util/config.h"
#include "util/metrics.h"
#include "util/timer.h"
#ifdef _OPENMP
#include <omp.h>
#endif

namespace sp {
// Diagnostic symbol generated from the pinned reference, not a production API.
MacroSaResult legalizeMacrosReference(PlaceDB&, const Config&, MetricsSink&);
}
namespace fs = std::filesystem;

namespace {
void finish(sp::PlaceDB& db, const sp::Config& cfg, sp::MetricsSink& sink,
            const sp::GpResult& mgp, double initialHpwl, int fillers, bool reference) {
    sp::Timer timer;
    std::vector<int> macros;
    std::vector<float> originalX, originalY;
    for (int i = 0; i < db.numMovable; ++i) if (db.isMacro(i) && !db.isFixed(i)) {
        macros.push_back(i);
        originalX.push_back(db.node_x[i]); originalY.push_back(db.node_y[i]);
    }
    double finalHpwl = mgp.hpwl;
    float finalOverflow = mgp.overflow;
    if (!macros.empty()) {
        const auto mlg = reference ? sp::legalizeMacrosReference(db, cfg, sink)
                                   : sp::legalizeMacros(db, cfg, sink);
        if (mlg.overlapAfter > 0) throw std::runtime_error("paired mLG residual overlap");
        double sum = 0, mx = 0;
        std::vector<float> fixedX, fixedY;
        for (size_t s = 0; s < macros.size(); ++s) {
            const int i = macros[s];
            const double d = std::hypot(double(db.node_x[i] - originalX[s]),
                                        double(db.node_y[i] - originalY[s]));
            sum += d; mx = std::max(mx, d);
            fixedX.push_back(db.node_x[i]); fixedY.push_back(db.node_y[i]);
        }
        sp::BinGrid grid;
        grid.initialize(db, cfg.target_density, cfg.gp_bin_dim, cfg.density_chunks);
        grid.accumulate(db);
        sink.setSummary("mlg_overflow_after", grid.overflow());
        sink.setSummary("mlg_macro_disp_mean", sum / macros.size());
        sink.setSummary("mlg_macro_disp_max", mx);
        sink.setSummary("mlg_overlap_before", mlg.overlapBefore);
        sink.setSummary("mlg_overlap_after", mlg.overlapAfter);
        sink.setSummary("mlg_hpwl", mlg.hpwlAfter);
        sink.setSummary("mlg_repaired", mlg.repaired);
        sp::scatterFillers(db, cfg, 1);
        sp::GpResult fo, cgp;
        { sp::EPlace e(db, cfg, sink); fo = e.run(sp::GpStage::kFillerOnly); }
        { sp::EPlace e(db, cfg, sink); cgp = e.run(sp::GpStage::kCGP); }
        sink.setSummary("filleronly_iterations", fo.iterations);
        sink.setSummary("cgp_hpwl", cgp.hpwl);
        sink.setSummary("cgp_overflow", cgp.overflow);
        sink.setSummary("cgp_iterations", cgp.iterations);
        int moved = 0;
        for (size_t s = 0; s < macros.size(); ++s)
            if (db.node_x[macros[s]] != fixedX[s] || db.node_y[macros[s]] != fixedY[s]) ++moved;
        sink.setSummary("cgp_macros_moved", moved);
        if (moved) throw std::runtime_error("paired cGP moved a frozen macro");
        finalHpwl = cgp.hpwl; finalOverflow = cgp.overflow;
    }
    int outside = 0;
    for (int i = 0; i < db.totalNodes(); ++i) {
        if (db.isFixed(i) || db.isNI(i)) continue;
        if (db.llx(i) < db.coreRegion.lx - 1e-2f || db.urx(i) > db.coreRegion.hx + 1e-2f ||
            db.lly(i) < db.coreRegion.ly - 1e-2f || db.ury(i) > db.coreRegion.hy + 1e-2f) ++outside;
    }
    sink.setSummary("gp_out_of_core", outside);
    sink.setSummary("gp_hpwl_before", initialHpwl);
    sink.setSummary("gp_hpwl", finalHpwl);
    sink.setSummary("gp_overflow", finalOverflow);
    sink.setSummary("gp_num_fillers", fillers);
    sink.setSummary("gp_movable_macros", static_cast<double>(macros.size()));
    sink.recordDuration("paired_tail", timer.elapsedMs());
    if (outside) throw std::runtime_error("paired output outside core");
    sp::BookshelfWriter::writePl(sink.outputDir() + "/" + cfg.benchmark_name + ".gp.pl", db);
    sink.writeSummary(sink.outputDir() + "/summary.json");
}
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s <aux> <config.json> <reference-run> <candidate-run>\n", argv[0]);
        return 2;
    }
    try {
        sp::Config cfg;
        cfg.loadJson(argv[2]);
        if (cfg.lambda_schedule != "fixed" || cfg.poisson_backend != "cpu")
            throw std::runtime_error("paired legalizer comparison requires fixed CPU schedule");
        cfg.aux_path = argv[1];
        cfg.benchmark_name = fs::path(argv[1]).stem().string();
#ifdef _OPENMP
        omp_set_num_threads(cfg.num_threads > 0 ? cfg.num_threads : 16);
#endif
        const fs::path baseRun = argv[3], candidateRun = argv[4];
        const auto baseOut = baseRun / cfg.benchmark_name;
        const auto candidateOut = candidateRun / cfg.benchmark_name;
        fs::create_directories(baseOut); fs::create_directories(candidateOut);
        if (!std::freopen((baseRun / "run.log").c_str(), "w", stdout))
            throw std::runtime_error("cannot open reference log");
        sp::PlaceDB db;
        sp::BookshelfReader{}.read(cfg.aux_path, db);
        sp::MetricsSink base;
        base.setOutputDir(baseOut.string());
        base.setSummary("benchmark", cfg.benchmark_name);
        base.setSummary("aux_path", cfg.aux_path);
        base.beginStage("init");
        sp::InitStage{}.run(db, cfg, base);
        base.endStage(0.0);
        sp::BookshelfWriter::writePl((baseOut / (cfg.benchmark_name + ".init.pl")).string(), db);
        fs::copy_file(baseOut / (cfg.benchmark_name + ".init.pl"),
                      candidateOut / (cfg.benchmark_name + ".init.pl"), fs::copy_options::overwrite_existing);
        const double initialHpwl = sp::computeHPWL(db);
        const auto fi = sp::initializeFillers(db, cfg);
        sp::GpResult mgp;
        { sp::EPlace e(db, cfg, base); mgp = e.run(sp::GpStage::kMGP); }
        base.setSummary("mgp_hpwl", mgp.hpwl);
        base.setSummary("mgp_overflow", mgp.overflow);
        base.setSummary("mgp_iterations", mgp.iterations);
        sp::BookshelfWriter::writePl((baseOut / (cfg.benchmark_name + ".mgp.pl")).string(), db);
        fs::copy_file(baseOut / (cfg.benchmark_name + ".mgp.pl"),
                      candidateOut / (cfg.benchmark_name + ".mgp.pl"), fs::copy_options::overwrite_existing);
        std::fflush(stdout);
        fs::copy_file(baseRun / "run.log", candidateRun / "run.log", fs::copy_options::overwrite_existing);
        sp::PlaceDB candidateDb = db;  // Deep copy includes filler/CSR/pin/flag state.
        sp::MetricsSink candidate = base;
        candidate.setOutputDir(candidateOut.string());
        finish(db, cfg, base, mgp, initialHpwl, fi.count, true);
        if (!std::freopen((candidateRun / "run.log").c_str(), "a", stdout))
            throw std::runtime_error("cannot open candidate log");
        finish(candidateDb, cfg, candidate, mgp, initialHpwl, fi.count, false);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "paired M3: %s\n", e.what());
        return 1;
    }
}
