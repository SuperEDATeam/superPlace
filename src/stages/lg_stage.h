#pragma once

#include "stage.h"

namespace sp {

class LegalizeStage : public PlacementStage {
public:
    void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "lg"; }
};

}  // namespace sp
