#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util/config.h"

namespace sp {

inline double hpwlReference(const Config& cfg, double initialHpwl, bool fillerOnly) {
    const double ref = cfg.lambda_schedule == "relative" && !fillerOnly
                           ? initialHpwl * cfg.delta_hpwl_relative : cfg.delta_hpwl_ref;
    // A netless fixture has zero HPWL: retain the finite historical scale.
    if (initialHpwl == 0.0 && cfg.lambda_schedule == "relative" && !fillerOnly)
        return cfg.delta_hpwl_ref;
    if (!(ref > 0.0) || !std::isfinite(ref))
        throw std::runtime_error("invalid effective HPWL reference");
    return ref;
}

inline float lambdaMultiplier(double deltaHpwl, double reference) {
    const double mu = std::pow(1.1, -deltaHpwl / reference + 1.0);
    return static_cast<float>(std::min(1.05, std::max(0.95, mu)));
}

}  // namespace sp
