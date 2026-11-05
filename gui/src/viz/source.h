// IPlacementSource —— 前端与"数据从哪来"之间的唯一接缝。
//
// 这是整个低耦合设计的支点。UI 与渲染层**只认这个接口**，不知道背后是
// 一堆 bundle 文件。于是：
//
//   * 开发只对着 BundleSource，连算法侧的头文件都不需要 include；
//   * 算法侧重构 bundle 落盘格式不会波及 UI——波及的只有 BundleSource 那一个适配器。
//
// 形式上这就是"决策 7 Backend Strategy"的同一套路：纯接口 + 工厂，
// 实现藏在各自的 .cpp 里。
//
// T16：实时集成模式已整体移除（旧内嵌引擎数据源与对应可执行目标已删除），
// 本接口只服务离线 bundle，不再保留任何实时轮询接口。
#pragma once

#include <memory>
#include <string>

#include "viz/scene.h"

namespace viz {
struct NetList;  // T10


class IPlacementSource {
public:
    virtual ~IPlacementSource() = default;

    IPlacementSource(const IPlacementSource&) = delete;
    IPlacementSource& operator=(const IPlacementSource&) = delete;

    virtual const SceneInfo&    scene()    const = 0;
    virtual const NodeGeometry& geometry() const = 0;
    virtual const MetricsTable* metrics()  const = 0;   ///< 没有则返回 nullptr
    /// summary.json 总览（T1）。没有/解析失败返回 nullptr。
    virtual const SummaryTable* summary() const { return nullptr; }

    /// 可用帧数。
    virtual int  frameCount() const = 0;
    virtual bool frame(int index, FrameView& out) = 0;

    /// 帧元数据（stage/iter/t/breakBefore/label），**不读位置文件**。
    /// 越界返回 index=-1 的占位。T5 映射、进度条刻度、T14 时间轴一律用它，
    /// 绝不为取标签而对每帧调 frame()（那会把位置也读进来挤掉缓存，即 T15 根因）。
    virtual FrameMeta frameInfo(int index) const = 0;

    /// T17：可测缓存条目数（单测断言 LRU 有界，非实时路径）；-1 表示不支持。
    virtual int cachedFrameCount() const { return -1; }

    /// bundle 是否带真实全局 t（§3.2）。false 时时间轴退化为按帧序号均匀分布。
    virtual bool hasGlobalT() const { return false; }

    /// T17：提示后台线程按播放方向预读 center 附近的关键帧。默认空实现。
    virtual void prefetch(int /*center*/, int /*direction*/) {}

    virtual const std::string& sourceName() const = 0;

    /// T10：网表（§3.9）。无 nets/ 或校验失败返回 nullptr。首次调用才加载。
    virtual const NetList* nets() const { return nullptr; }
    /// 仅探测 bundle 是否声明 nets（不触发加载）。
    virtual bool hasNetsAvailable() const { return false; }
    /// 网表加载失败的具体原因（成功/无网表为空串）。
    virtual const std::string& netsError() const { static std::string e; return e; }

protected:
    IPlacementSource() = default;
};

/// 从一个 viz bundle 目录加载。失败时返回 nullptr 并填 error。
/// 这个工厂**不依赖算法侧任何东西**，独立构建时唯一可用的数据源。
std::unique_ptr<IPlacementSource> makeBundleSource(const std::string& dir, std::string* error);

}  // namespace viz
