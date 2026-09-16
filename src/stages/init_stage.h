#pragma once

#include "stage.h"

namespace sp {

/// 初始布局阶段（课程任务 4）。具体算法由 Config::init_method 选择，
/// 本阶段只负责调度与指标汇总，不知道背后是哪种方法。
class InitStage : public PlacementStage {
public:
    void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) override;
    const char* name() const override { return "init"; }
};

}  // namespace sp
