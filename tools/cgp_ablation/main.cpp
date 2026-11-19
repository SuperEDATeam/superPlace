// Identical in-memory cGP initial state; only the named variable differs.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <vector>

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
        std::vector<char*> args{argv[0]};
        std::string requested, requestedStage = "cgp";
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--variants" || arg == "--diagnostic-stage") {
                if (i + 1 == argc) throw std::runtime_error("missing diagnostic argument");
                (arg == "--variants" ? requested : requestedStage) = argv[++i];
            } else args.push_back(argv[i]);
        }
        if (requestedStage != "mgp" && requestedStage != "cgp")
            throw std::runtime_error("--diagnostic-stage must be mgp or cgp");
        const auto stage = requestedStage == "mgp" ? sp::GpStage::kMGP : sp::GpStage::kCGP;
        std::vector<std::string> variants;
        if (requested.empty()) requested = "baseline,no_backtrack,fixed_gamma,fixed_lambda" +
            std::string(stage == sp::GpStage::kCGP ? ",filler100" : "");
        std::stringstream names(requested); std::string item;
        while (std::getline(names, item, ',')) {
            if (item != "baseline" && item != "no_backtrack" && item != "fixed_gamma" &&
                item != "fixed_lambda" && item != "filler100" && item != "no_momentum" &&
                item != "half_step" && item != "adaptive_restart" && item != "gradient_restart")
                throw std::runtime_error("unknown ablation: " + item);
            if (stage == sp::GpStage::kMGP && item == "filler100") throw std::runtime_error("filler100 requires cgp");
            variants.push_back(item);
        }
        sp::Config cfg = sp::Config::fromArgs(static_cast<int>(args.size()), args.data());
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
        sp::PlaceDB beforeFiller;
        if (stage == sp::GpStage::kCGP) {
            { sp::EPlace e(db, cfg, sink); e.run(sp::GpStage::kMGP); }
            sp::BookshelfWriter::writePl(cfg.out_dir + "/preparation/input.mgp.pl", db);
            const auto mlg = sp::legalizeMacros(db, cfg, sink);
            if (mlg.overlapAfter > 0.0) throw std::runtime_error("mLG failed");
            sp::scatterFillers(db, cfg, 1);
            beforeFiller = db;
            { sp::EPlace e(db, cfg, sink); e.run(sp::GpStage::kFillerOnly); }
        }
        sink.writeSummary(cfg.out_dir + "/preparation/summary.json");
        const sp::PlaceDB initial = std::move(db);

        for (const std::string& name : variants) {
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
            grid.initialize(trial, cfg.target_density, cfg.gp_bin_dim, cfg.density_chunks,
                            false, cfg.fftw_threads);
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
            sp::GpRunOptions options;
            options.freezeGamma = name == "fixed_gamma";
            options.freezeLambda = name == "fixed_lambda";
            options.noMomentum = name == "no_momentum";
            options.stepScale = name == "half_step" ? .5f : 1.f;
            options.adaptiveRestart = name == "adaptive_restart" || name == "gradient_restart";
            options.gradientRestart = name == "gradient_restart";
            { sp::EPlace e(trial, variant, metrics);
              r = e.run(stage, options); }
            int moved = 0;
            for (int i = 0; i < trial.numMovable; ++i)
                if (trial.isFixed(i) && (trial.node_x[i] != initial.node_x[i] ||
                                        trial.node_y[i] != initial.node_y[i])) ++moved;
            metrics.setSummary("cgp_macros_moved", moved);
            metrics.setSummary("diagnostic_stage", requestedStage);
            metrics.setSummary("variant", name);
            metrics.setSummary("overflow", r.overflow);
            metrics.setSummary("hpwl", r.hpwl);
            metrics.setSummary("iterations", r.iterations);
            metrics.writeSummary((out / "summary.json").string());
            sp::BookshelfWriter::writePl((out / (requestedStage + ".pl")).string(), trial);
            std::printf("ABLATION %s tau=%.9g hpwl=%.12g iterations=%d evals=%ld moved=%d\n",
                        name.c_str(), r.overflow, r.hpwl, r.iterations, r.evaluations, moved);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cgp-ablation: %s\n", e.what());
        return 2;
    }
}
