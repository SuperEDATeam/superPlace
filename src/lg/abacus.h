#pragma once

#include <vector>

#include "db/place_db.h"

namespace sp {

struct AbacusSegment {
    int row = 0;
    float lx = 0.f;
    float hx = 0.f;
};

struct AbacusOptions {
    // Full-state trials are an intentionally slow oracle for small tests.
    bool referenceTrials = false;
};

struct AbacusResult {
    bool preconditionOk = true;
    int cells = 0;
    int failed = 0;
    int rows = 0;
    int segments = 0;
    int roundedWidths = 0;
    int selfcheckOverlaps = 0;
    int selfcheckOffSite = 0;
    double dispMean = 0.0;
    double dispMax = 0.0;
};

std::vector<AbacusSegment> buildAbacusSegments(const PlaceDB& db);
AbacusResult legalizeCells(PlaceDB& db, const AbacusOptions& options = {});

}  // namespace sp
