#include "gp/diagnostics.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>

#include "db/place_db.h"
#include "gp/bin_grid.h"
#include "gp/wa_wirelength.h"
#include "nlohmann/json.hpp"
#include "util/config.h"

namespace sp {
namespace {
std::ofstream output(const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write GP diagnostic: " + path.string());
    f << std::setprecision(17);
    return f;
}
double excess(const BinGrid& g, size_t b, bool staticOnly = false) {
    const double q = double(g.terminalDensity()[b]) + g.baseDensity()[b] +
                     (staticOnly ? 0.0 : double(g.nodeDensity()[b]));
    return std::max(0.0, q - double(g.targetDensity()) * g.binArea());
}
double energy(const BinGrid& g) {
    double e = 0;
    for (size_t b = 0; b < g.nodeDensity().size(); ++b)
        e += .5 * (double(g.nodeDensity()[b]) + g.fillerDensity()[b] +
                   g.terminalDensity()[b] + g.baseDensity()[b]) * g.potential()[b];
    return e;
}
}  // namespace

void writeGpDiagnostic(const std::string& root, const std::string& stage,
    const std::string& point, int iter, float lambda, float gamma, float step,
    const PlaceDB& db, const Config& cfg, const BinGrid& grid,
    const std::vector<int>& optIdx, const std::vector<float>& previous,
    const std::vector<float>& wlGradient, const std::vector<float>& force) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(root) / "diagnostics" / stage / point;
    fs::create_directories(dir);
    const int dim = grid.dim(), total = db.totalNodes(), n = static_cast<int>(optIdx.size());
    const size_t bins = grid.nodeDensity().size();
    std::vector<int> labels(bins, -1), queue;
    nlohmann::json components = nlohmann::json::array();
    double overflow = 0, staticOverflow = 0;
    for (size_t b = 0; b < bins; ++b) {
        overflow += excess(grid, b);
        staticOverflow += excess(grid, b, true);
        if (labels[b] >= 0 || excess(grid, b) <= 0) continue;
        const int id = static_cast<int>(components.size());
        queue.clear(); queue.push_back(static_cast<int>(b)); labels[b] = id;
        double mass = 0;
        int minX = dim, minY = dim, maxX = 0, maxY = 0;
        for (size_t h = 0; h < queue.size(); ++h) {
            const int here = queue[h], x = here / dim, y = here % dim;
            mass += excess(grid, here);
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            const int next[4] = {x > 0 ? here - dim : -1, x + 1 < dim ? here + dim : -1,
                                 y > 0 ? here - 1 : -1, y + 1 < dim ? here + 1 : -1};
            for (int q : next) if (q >= 0 && labels[q] < 0 && excess(grid, q) > 0) {
                labels[q] = id; queue.push_back(q);
            }
        }
        components.push_back({{"id", id}, {"bins", queue.size()}, {"overflow_area", mass},
            {"bin_bounds", {minX, minY, maxX, maxY}}});
    }
    {
        auto f = output(dir / "bins.csv");
        f << "bx,by,lx,ly,hx,hy,capacity,node_charge,filler_charge,terminal_charge,base_charge,overflow_area,static_overflow_area,potential,field_x,field_y,component\n";
        for (int x = 0; x < dim; ++x) for (int y = 0; y < dim; ++y) {
            const size_t b = size_t(x) * dim + y;
            const float lx = grid.originX() + x * grid.stepX(), ly = grid.originY() + y * grid.stepY();
            f << x << ',' << y << ',' << lx << ',' << ly << ',' << lx + grid.stepX() << ','
              << ly + grid.stepY() << ',' << double(grid.targetDensity()) * grid.binArea() << ','
              << grid.nodeDensity()[b] << ',' << grid.fillerDensity()[b] << ','
              << grid.terminalDensity()[b] << ',' << grid.baseDensity()[b] << ','
              << excess(grid, b) << ',' << excess(grid, b, true) << ',' << grid.potential()[b]
              << ',' << grid.fieldX()[b] << ',' << grid.fieldY()[b] << ',' << labels[b] << '\n';
        }
    }
    std::vector<float> densityDirection(2 * n), combinedDirection(2 * n);
    {
        auto f = output(dir / "nodes.csv");
        f << "id,name,filler,macro,component,x,y,width,height,wl_x,wl_y,force_x,force_y,lambda_force_x,lambda_force_y,preconditioner,gradient_x,gradient_y,actual_dx,actual_dy,boundary\n";
        for (int k = 0; k < n; ++k) {
            const int i = optIdx[k];
            const auto footprint = grid.diagnosticFootprint(db, i);
            std::map<int, double> weights;
            const Rect rect{footprint.lx, footprint.ly, footprint.hx, footprint.hy};
            for (int x = footprint.bx0; x <= footprint.bx1; ++x)
                for (int y = footprint.by0; y <= footprint.by1; ++y) {
                    const int label = labels[size_t(x) * dim + y];
                    if (label < 0) continue;
                    const float lx = grid.originX() + x * grid.stepX();
                    const float ly = grid.originY() + y * grid.stepY();
                    weights[label] += overlapArea(rect, {lx, ly, lx + grid.stepX(), ly + grid.stepY()});
                }
            int component = -1; double most = 0;
            for (auto [id, weight] : weights) if (weight > most) { component = id; most = weight; }
            const float wx = i < db.numMovable ? wlGradient[i] : 0.f;
            const float wy = i < db.numMovable ? wlGradient[db.numMovable + i] : 0.f;
            const float fx = force[i], fy = force[total + i];
            const float pre = static_cast<float>(1.0 / std::max(1.0,
                double(db.nodeDegree(i)) + double(lambda) * db.area(i)));
            const float gx = pre * (wx - lambda * fx), gy = pre * (wy - lambda * fy);
            densityDirection[k] = pre * lambda * fx; densityDirection[n + k] = pre * lambda * fy;
            combinedDirection[k] = -gx; combinedDirection[n + k] = -gy;
            const double dx = previous.size() == size_t(2 * n) ? double(db.node_x[i]) - previous[k] : 0;
            const double dy = previous.size() == size_t(2 * n) ? double(db.node_y[i]) - previous[n + k] : 0;
            const bool boundary = db.llx(i) <= db.coreRegion.lx + .01f || db.lly(i) <= db.coreRegion.ly + .01f ||
                db.urx(i) >= db.coreRegion.hx - .01f || db.ury(i) >= db.coreRegion.hy - .01f;
            // Include every optimized node, including non-hotspot escape routes.
            f << i << ',' << std::quoted(db.node_name[i], '"', '"') << ',' << db.isFiller(i) << ','
              << db.isMacro(i) << ',' << component << ',' << db.node_x[i] << ',' << db.node_y[i]
              << ',' << db.node_w[i] << ',' << db.node_h[i] << ',' << wx << ',' << wy << ','
              << fx << ',' << fy << ',' << lambda * fx << ',' << lambda * fy << ',' << pre << ','
              << gx << ',' << gy << ',' << dx << ',' << dy << ',' << boundary << '\n';
        }
    }
    {
        auto f = output(dir / "geometry.csv");
        f << "kind,id,lx,ly,hx,hy\n";
        for (int i = 0; i < db.numNodes; ++i) if (db.isFixed(i) && !db.isNI(i))
            f << "fixed," << i << ',' << db.llx(i) << ',' << db.lly(i) << ',' << db.urx(i) << ',' << db.ury(i) << '\n';
        for (size_t i = 0; i < db.rows.size(); ++i) {
            const auto rect = db.rows[i].rect();
            f << "row," << i << ',' << rect.lx << ',' << rect.ly << ',' << rect.hx << ',' << rect.hy << '\n';
        }
    }
    nlohmann::json probes = nlohmann::json::array();
    // These are local geometric probes, not optimizer continuation. The live
    // momentum, schedules, fillers and lastEval are never touched.
    for (int direction = 0; direction < 2; ++direction) {
        const auto& v = direction ? combinedDirection : densityDirection;
        double norm = 0;
        for (int k = 0; k < n; ++k) norm = std::max(norm, std::hypot(double(v[k]), double(v[n + k])));
        for (double fraction : {.01, .1, 1.0}) {
            PlaceDB trial = db;
            const double scale = norm > 0 ? fraction * std::min(grid.stepX(), grid.stepY()) / norm : 0;
            for (int k = 0; k < n; ++k) {
                const int i = optIdx[k];
                const float xlo = db.coreRegion.lx + .5f * db.node_w[i] + 1e-3f;
                const float ylo = db.coreRegion.ly + .5f * db.node_h[i] + 1e-3f;
                trial.node_x[i] = std::clamp(db.node_x[i] + float(scale * v[k]), xlo,
                    std::max(xlo, db.coreRegion.hx - .5f * db.node_w[i] - 1e-3f));
                trial.node_y[i] = std::clamp(db.node_y[i] + float(scale * v[n + k]), ylo,
                    std::max(ylo, db.coreRegion.hy - .5f * db.node_h[i] - 1e-3f));
            }
            BinGrid probe;
            probe.initialize(trial, cfg.target_density, dim, cfg.density_chunks, false, cfg.fftw_threads);
            probe.accumulate(trial); probe.solveField();
            probes.push_back({{"direction", direction ? "combined" : "density"},
                {"fraction_of_bin", fraction}, {"overflow", probe.overflow()}, {"energy", energy(probe)}});
        }
    }
    const double denom = grid.overflowDenominator();
    auto f = output(dir / "metadata.json");
    f << nlohmann::json({{"stage", stage}, {"point", point}, {"iter", iter},
        {"movement_reference", point == "delivered" ? "last evaluation (includes rollback jump)" : "previous evaluation"},
        {"eval_lambda", lambda}, {"eval_gamma", gamma}, {"step_size", step},
        {"grid_dim", dim}, {"origin", {grid.originX(), grid.originY()}},
        {"bin_step", {grid.stepX(), grid.stepY()}}, {"overflow_denominator", denom},
        {"overflow_area", overflow}, {"overflow", grid.overflow()},
        {"static_overflow_area", staticOverflow}, {"static_floor_tau", denom > 0 ? staticOverflow / denom : 0},
        {"energy", energy(grid)}, {"components", components}, {"probes", probes}}).dump(2) << '\n';
}

void diagnoseDelivered(const std::string& root, const std::string& stage,
    int iter, float lambda, float gamma, const PlaceDB& db, const Config& cfg,
    int gridDim, const std::vector<int>& optIdx, const std::vector<float>& previous) {
    PlaceDB clone = db;
    BinGrid grid;
    grid.initialize(clone, cfg.target_density, gridDim, cfg.density_chunks,
                    cfg.poisson_backend == "cuda", cfg.fftw_threads);
    grid.accumulate(clone); grid.solveField();
    WaWirelength wl; wl.prepare(clone);
    std::vector<float> grad(2 * clone.numMovable), force(2 * clone.totalNodes());
    double value = 0;
    wl.compute(clone, gamma, cfg.ignore_net_degree, &value, grad.data());
    grid.gatherForce(clone, force.data());
    writeGpDiagnostic(root, stage, "delivered", iter, lambda, gamma, 0.f,
                      clone, cfg, grid, optIdx, previous, grad, force);
}
}  // namespace sp
