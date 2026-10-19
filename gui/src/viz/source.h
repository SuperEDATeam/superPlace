// IPlacementSource —— 前端与"数据从哪来"之间的唯一接缝。
//
// 这是整个低耦合设计的支点。UI 与渲染层**只认这个接口**，不知道背后是
// 一堆文件还是一个正在跑的布局器。于是：
//
//   * 同伴日常开发只对着 BundleSource，连算法侧的头文件都不需要 include；
//   * 将来接实时模式时新增一个 LiveSource，UI 代码一行都不用改；
//   * 算法侧重构 PlaceDB 不会波及这里——波及的只有 LiveSource 那一个适配器。
//
// 形式上这就是算法侧"决策 7 Backend Strategy"的同一套路：纯接口 + 工厂，
// 实现藏在各自的 .cpp 里。
#pragma once

#include <memory>
#include <string>

#include "viz/scene.h"

namespace viz {

class IPlacementSource {
public:
    virtual ~IPlacementSource() = default;

    IPlacementSource(const IPlacementSource&) = delete;
    IPlacementSource& operator=(const IPlacementSource&) = delete;

    virtual const SceneInfo&    scene()    const = 0;
    virtual const NodeGeometry& geometry() const = 0;
    virtual const MetricsTable* metrics()  const = 0;   ///< 没有则返回 nullptr

    /// 可用帧数。实时源会随迭代推进不断变大。
    virtual int  frameCount() const = 0;
    virtual bool frame(int index, FrameView& out) = 0;

    /// 实时源返回 true。UI 据此决定是否自动追帧、是否显示"运行中"状态。
    virtual bool isLive() const { return false; }

    /// 实时源用：有新帧时返回 true（UI 每帧轮询一次，不阻塞）。
    virtual bool poll() { return false; }

    virtual const std::string& sourceName() const = 0;

protected:
    IPlacementSource() = default;
};

/// 从一个 viz bundle 目录加载。失败时返回 nullptr 并填 error。
/// 这个工厂**不依赖算法侧任何东西**，独立构建时唯一可用的数据源。
std::unique_ptr<IPlacementSource> makeBundleSource(const std::string& dir, std::string* error);

}  // namespace viz
