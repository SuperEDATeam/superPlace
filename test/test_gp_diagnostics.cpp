#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "db/place_db.h"
#include "gp/eplace.h"
#include "nlohmann/json.hpp"
#include "test_util.h"
#include "util/config.h"
#include "util/invariant.h"
#include "util/metrics.h"

sp::PlaceDB fixture() {
    sp::PlaceDB db;
    for (int i = 0; i < 4; ++i) {
        db.addNode("c" + std::to_string(i), 4, 4, 0);
        db.node_x.back() = 4; db.node_y.back() = 4;
    }
    db.numNodes = db.numMovable = 4;
    db.node2pin_start.assign(5, 0); db.net2pin_start.assign(1, 0);
    db.rows.push_back({0, 16, 0, 1, 16}); db.computeRegions();
    return db;
}

int main() {
    char name[] = "/tmp/eplace-diagnostic-XXXXXX";
    const std::filesystem::path dir = ::mkdtemp(name);
    auto plain = fixture(), observed = plain;
    sp::Config cfg; cfg.gp_bin_dim = 8; cfg.gp_max_iter = 2;
    cfg.gp_stagnation_window = 0; cfg.target_overflow = 1e-8f;
    sp::MetricsSink reference, diagnostic;
    diagnostic.setOutputDir(dir.string());
    const auto before = sp::EPlace(plain, cfg, reference).run(sp::GpStage::kMGP);
    cfg.gp_diagnostics = true;
    const auto after = sp::EPlace(observed, cfg, diagnostic).run(sp::GpStage::kMGP);
    CHECK_TRUE(plain.node_x == observed.node_x && plain.node_y == observed.node_y);
    CHECK_TRUE(plain.node_flags == observed.node_flags);
    CHECK_TRUE(before.hpwl == after.hpwl && before.overflow == after.overflow);
    for (const std::string point : {"first_eval", "last_eval", "delivered"}) {
        const auto path = dir / "diagnostics/mGP" / point;
        nlohmann::json meta; std::ifstream(path / "metadata.json") >> meta;
        CHECK_TRUE(meta["grid_dim"] == 8);
        CHECK_NEAR(meta["static_floor_tau"].get<double>(), 0, 0);
        CHECK_TRUE(meta["probes"].size() == 6);
        double overflow = 0;
        std::ifstream rows(path / "bins.csv"); std::string line;
        std::getline(rows, line);
        while (std::getline(rows, line)) {
            std::stringstream ss(line); std::string token;
            for (int col = 0; std::getline(ss, token, ','); ++col)
                if (col == 11) overflow += std::stod(token);
        }
        CHECK_NEAR(overflow / meta["overflow_denominator"].get<double>(),
                   meta["overflow"].get<double>(), 1e-6);
    }
    bool rejected = false;
    auto invalid = fixture(); invalid.node_x[0] = std::numeric_limits<float>::quiet_NaN();
    try { sp::EPlace engine(invalid, cfg, reference); } catch (const sp::InvariantError&) { rejected = true; }
    CHECK_TRUE(rejected);
    invalid = fixture(); invalid.node_flags[0] |= sp::F_FIXED; invalid.node_x[0] = 30;
    rejected = false;
    try { sp::checkGeometry(invalid, "frozen", true); } catch (const sp::InvariantError&) { rejected = true; }
    CHECK_TRUE(rejected);
    rejected = false;
    try { reference.setSummary("bad", std::numeric_limits<double>::infinity()); }
    catch (const sp::InvariantError&) { rejected = true; }
    CHECK_TRUE(rejected);
    auto frozen = fixture();
    frozen.node_flags[0] |= sp::F_FIXED | sp::F_MACRO;
    const float frozenX = frozen.node_x[0], frozenY = frozen.node_y[0];
    sp::MetricsSink frozenSink; frozenSink.setOutputDir((dir / "frozen").string());
    sp::EPlace(frozen, cfg, frozenSink).run(sp::GpStage::kCGP);
    CHECK_TRUE(frozen.node_x[0] == frozenX && frozen.node_y[0] == frozenY);
    CHECK_TRUE(frozen.isFixed(0));
    const auto cfgPath = dir / "config.json";
    std::ofstream(cfgPath) << "{\"gp_diagnostics\":false}";
    std::vector<std::string> args{"cli", "--aux", "fixture.aux", "--config", cfgPath.string(), "--gp-diagnostics"};
    std::vector<char*> raw;
    for (auto& arg : args) raw.push_back(arg.data());
    CHECK_TRUE(sp::Config::fromArgs(static_cast<int>(raw.size()), raw.data()).gp_diagnostics);
    std::filesystem::remove_all(dir);
    return sptest::summary("test_gp_diagnostics");
}
