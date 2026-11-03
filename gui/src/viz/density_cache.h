#pragma once
// 关键帧密度预计算缓存（无 GL、可单测）。
//
// 契约：
//  - start() 启动后台线程逐关键帧调用 builder(int) 产出密度场；builder 不得抛（内部应 catch）。
//  - 预算上限 budget：只预计算前 min(frameCount, budget) 帧；超出的 get() 返回空（调用方退化当场 build）。
//  - 发布协议：所有字段/ready/done 由 mu_ 保护；worker 每完成一帧在锁内发布。
//  - 生命周期：调用方保证 builder 捕获的源在 stop()/析构返回前始终有效（App 在 setSource 中先 stop+join 再换源）。
//  - stop() 可重复调用；析构会 stop+join，绝不 detach/terminate。
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace viz {

// 近景绘制/上传的统一状态策略（App 与测试共用同一规则，避免测试局部 bool 假覆盖）。
// positionsValid：本帧 frame() 成功 materialize；hasPtr：有有效位置指针；lodMoving：LOD 且正在播放。
inline bool shouldDrawPositions(bool positionsValid) { return positionsValid; }
inline bool shouldUploadPositions(bool positionsValid, bool hasPtr, bool lodMoving) {
    return positionsValid && hasPtr && !lodMoving;
}

struct DensityFieldData {
    std::vector<float> data;
    float maxNorm = 0.f;
    bool ok = false;
};

class DensityCache {
public:
    using Builder = std::function<DensityFieldData(int frame)>;

    DensityCache() = default;
    ~DensityCache() { stop(); join(); }

    DensityCache(const DensityCache&) = delete;
    DensityCache& operator=(const DensityCache&) = delete;

    // 启动后台预计算。重复调用前必须先 stop+join。
    void start(int frameCount, int budget, Builder builder);

    // 请求取消（不阻塞）。
    void stop() { stop_.store(true); }
    // 阻塞直到 worker 退出。
    void join();

    // 取已发布帧；未计算/未就绪/越界返回 ok=false 的空场。
    DensityFieldData get(int key) const;

    int ready() const;
    bool done() const;
    int budgetFrames() const { return budgetFrames_; }

private:
    mutable std::mutex mu_;
    std::vector<DensityFieldData> fields_;
    int ready_ = 0;
    bool done_ = false;
    int budgetFrames_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false};
};

} // namespace viz
