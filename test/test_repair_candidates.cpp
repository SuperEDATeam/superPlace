#include <limits>

#include "lg/repair_candidates.h"
#include "test_util.h"

namespace {
double nearest(float x, float y, double radius) {
    const sp::Rect core{0, 0, 2000, 2000};
    // The only legal vertical channel is 201 wide, at a non-grid coordinate.
    const std::vector<sp::Rect> obstacles{{0, 0, 941, 2000}, {1142, 0, 2000, 2000}};
    auto points = sp::contactCandidates(x, y, x, y, 200, 80, core, obstacles, radius);
    double best = std::numeric_limits<double>::infinity();
    for (auto [px, py] : points) {
        const sp::Rect rect{px - 100, py - 40, px + 100, py + 40};
        bool legal = true;
        for (const auto& b : obstacles) if (sp::overlapArea(rect, b) > 0) legal = false;
        if (legal) best = std::min(best, std::hypot(double(px - x), double(py - y)));
    }
    return best;
}
}

int main() {
    CHECK_NEAR(nearest(1000, 1000, 100), 41.0, 0.0);
    CHECK_TRUE(!std::isfinite(nearest(200, 1000, 100)));
    CHECK_NEAR(nearest(200, 1000, 2000), 841.0, 0.0);
    return sptest::summary("test_repair_candidates");
}
