#pragma once

#include "init/initial_placer.h"

namespace sp {

class RandomPlacer : public IInitialPlacer {
public:
    void place(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "random"; }
};

}  // namespace sp
