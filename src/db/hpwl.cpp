#include "db/hpwl.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "db/place_db.h"

namespace sp {

double computeHPWL(const PlaceDB& db) {
    const int numNets = db.numNets;
    const int* __restrict start = db.net2pin_start.data();
    const int* __restrict flat  = db.flat_net2pin.data();

    double total = 0.0;
#pragma omp parallel for schedule(static) reduction(+ : total)
    for (int k = 0; k < numNets; ++k) {
        const int b = start[k], e = start[k + 1];
        if (e - b < 2) continue;

        float lox = std::numeric_limits<float>::max(), hix = -std::numeric_limits<float>::max();
        float loy = std::numeric_limits<float>::max(), hiy = -std::numeric_limits<float>::max();
        for (int t = b; t < e; ++t) {
            const int p = flat[t];
            const float px = db.pinX(p);
            const float py = db.pinY(p);
            lox = std::min(lox, px);
            hix = std::max(hix, px);
            loy = std::min(loy, py);
            hiy = std::max(hiy, py);
        }
        total += static_cast<double>(hix - lox) + static_cast<double>(hiy - loy);
    }
    return total;
}

void computeNetBoundBoxes(const PlaceDB& db, std::vector<float>& minX, std::vector<float>& maxX,
                          std::vector<float>& minY, std::vector<float>& maxY) {
    const int numNets = db.numNets;
    minX.assign(static_cast<size_t>(numNets), 0.f);
    maxX.assign(static_cast<size_t>(numNets), 0.f);
    minY.assign(static_cast<size_t>(numNets), 0.f);
    maxY.assign(static_cast<size_t>(numNets), 0.f);

#pragma omp parallel for schedule(static)
    for (int k = 0; k < numNets; ++k) {
        const int b = db.net2pin_start[k], e = db.net2pin_start[k + 1];
        if (b == e) continue;
        float lox = std::numeric_limits<float>::max(), hix = -std::numeric_limits<float>::max();
        float loy = std::numeric_limits<float>::max(), hiy = -std::numeric_limits<float>::max();
        for (int t = b; t < e; ++t) {
            const int p = db.flat_net2pin[t];
            const float px = db.pinX(p), py = db.pinY(p);
            lox = std::min(lox, px);
            hix = std::max(hix, px);
            loy = std::min(loy, py);
            hiy = std::max(hiy, py);
        }
        minX[static_cast<size_t>(k)] = lox;
        maxX[static_cast<size_t>(k)] = hix;
        minY[static_cast<size_t>(k)] = loy;
        maxY[static_cast<size_t>(k)] = hiy;
    }
}

void computeNetBoundPins(const PlaceDB& db, std::vector<int>& minPinX, std::vector<int>& maxPinX,
                         std::vector<int>& minPinY, std::vector<int>& maxPinY) {
    const int numNets = db.numNets;
    minPinX.assign(static_cast<size_t>(numNets), -1);
    maxPinX.assign(static_cast<size_t>(numNets), -1);
    minPinY.assign(static_cast<size_t>(numNets), -1);
    maxPinY.assign(static_cast<size_t>(numNets), -1);

#pragma omp parallel for schedule(static)
    for (int k = 0; k < numNets; ++k) {
        const int b = db.net2pin_start[k], e = db.net2pin_start[k + 1];
        if (b == e) continue;

        int pMinX = db.flat_net2pin[b], pMaxX = pMinX;
        int pMinY = pMinX, pMaxY = pMinX;
        float loX = db.pinX(pMinX), hiX = loX;
        float loY = db.pinY(pMinY), hiY = loY;

        for (int t = b + 1; t < e; ++t) {
            const int p = db.flat_net2pin[t];
            const float px = db.pinX(p), py = db.pinY(p);
            // 严格不等号：并列时保留先出现（下标更小）者，保证可复现
            if (px < loX) { loX = px; pMinX = p; }
            if (px > hiX) { hiX = px; pMaxX = p; }
            if (py < loY) { loY = py; pMinY = p; }
            if (py > hiY) { hiY = py; pMaxY = p; }
        }
        minPinX[static_cast<size_t>(k)] = pMinX;
        maxPinX[static_cast<size_t>(k)] = pMaxX;
        minPinY[static_cast<size_t>(k)] = pMinY;
        maxPinY[static_cast<size_t>(k)] = pMaxY;
    }
}

DensityStats computeDensityStats(const PlaceDB& db, int dim, float targetDensity) {
    DensityStats s;
    if (dim <= 0 || !db.coreRegion.valid()) return s;

    const float stepX = db.coreRegion.width() / static_cast<float>(dim);
    const float stepY = db.coreRegion.height() / static_cast<float>(dim);
    const double binArea = static_cast<double>(stepX) * static_cast<double>(stepY);
    if (binArea <= 0.0) return s;

    std::vector<double> occupancy(static_cast<size_t>(dim) * dim, 0.0);

    // 串行累加：per-bin 的浮点加法顺序固定，保证逐位可复现（铁律 7）。
    // 若将来要并行，必须用 per-thread 局部网格 + 固定顺序规约，禁止原子加。
    for (int i = 0; i < db.numMovable; ++i) {
        const float lx = db.llx(i), hx = db.urx(i);
        const float ly = db.lly(i), hy = db.ury(i);

        int bx0 = static_cast<int>((lx - db.coreRegion.lx) / stepX);
        int bx1 = static_cast<int>((hx - db.coreRegion.lx) / stepX);
        int by0 = static_cast<int>((ly - db.coreRegion.ly) / stepY);
        int by1 = static_cast<int>((hy - db.coreRegion.ly) / stepY);
        bx0 = std::clamp(bx0, 0, dim - 1);
        bx1 = std::clamp(bx1, 0, dim - 1);
        by0 = std::clamp(by0, 0, dim - 1);
        by1 = std::clamp(by1, 0, dim - 1);

        for (int bx = bx0; bx <= bx1; ++bx) {
            const float blx = db.coreRegion.lx + bx * stepX;
            for (int by = by0; by <= by1; ++by) {
                const float bly = db.coreRegion.ly + by * stepY;
                const Rect bin{blx, bly, blx + stepX, bly + stepY};
                occupancy[static_cast<size_t>(bx) * dim + by] +=
                    static_cast<double>(overlapArea(bin, db.box(i)));
            }
        }
    }

    double sum = 0.0, sumSq = 0.0;
    for (double occ : occupancy) {
        const double d = occ / binArea;
        s.maxDensity = std::max(s.maxDensity, static_cast<float>(d));
        sum += d;
        sumSq += d * d;
        const double over = occ - binArea * static_cast<double>(targetDensity);
        if (over > 0.0) s.overflowArea += over;
    }
    const double n = static_cast<double>(occupancy.size());
    const double mean = sum / n;
    s.meanDensity = static_cast<float>(mean);
    s.stdDensity = static_cast<float>(std::sqrt(std::max(0.0, sumSq / n - mean * mean)));
    return s;
}

}  // namespace sp
