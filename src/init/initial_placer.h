// 初始布局算法的抽象接口（课程任务 4）。
//
// 四个实现对应课程要求的三种方法：
//   random      随机布局                         —— 方法一
//   cluster_fc  聚类驱动，First-Choice 合并       —— 方法二（变体 A）
//   cluster_bc  聚类驱动，Best-Choice 合并        —— 方法二（变体 B，忠于课程资料描述）
//   quadratic   Kraftwerk2 B2B 二次解析布局       —— 方法三
//
// FC 与 BC 是聚类方法内部的两种合并策略，不是第四种方法。
//
// 与决策 7 的 Backend Strategy 同构：调用方只认接口，运行时由 --init-method 选择。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace sp {

class PlaceDB;
struct Config;
class MetricsSink;

class IInitialPlacer {
public:
    virtual ~IInitialPlacer() = default;
    /// 就地改写 db 中可移动节点的位置；逐轮指标推给 sink。
    virtual void place(PlaceDB& db, const Config& cfg, MetricsSink& sink) = 0;
    virtual const char* name() const = 0;
};

/// 未知名字返回 nullptr。
std::unique_ptr<IInitialPlacer> makeInitialPlacer(const std::string& method);

/// 全部可用方法名，供 CLI 提示与对比脚本枚举。
const std::vector<std::string>& initialPlacerNames();

}  // namespace sp
