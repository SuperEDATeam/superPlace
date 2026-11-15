#pragma once

#include <cmath>
#include <stdexcept>
#include <string>

#include "db/place_db.h"

namespace sp {
class InvariantError : public std::runtime_error {
public:
    const std::string kind, stage;
    InvariantError(std::string k, std::string s, const std::string& message)
        : std::runtime_error(s + ": " + message), kind(std::move(k)), stage(std::move(s)) {}
};
inline void requireInvariant(bool ok, const std::string& kind,
                             const std::string& stage, const std::string& message) {
    if (!ok) throw InvariantError(kind, stage, message);
}
inline void checkGeometry(const PlaceDB& db, const std::string& stage, bool bounds) {
    for (int i = 0; i < db.totalNodes(); ++i) {
        if (!(std::isfinite(db.node_x[i]) && std::isfinite(db.node_y[i]) &&
            std::isfinite(db.node_w[i]) && std::isfinite(db.node_h[i])))
            throw InvariantError("non_finite", stage, "non-finite node geometry: " + db.node_name[i]);
        // numMovable retains frozen macros after mLG. Original fixed IO pads
        // may legitimately lie outside the placement region.
        if (bounds && (i < db.numMovable || db.isFiller(i)) && !db.isNI(i))
            if (!(db.llx(i) >= db.coreRegion.lx - .01f &&
                db.lly(i) >= db.coreRegion.ly - .01f && db.urx(i) <= db.coreRegion.hx + .01f &&
                db.ury(i) <= db.coreRegion.hy + .01f))
                throw InvariantError("out_of_core", stage, "node outside core: " + db.node_name[i]);
    }
}
}  // namespace sp
