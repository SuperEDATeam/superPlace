#include "lg/abacus.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace sp {
namespace {

bool movableCell(const PlaceDB& db, int i) {
    return i < db.numMovable && !db.isMacro(i) && !db.isFixed(i) &&
           !db.isFiller(i) && !db.isNI(i) && db.node_w[i] > 0.f && db.node_h[i] > 0.f;
}

double siteWidth(float width, float step) {
    const double sites = static_cast<double>(width) / step;
    const double nearest = std::round(sites);
    return (std::abs(sites - nearest) < 1e-5 ? nearest : std::ceil(sites)) * step;
}

struct Cluster {
    int first = 0;
    int last = 0;
    double e = 0.0;
    double q = 0.0;
    double w = 0.0;
    double x = 0.0;
};

struct SegmentState {
    AbacusSegment segment;
    double used = 0.0;
    std::vector<int> cells;
    std::vector<Cluster> clusters;
};

void collapsePosition(Cluster& c, const AbacusSegment& s) {
    c.x = std::clamp(c.q / c.e, static_cast<double>(s.lx), static_cast<double>(s.hx) - c.w);
}

void addCell(Cluster& c, int slot, double desired, double width) {
    // Abacus (ISPD 2008), Algorithm 2 / Eq. (4): offsets are relative to cluster start.
    c.last = slot;
    c.e += 1.0;
    c.q += desired - c.w;
    c.w += width;
}

Cluster mergeClusters(const Cluster& left, const Cluster& right) {
    Cluster merged = left;
    merged.last = right.last;
    merged.q += right.q - right.e * merged.w;
    merged.e += right.e;
    merged.w += right.w;
    return merged;
}

// The only changed state is the new tail and the predecessor suffix it absorbs.
// Trials never mutate a real segment; copying the full row here is quadratic.
Cluster trialTail(const SegmentState& state, double desired, double width, int& prefix) {
    prefix = static_cast<int>(state.clusters.size());
    Cluster tail;
    const int slot = static_cast<int>(state.cells.size());
    if (prefix && state.clusters.back().x + state.clusters.back().w > desired) {
        tail = state.clusters.back();
        --prefix;
    } else {
        tail.first = slot;
    }
    addCell(tail, slot, desired, width);
    collapsePosition(tail, state.segment);
    while (prefix && state.clusters[prefix - 1].x + state.clusters[prefix - 1].w > tail.x) {
        tail = mergeClusters(state.clusters[--prefix], tail);
        collapsePosition(tail, state.segment);
    }
    return tail;
}

// A separate full-state mutation path is retained as a test oracle.
void appendReference(std::vector<Cluster>& clusters, int slot, double desired, double width,
                     const AbacusSegment& segment) {
    if (clusters.empty() || clusters.back().x + clusters.back().w <= desired) {
        Cluster fresh;
        fresh.first = slot;
        clusters.push_back(fresh);
    }
    addCell(clusters.back(), slot, desired, width);
    collapsePosition(clusters.back(), segment);
    while (clusters.size() > 1) {
        const int last = static_cast<int>(clusters.size()) - 1;
        if (clusters[last - 1].x + clusters[last - 1].w <= clusters[last].x) break;
        clusters[last - 1] = mergeClusters(clusters[last - 1], clusters[last]);
        clusters.pop_back();
        collapsePosition(clusters.back(), segment);
    }
}

Cluster referenceTrial(const SegmentState& state, double desired, double width) {
    std::vector<Cluster> clusters = state.clusters;
    appendReference(clusters, static_cast<int>(state.cells.size()), desired, width, state.segment);
    return clusters.back();
}

}  // namespace

std::vector<AbacusSegment> buildAbacusSegments(const PlaceDB& db) {
    float minimumWidth = std::numeric_limits<float>::infinity();
    for (int i = 0; i < db.numMovable; ++i)
        if (movableCell(db, i)) minimumWidth = std::min(minimumWidth, db.node_w[i]);
    if (!std::isfinite(minimumWidth)) minimumWidth = 0.f;

    std::vector<int> obstacles;
    for (int i = 0; i < db.numNodes; ++i)
        if (db.isFixed(i) && !db.isNI(i) && !db.isFiller(i) &&
            db.node_w[i] > 0.f && db.node_h[i] > 0.f) obstacles.push_back(i);

    std::vector<AbacusSegment> result;
    for (int index = 0; index < static_cast<int>(db.rows.size()); ++index) {
        const auto& row = db.rows[index];
        if (!std::isfinite(row.lx) || !std::isfinite(row.ly) || !std::isfinite(row.step) ||
            !std::isfinite(row.height) || row.step <= 0.f || row.height <= 0.f || row.numSites <= 0)
            throw std::invalid_argument("invalid site row for Abacus");
        if (row.ly < db.coreRegion.ly || row.hy() > db.coreRegion.hy) continue;
        const double low = std::max(row.lx, db.coreRegion.lx);
        const double high = std::min(row.hx(), db.coreRegion.hx);
        if (low >= high) continue;
        std::vector<std::pair<double, double>> blocked;
        for (int i : obstacles) {
            // Touching row boundaries is legal: only positive-area intersection blocks sites.
            if (db.lly(i) >= row.hy() || db.ury(i) <= row.ly) continue;
            const double a = std::max(low, static_cast<double>(db.llx(i)));
            const double b = std::min(high, static_cast<double>(db.urx(i)));
            if (a < b) blocked.emplace_back(a, b);
        }
        std::sort(blocked.begin(), blocked.end());
        auto emit = [&](double a, double b) {
            if (a >= b) return;
            const double lx = row.lx + std::ceil((a - row.lx) / row.step) * row.step;
            const double hx = row.lx + std::floor((b - row.lx) / row.step) * row.step;
            if (hx > lx && hx - lx >= minimumWidth)
                result.push_back({index, static_cast<float>(lx), static_cast<float>(hx)});
        };
        double cursor = low;
        for (const auto& interval : blocked) {
            emit(cursor, interval.first);
            cursor = std::max(cursor, interval.second);
        }
        emit(cursor, high);
    }
    return result;
}

AbacusResult legalizeCells(PlaceDB& db, const AbacusOptions& options) {
    AbacusResult result;
    result.rows = static_cast<int>(db.rows.size());
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.isMacro(i) && !db.isFixed(i)) {
            result.preconditionOk = false;
            return result;
        }
    }
    std::vector<int> order;
    for (int i = 0; i < db.numNodes; ++i) {
        if (!std::isfinite(db.node_x[i]) || !std::isfinite(db.node_y[i]) ||
            !std::isfinite(db.node_w[i]) || !std::isfinite(db.node_h[i]))
            throw std::invalid_argument("non-finite node geometry for Abacus");
        if (movableCell(db, i)) order.push_back(i);
    }
    result.cells = static_cast<int>(order.size());
    if (order.empty()) return result;

    // SoA snapshots retain the GP objective even when earlier cells have been legalized.
    std::vector<float> originalX(db.numMovable), originalY(db.numMovable);
    for (int i : order) { originalX[i] = db.llx(i); originalY[i] = db.lly(i); }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return originalX[a] < originalX[b] || (originalX[a] == originalX[b] && a < b);
    });
    const auto geometry = buildAbacusSegments(db);
    result.segments = static_cast<int>(geometry.size());
    std::vector<SegmentState> states(geometry.size());
    std::vector<std::vector<int>> rowSegments(db.rows.size());
    for (int s = 0; s < result.segments; ++s) {
        states[s].segment = geometry[s];
        rowSegments[geometry[s].row].push_back(s);
    }
    std::vector<int> rowOrder(db.rows.size());
    for (int r = 0; r < result.rows; ++r) rowOrder[r] = r;
    std::sort(rowOrder.begin(), rowOrder.end(), [&](int a, int b) {
        return db.rows[a].ly < db.rows[b].ly || (db.rows[a].ly == db.rows[b].ly && a < b);
    });

    const float* widths = db.node_w.data();
    const float* heights = db.node_h.data();
    for (int i : order) {
        const double desiredX = originalX[i], desiredY = originalY[i];
        int best = -1;
        double bestCost = std::numeric_limits<double>::infinity();
        auto aboveIt = std::lower_bound(rowOrder.begin(), rowOrder.end(), desiredY,
            [&](int row, double y) { return db.rows[row].ly < y; });
        int above = static_cast<int>(aboveIt - rowOrder.begin());
        int below = above - 1;
        while (below >= 0 || above < result.rows) {
            const double belowDistance = below >= 0 ? std::abs(db.rows[rowOrder[below]].ly - desiredY)
                                                    : std::numeric_limits<double>::infinity();
            const double aboveDistance = above < result.rows ? std::abs(db.rows[rowOrder[above]].ly - desiredY)
                                                              : std::numeric_limits<double>::infinity();
            const double distance = std::min(belowDistance, aboveDistance);
            if (distance * distance >= bestCost) break;
            const int r = belowDistance <= aboveDistance ? rowOrder[below--] : rowOrder[above++];
            const auto& row = db.rows[r];
            if (heights[i] > row.height + 1e-5f) continue;
            const double width = siteWidth(widths[i], row.step);
            int candidate = -1;
            double nearest = std::numeric_limits<double>::infinity();
            for (int s : rowSegments[r]) {
                const auto& state = states[s];
                if (state.used + width > static_cast<double>(state.segment.hx) - state.segment.lx + 1e-7) continue;
                const double dx = std::abs(std::clamp(desiredX, static_cast<double>(state.segment.lx),
                                                     static_cast<double>(state.segment.hx) - width) - desiredX);
                if (dx < nearest) { nearest = dx; candidate = s; }
            }
            if (candidate < 0) continue;
            int prefix;
            const Cluster tail = options.referenceTrials ? referenceTrial(states[candidate], desiredX, width)
                                                         : trialTail(states[candidate], desiredX, width, prefix);
            const double dx = tail.x + tail.w - width - desiredX;
            const double cost = dx * dx + distance * distance;
            if (cost < bestCost) { bestCost = cost; best = candidate; }
        }
        if (best < 0) { ++result.failed; continue; }
        auto& state = states[best];
        const double step = db.rows[state.segment.row].step;
        const double width = siteWidth(widths[i], static_cast<float>(step));
        if (std::abs(width - widths[i]) > 1e-5) ++result.roundedWidths;
        if (options.referenceTrials) {
            appendReference(state.clusters, static_cast<int>(state.cells.size()), desiredX, width, state.segment);
        } else {
            int prefix;
            const Cluster tail = trialTail(state, desiredX, width, prefix);
            state.clusters.resize(prefix);
            state.clusters.push_back(tail);
        }
        state.cells.push_back(i);
        state.used += width;
    }

    double dispSum = 0.0;
    for (auto& state : states) {
        const auto& row = db.rows[state.segment.row];
        double previousRight = state.segment.lx;
        for (const auto& cluster : state.clusters) {
            double x = row.lx + std::round((cluster.x - row.lx) / row.step) * row.step;
            x = std::clamp(x, static_cast<double>(state.segment.lx), static_cast<double>(state.segment.hx) - cluster.w);
            // Rounding is monotone; retaining the preceding padded edge also protects FP ties.
            x = std::max(x, previousRight);
            for (int slot = cluster.first; slot <= cluster.last; ++slot) {
                const int i = state.cells[slot];
                db.setLlx(i, static_cast<float>(x));
                db.setLly(i, row.ly);
                const double displacement = std::hypot(static_cast<double>(db.llx(i)) - originalX[i],
                                                        static_cast<double>(db.lly(i)) - originalY[i]);
                dispSum += displacement;
                result.dispMax = std::max(result.dispMax, displacement);
                x += siteWidth(widths[i], row.step);
            }
            previousRight = x;
        }
        float right = state.segment.lx;
        for (int i : state.cells) {
            if (db.llx(i) < right - 1e-3f) ++result.selfcheckOverlaps;
            const double sites = (static_cast<double>(db.llx(i)) - row.lx) / row.step;
            if (std::abs(sites - std::round(sites)) * row.step > 1e-3 ||
                std::abs(db.lly(i) - row.ly) > 1e-3 || db.urx(i) > state.segment.hx + 1e-3f)
                ++result.selfcheckOffSite;
            right = db.urx(i);
        }
    }
    const int placed = result.cells - result.failed;
    result.dispMean = placed ? dispSum / placed : 0.0;
    return result;
}

}  // namespace sp
