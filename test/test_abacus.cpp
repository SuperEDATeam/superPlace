#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>

#include "db/place_db.h"
#include "lg/abacus.h"
#include "stages/lg_stage.h"
#include "util/config.h"
#include "util/metrics.h"
#include "test_util.h"

namespace {
sp::PlaceDB fixture(int rows = 2, int sites = 20, float step = 1.f) {
    sp::PlaceDB db;
    for (int r = 0; r < rows; ++r) db.rows.push_back({float(r * 2), 2.f, 0.f, step, sites});
    db.computeRegions();
    return db;
}
int cell(sp::PlaceDB& db, float x, float y, float width = 2.f, uint8_t flag = 0, float height = 2.f) {
    const int i = db.addNode("c" + std::to_string(db.totalNodes()), width, height, flag);
    db.setLlx(i, x); db.setLly(i, y);
    db.numNodes = static_cast<int>(db.node_x.size());
    return i;
}
void validate(const sp::PlaceDB& db) {
    for (int i = 0; i < db.numMovable; ++i) {
        if (db.isFixed(i) || db.isMacro(i) || db.isNI(i) || db.node_w[i] <= 0.f) continue;
        bool inRow = false;
        for (const auto& r : db.rows) {
            if (std::abs(db.lly(i) - r.ly) < 1e-4f && db.llx(i) >= r.lx - 1e-4f &&
                db.urx(i) <= r.hx() + 1e-4f && db.ury(i) <= r.hy() + 1e-4f) {
                const double site = (db.llx(i) - r.lx) / r.step;
                CHECK_NEAR(site, std::round(site), 1e-4); inRow = true; break;
            }
        }
        CHECK_TRUE(inRow);
        for (int j = i + 1; j < db.numNodes; ++j) {
            if (db.isNI(j) || db.node_w[j] <= 0.f || db.node_h[j] <= 0.f) continue;
            const float overlapX = std::min(db.urx(i), db.urx(j)) - std::max(db.llx(i), db.llx(j));
            const float overlapY = std::min(db.ury(i), db.ury(j)) - std::max(db.lly(i), db.lly(j));
            CHECK_TRUE(overlapX <= 1e-4f || overlapY <= 1e-4f);
        }
    }
}
void equalPositions(const sp::PlaceDB& a, const sp::PlaceDB& b) {
    CHECK_EQ(a.node_x.size(), b.node_x.size());
    CHECK_TRUE(std::memcmp(a.node_x.data(), b.node_x.data(), a.node_x.size() * sizeof(float)) == 0);
    CHECK_TRUE(std::memcmp(a.node_y.data(), b.node_y.data(), a.node_y.size() * sizeof(float)) == 0);
}
}  // namespace

int main() {
    {
        auto db = fixture(1);
        cell(db, 8.f, 0.f); cell(db, 8.f, 0.f); db.numMovable = 2;
        const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 0); CHECK_EQ(r.selfcheckOverlaps, 0);
        CHECK_NEAR(db.llx(0), 7.f, 1e-5); CHECK_NEAR(db.llx(1), 9.f, 1e-5);
        CHECK_NEAR(std::abs(db.llx(0) - 8.f), std::abs(db.llx(1) - 8.f), 1e-5);
        validate(db);
    }
    {
        auto db = fixture(2, 10, 2.f);
        cell(db, 8.f, 0.f); cell(db, 10.f, 0.f);
        cell(db, 6.2f, 0.f, 7.6f, sp::F_FIXED);
        db.numMovable = 2;
        const auto segments = sp::buildAbacusSegments(db);
        CHECK_EQ(segments.size(), size_t(3));
        CHECK_EQ(segments[0].lx, 0.f); CHECK_EQ(segments[0].hx, 6.f);
        CHECK_EQ(segments[1].lx, 14.f); CHECK_EQ(segments[1].hx, 20.f);
        CHECK_EQ(segments[2].row, 1); CHECK_EQ(segments[2].lx, 0.f);
        const float fixedX = db.node_x[2], fixedY = db.node_y[2];
        const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 0); CHECK_EQ(db.node_x[2], fixedX); CHECK_EQ(db.node_y[2], fixedY);
        validate(db);
    }
    {
        auto db = fixture(3, 2);
        cell(db, 0.f, 2.f); cell(db, 0.f, 2.f); db.numMovable = 2;
        const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 0); CHECK_EQ(db.lly(0), 2.f);
        CHECK_EQ(db.lly(1), 0.f); validate(db);
    }
    {
        auto db = fixture(2, 12, 2.f);
        cell(db, -30.f, -5.f, 3.f); cell(db, 50.f, 20.f, 5.f); db.numMovable = 2;
        const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 0); CHECK_EQ(r.roundedWidths, 2); validate(db);
    }
    {
        auto db = fixture(2);
        cell(db, 4.f, 0.f); cell(db, 8.f, 0.f);
        cell(db, 10.f, 0.f, 4.f, sp::F_FIXED, 4.f);
        cell(db, 3.f, 0.f, 0.f, sp::F_FIXED | sp::F_NI, 0.f);
        db.numMovable = 2;
        const int filler = db.addNode("filler", 2.f, 2.f, sp::F_FILLER);
        db.numNodes = 4; db.numFillers = 1;
        db.node_x[filler] = 7.f; db.node_y[filler] = 3.f;
        const auto before = db;
        const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 0);
        for (int i = 2; i < db.totalNodes(); ++i) {
            CHECK_EQ(db.node_x[i], before.node_x[i]); CHECK_EQ(db.node_y[i], before.node_y[i]);
        }
        validate(db);
    }
    {
        auto db = fixture(1, 4);
        for (int n = 0; n < 3; ++n) cell(db, 1.f, 0.f);
        db.numMovable = 3; const auto r = sp::legalizeCells(db);
        CHECK_EQ(r.failed, 1); CHECK_EQ(db.llx(2), 1.f);
    }
    {
        auto db = fixture();
        cell(db, 8.f, 0.f); cell(db, 8.f, 0.f, 4.f, sp::F_MACRO, 4.f); db.numMovable = 2;
        const auto before = db; const auto r = sp::legalizeCells(db);
        CHECK_TRUE(!r.preconditionOk); equalPositions(before, db);
        sp::MetricsSink sink; sp::Config cfg; sp::LegalizeStage stage;
        stage.run(db, cfg, sink); equalPositions(before, db);
    }
    {
        auto db = fixture(1);
        cell(db, 15.f, 0.f, 4.f); cell(db, 16.f, 0.f, 4.f); db.numMovable = 2;
        const auto r = sp::legalizeCells(db); CHECK_EQ(r.failed, 0); validate(db);
    }
    {
        auto db = fixture(2);
        cell(db, 2.f, 0.f); cell(db, 7.f, 0.f, 4.f, sp::F_FIXED, 4.f);
        cell(db, 9.f, 0.f, 4.f, sp::F_FIXED, 2.f); db.numMovable = 1;
        const auto s = sp::buildAbacusSegments(db);
        CHECK_EQ(s.size(), size_t(4)); CHECK_EQ(s[1].lx, 13.f); CHECK_EQ(s[3].lx, 11.f);
    }
    {
        auto db = fixture(0);
        cell(db, 0.f, 0.f); db.numMovable = 1;
        const auto r = sp::legalizeCells(db); CHECK_EQ(r.failed, 1);
    }
    {
        auto db = fixture(1);
        cell(db, 15.f, 0.f, 2.f);
        cell(db, 3.f, 2.f, 10.f, sp::F_FIXED, 2.f);
        db.numMovable = 1;
        const auto segments = sp::buildAbacusSegments(db);
        CHECK_EQ(segments.size(), size_t(1)); CHECK_EQ(segments[0].hx, 20.f);
        CHECK_EQ(sp::legalizeCells(db).failed, 0); validate(db);
    }
    {
        auto db = fixture(2);
        cell(db, 1.f, 0.f, 2.f);
        cell(db, 5.f, 0.f, 4.f, sp::F_MACRO | sp::F_FIXED, 4.f);
        db.numMovable = 2;
        const auto before = db;
        CHECK_EQ(sp::legalizeCells(db).failed, 0);
        CHECK_EQ(db.node_x[1], before.node_x[1]); CHECK_EQ(db.node_y[1], before.node_y[1]);
        validate(db);
    }
    {
        auto db = fixture(1);
        cell(db, 2.f, 0.f); db.numMovable = 1;
        db.rows[0].step = 0.f;
        const auto before = db;
        bool rejected = false;
        try { sp::legalizeCells(db); } catch (const std::invalid_argument&) { rejected = true; }
        CHECK_TRUE(rejected); equalPositions(before, db);
    }
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 100; ++trial) {
        auto db = fixture(5, 80);
        for (int n = 0; n < 40; ++n) {
            const float x = float(int(rng() % 110) - 10) + 0.125f;
            const float y = float(int(rng() % 14) - 3);
            cell(db, x, y, float(1 + rng() % 5));
        }
        db.numMovable = 40;
        cell(db, 30.f, 2.f, 10.f, sp::F_FIXED, 4.f);
        auto reference = db, repeat = db;
        const auto r = sp::legalizeCells(db);
        sp::AbacusOptions opts; opts.referenceTrials = true;
        const auto rr = sp::legalizeCells(reference, opts);
        sp::legalizeCells(repeat);
        CHECK_EQ(r.failed, 0); CHECK_EQ(rr.failed, 0);
        equalPositions(db, reference); equalPositions(db, repeat);
        CHECK_EQ(r.selfcheckOverlaps, 0); CHECK_EQ(r.selfcheckOffSite, 0);
        validate(db);
    }
    return sptest::summary("test_abacus");
}
