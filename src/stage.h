// 阶段统一接口（05 §4.1）。
//
// 依赖铁律：三个算法阶段互不依赖，只通过 PlaceDB 通信。
// gp/ 不得 include init/，lg/ 不得 include gp/，以此类推。
#pragma once

#include <memory>
#include <string>

namespace sp {

class PlaceDB;
struct Config;
class MetricsSink;

class PlacementStage {
public:
    virtual ~PlacementStage() = default;
    /// 读写 db；逐轮指标推给 sink；参数从 cfg 取。
    virtual void run(PlaceDB& db, const Config& cfg, MetricsSink& sink) = 0;
    virtual const char* name() const = 0;
};

/// 按名字构造阶段。未知名字返回 nullptr。
std::unique_ptr<PlacementStage> makeStage(const std::string& name);

}  // namespace sp
