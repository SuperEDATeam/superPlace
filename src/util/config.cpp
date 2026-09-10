#include "util/config.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "util/logger.h"

namespace sp {
namespace {

std::string basenameNoExt(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

std::string Config::usage() {
    return
        "superplace-cli — VLSI global placement engine\n"
        "\n"
        "Usage:\n"
        "  superplace-cli --aux <design.aux> [options]\n"
        "\n"
        "Options:\n"
        "  --aux <path>        BookShelf .aux file (required)\n"
        "  --config <path>     JSON config file\n"
        "  --out <dir>         Output directory (default: results)\n"
        "  --stage <list>      Comma-separated stages to run (default: parse)\n"
        "  --seed <n>          Random seed (default: 1002)\n"
        "  --threads <n>       OpenMP thread count (0 = auto)\n"
        "  --no-plot           Disable image output\n"
        "  --full-plot         Emit an image every iteration\n"
        "  --verbose           Verbose logging\n"
        "  -h, --help          Show this help\n";
}

void Config::loadJson(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open config file: " + path);

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
    }

    auto get = [&](const char* key, auto& dst) {
        if (j.contains(key)) dst = j.at(key).get<std::decay_t<decltype(dst)>>();
    };

    get("out_dir", out_dir);
    get("random_seed", random_seed);
    get("num_threads", num_threads);
    get("verbose", verbose);
    get("plot", plot);
    get("full_plot", full_plot);
    get("plot_interval", plot_interval);
    get("plot_min_side", plot_min_side);
    get("plot_margin", plot_margin);
    get("plot_fillers", plot_fillers);
    get("target_density", target_density);
    get("target_overflow", target_overflow);
    get("gp_max_iter", gp_max_iter);
    get("qp_max_iter", qp_max_iter);
    get("qp_min_distance", qp_min_distance);
    get("ignore_net_degree", ignore_net_degree);
    if (j.contains("stages")) stages = j.at("stages").get<std::vector<std::string>>();
    if (j.contains("aux_path") && aux_path.empty()) aux_path = j.at("aux_path").get<std::string>();
}

Config Config::fromArgs(int argc, char** argv) {
    Config cfg;
    std::unordered_set<std::string> cliSet;   // CLI 显式指定过的键，JSON 不得覆盖

    auto need = [&](int i, const char* what) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + what);
        return argv[i + 1];
    };

    std::string pendingConfig;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            throw std::invalid_argument("help");
        } else if (a == "--aux") {
            cfg.aux_path = need(i, "--aux");    ++i; cliSet.insert("aux_path");
        } else if (a == "--config") {
            pendingConfig = need(i, "--config"); ++i;
        } else if (a == "--out") {
            cfg.out_dir = need(i, "--out");     ++i; cliSet.insert("out_dir");
        } else if (a == "--stage") {
            cfg.stages = splitComma(need(i, "--stage")); ++i; cliSet.insert("stages");
        } else if (a == "--seed") {
            cfg.random_seed = std::stoull(need(i, "--seed")); ++i; cliSet.insert("random_seed");
        } else if (a == "--threads") {
            cfg.num_threads = std::stoi(need(i, "--threads")); ++i; cliSet.insert("num_threads");
        } else if (a == "--no-plot") {
            cfg.plot = false;  cliSet.insert("plot");
        } else if (a == "--full-plot") {
            cfg.full_plot = true; cliSet.insert("full_plot");
        } else if (a == "--verbose") {
            cfg.verbose = true;   cliSet.insert("verbose");
        } else {
            throw std::runtime_error("unknown argument: " + a);
        }
    }

    if (!pendingConfig.empty()) {
        cfg.config_path = pendingConfig;
        // JSON 先读进一个副本，再把 CLI 显式指定过的项还原回去
        Config fromFile = cfg;
        fromFile.loadJson(pendingConfig);
        if (cliSet.count("out_dir"))      fromFile.out_dir      = cfg.out_dir;
        if (cliSet.count("stages"))       fromFile.stages       = cfg.stages;
        if (cliSet.count("random_seed"))  fromFile.random_seed  = cfg.random_seed;
        if (cliSet.count("num_threads"))  fromFile.num_threads  = cfg.num_threads;
        if (cliSet.count("plot"))         fromFile.plot         = cfg.plot;
        if (cliSet.count("full_plot"))    fromFile.full_plot    = cfg.full_plot;
        if (cliSet.count("verbose"))      fromFile.verbose      = cfg.verbose;
        if (cliSet.count("aux_path"))     fromFile.aux_path     = cfg.aux_path;
        cfg = fromFile;
    }

    if (cfg.aux_path.empty()) throw std::runtime_error("--aux is required");
    cfg.benchmark_name = basenameNoExt(cfg.aux_path);
    return cfg;
}

}  // namespace sp
