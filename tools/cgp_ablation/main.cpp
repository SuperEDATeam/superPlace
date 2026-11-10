// Identical in-memory cGP initial state; only the named variable differs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <stdexcept>

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
#ifdef _OPENMP
#include <omp.h>
#endif

int main(int argc, char** argv) {
    try {
        sp::Config cfg = sp::Config::fromArgs(argc, argv);
        if (cfg.poisson_backend != "cpu") throw std::runtime_error("CPU ablations only");
#ifdef _OPENMP
        omp_set_num_threads(cfg.num_threads > 0 ? cfg.num_threads : 16);
#endif
        sp::PlaceDB db;
        sp::BookshelfReader reader;
        reader.read(cfg.aux_path, db);
        sp::MetricsSink sink;
        sink.setOutputDir(cfg.out_dir + "/preparation");
        sink.beginStage("init");
        sp::InitStage{}.run(db, cfg, sink);
        sink.endStage(0.0);
        sp::initializeFillers(db, cfg);
        { sp::EPlace e(db, cfg, sink); e.run(sp::GpStage::kMGP); }
        sp::BookshelfWriter::writePl(cfg.out_dir + "/preparation/input.mgp.pl", db);
        const auto mlg = sp::legalizeMacros(db, cfg, sink);
        if (mlg.overlapAfter > 0.0) throw std::runtime_error("mLG failed");
        sp::scatterFillers(db, cfg, 1);
        const sp::PlaceDB beforeFiller = db;
        { sp::EPlace e(db, cfg, sink); e.run(sp::GpStage::kFillerOnly); }
        sink.writeSummary(cfg.out_dir + "/preparation/summary.json");
        const sp::PlaceDB initial = std::move(db);

        for (const std::string name : {"baseline", "no_backtrack", "fixed_gamma",
                                        "fixed_lambda", "filler100"}) {
            sp::PlaceDB trial = name == "filler100" ? beforeFiller : initial;
            sp::Config variant = cfg;
            sp::MetricsSink metrics;
            const auto out = std::filesystem::path(cfg.out_dir) / name;
            metrics.setOutputDir(out.string());
            if (name == "no_backtrack") variant.gp_backtrack = false;
            if (name == "filler100") {
                variant.filler_only_iters = 100;
                sp::EPlace e(trial, variant, metrics);
                e.run(sp::GpStage::kFillerOnly);
            }
            sp::BinGrid grid;
            grid.initialize(trial, cfg.target_density, cfg.gp_bin_dim, cfg.density_chunks);
            grid.accumulate(trial);
            auto sum = [](const auto& v) { return std::accumulate(v.begin(), v.end(), 0.0); };
            metrics.setSummary("node_charge", sum(grid.nodeDensity()));
            metrics.setSummary("filler_charge", sum(grid.fillerDensity()));
            metrics.setSummary("terminal_charge", sum(grid.terminalDensity()));
            metrics.setSummary("base_charge", sum(grid.baseDensity()));
            metrics.setSummary("sampled_charge", grid.totalCharge(trial));
            metrics.setSummary("grid_dim", grid.dim());
            metrics.setSummary("density_capacity", cfg.target_density *
                static_cast<double>(grid.binArea()) * grid.dim() * grid.dim());
            sp::GpResult r;
            { sp::EPlace e(trial, variant, metrics);
              r = e.run(sp::GpStage::kCGP, {name == "fixed_gamma", name == "fixed_lambda"}); }
            int moved = 0;
            for (int i = 0; i < trial.numMovable; ++i)
                if (trial.isMacro(i) && (trial.node_x[i] != initial.node_x[i] ||
                                        trial.node_y[i] != initial.node_y[i])) ++moved;
            metrics.setSummary("cgp_macros_moved", moved);
            metrics.setSummary("overflow", r.overflow);
            metrics.setSummary("hpwl", r.hpwl);
            metrics.setSummary("iterations", r.iterations);
            metrics.writeSummary((out / "summary.json").string());
            sp::BookshelfWriter::writePl((out / "cgp.pl").string(), trial);
            std::printf("ABLATION %s tau=%.9g hpwl=%.12g iterations=%d evals=%ld moved=%d\n",
                        name.c_str(), r.overflow, r.hpwl, r.iterations, r.evaluations, moved);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cgp-ablation: %s\n", e.what());
        return 2;
    }
}
