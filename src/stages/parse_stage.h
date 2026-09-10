#pragma once

#include "stage.h"

namespace sp {

/// M1 阶段：解析已在 main 中完成，此阶段负责计算并输出统计信息。
/// 保留为独立 Stage 是为了让 pipeline 组装方式从一开始就统一。
class ParseStage : public PlacementStage {
public:
    void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "parse"; }
};

}  // namespace sp
