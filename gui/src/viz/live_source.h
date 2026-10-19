// 实时数据源的工厂。
//
// 声明单独放一个头文件，是为了让 main_gui.cpp 不必 include 任何算法侧头文件——
// 对算法的依赖被完整地关在 live_source.cpp 这一个翻译单元里。
#pragma once

#include <memory>
#include <string>

#include "viz/source.h"

namespace viz {

/// 起一个 worker 线程跑布局，每 strideIters 轮发布一帧。
/// 仅在集成构建（-DSUPERPLACE_ROOT=...）中可用。
std::unique_ptr<IPlacementSource> makeLiveSource(const std::string& auxPath, int strideIters,
                                                 std::string* error);

}  // namespace viz
