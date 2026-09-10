#pragma once

#include <chrono>
#include <string>

namespace sp {

class Timer {
public:
    Timer() { reset(); }
    void reset() { t0_ = Clock::now(); }
    double elapsedMs() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0_).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point t0_;
};

class MetricsSink;

/// 作用域计时：析构时把耗时报给 MetricsSink。
class ScopedTimer {
public:
    ScopedTimer(MetricsSink& sink, std::string label);
    ~ScopedTimer();

private:
    MetricsSink& sink_;
    std::string label_;
    Timer timer_;
};

}  // namespace sp
