#include "density_cache.h"
#include <utility>

namespace viz {

void DensityCache::start(int frameCount, int budget, Builder builder) {
    stop();
    join();
    stop_.store(false);
    const int cap = (frameCount > 0 && budget > 0) ? std::min(frameCount, budget) : 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fields_.assign(static_cast<size_t>(cap), DensityFieldData{});
        ready_ = 0;
        done_ = false;
        budgetFrames_ = cap;
    }
    if (cap <= 0 || !builder) {
        std::lock_guard<std::mutex> lk(mu_);
        done_ = true;
        return;
    }
    worker_ = std::thread([this, cap, b = std::move(builder)]() {
        for (int i = 0; i < cap; ++i) {
            if (stop_.load()) break;
            DensityFieldData f;
            try {
                f = b(i); // builder 应自行处理失败帧，不抛
            } catch (...) {
                f = DensityFieldData{}; // 异常帧记为失败，不 terminate
            }
            std::lock_guard<std::mutex> lk(mu_);
            fields_[static_cast<size_t>(i)] = std::move(f);
            ++ready_;
        }
        std::lock_guard<std::mutex> lk(mu_);
        done_ = true;
    });
}

void DensityCache::join() {
    if (worker_.joinable()) worker_.join();
}

DensityFieldData DensityCache::get(int key) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (key < 0 || key >= static_cast<int>(fields_.size())) return {};
    return fields_[static_cast<size_t>(key)];
}

int DensityCache::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

bool DensityCache::done() const {
    std::lock_guard<std::mutex> lk(mu_);
    return done_;
}

} // namespace viz
