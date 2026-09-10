// 统一随机源。铁律 7：禁止 rand()、std::random_device、时间种子。
// 所有随机行为必须经由本文件，且种子来自 Config::random_seed。
#pragma once

#include <cstdint>
#include <random>

namespace sp {

class Rng {
public:
    explicit Rng(uint64_t seed) : eng_(static_cast<std::mt19937::result_type>(seed)) {}

    /// [lo, hi) 均匀实数
    float uniform(float lo, float hi) {
        std::uniform_real_distribution<float> dist(lo, hi);
        return dist(eng_);
    }

    /// [lo, hi] 均匀整数
    int uniformInt(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(eng_);
    }

    std::mt19937& engine() { return eng_; }

private:
    std::mt19937 eng_;
};

}  // namespace sp
