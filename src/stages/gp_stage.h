#pragma once

#include "stage.h"

namespace sp {

/// 全局布局阶段（课程任务 5）。负责插入 filler、驱动 ePlace 的各个子阶段，
/// 并做越界与合法性检查。具体的优化算法在 gp/eplace 中。
class GlobalPlaceStage : public PlacementStage {
public:
    void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "gp"; }
};

}  // namespace sp
