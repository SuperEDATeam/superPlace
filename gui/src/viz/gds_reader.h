#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace viz {

struct GdsInfo {
    std::string library, structure;
    double userUnitMeters = 0.0, databaseUnitMeters = 0.0;
    std::map<int, std::size_t> layerCounts;
    double lx = 0.0, ly = 0.0, hx = 0.0, hy = 0.0;
    std::size_t fileBytes = 0, records = 0;
    bool hasBounds = false;
};

// A strict reader for the single-structure BOUNDARY subset exported by this viewer.
bool readGdsFile(const std::string& path, GdsInfo& info, std::string& error);

}  // namespace viz
