#pragma once

#include <string>
#include <vector>

namespace sp {
class PlaceDB;
class BinGrid;
struct Config;

// The supplied fields belong to one actual evaluation point, before schedule
// updates. Probes run on deep copies; this observer never writes to the live DB.
void writeGpDiagnostic(const std::string& root, const std::string& stage,
    const std::string& point, int iter, float lambda, float gamma, float step,
    const PlaceDB& db, const Config& cfg, const BinGrid& grid,
    const std::vector<int>& optIdx, const std::vector<float>& previous,
    const std::vector<float>& wlGradient, const std::vector<float>& force);

void diagnoseDelivered(const std::string& root, const std::string& stage,
    int iter, float lambda, float gamma, const PlaceDB& db, const Config& cfg,
    int gridDim, const std::vector<int>& optIdx, const std::vector<float>& previous);
}  // namespace sp
