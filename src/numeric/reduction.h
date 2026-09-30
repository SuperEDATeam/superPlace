// 固定顺序的并行归约（铁律 7）。
//
// OpenMP 的 `reduction(+:)` 不保证求和次序：私有副本的划分取决于线程数与调度策略，
// 合并次序则是实现自定的。浮点加法不满足结合律，于是同一份输入换个线程数就会得到
// 不同的结果——实测 adaptec1 上跨 1/4/8 线程，60 轮后 HPWL 相对差约 1e-5。
// 配 `schedule(dynamic)` 时更糟：划分随运行时刻的抢占情况变化，同线程数下
// 两次运行都未必一致。
//
// 本文件把归约改成【固定分块】：无论多少线程，一律切成 kChunks 块，
// 块内顺序累加、块间按块号顺序合并。结果只取决于 n 与 kChunks，与线程数、
// 调度策略、运行时刻全部无关。
//
// 铁律 1：本文件不得出现任何布局概念，它只是一个求和工具。
#pragma once

#include <cstdint>
#include <vector>

namespace sp {

/// 固定分块数。取 64 是因为它远大于常见核数（负载均衡足够细），
/// 又远小于 n（每块内仍是长顺序累加，不会退化成逐元素合并而损失精度）。
inline constexpr int kReduceChunks = 64;

/// 块 c 在 [0, n) 上的下标区间 [lo, hi)。划分只依赖 n 与 c。
inline void chunkRange(int64_t n, int c, int64_t& lo, int64_t& hi) {
    lo = n * c / kReduceChunks;
    hi = n * (c + 1) / kReduceChunks;
}

/// 规模低于此值就不开并行域。
///
/// 开一个 OpenMP 并行域的固定开销在几微秒到几十微秒量级，而主循环每轮要调用
/// 好几次归约。小规模下这笔开销会完全压倒计算——实测 1.3 万节点的合成用例，
/// 32 线程（20.8 s）比 8 线程（0.73 s）慢 28 倍，全部耗在并行域的开合上。
///
/// 走串行路径时**分块方式完全不变**，因此结果与并行路径逐位相同，
/// 不会引入"小用例和大用例算得不一样"这种更难查的问题。
inline constexpr int64_t kReduceSerialBelow = 1 << 14;   // 16384

/// 确定性并行求和：f(i) 返回第 i 项的贡献。
template <typename F>
double deterministicSum(int64_t n, F&& f) {
    if (n <= 0) return 0.0;
    std::vector<double> partial(static_cast<size_t>(kReduceChunks), 0.0);

    if (n < kReduceSerialBelow) {
        for (int c = 0; c < kReduceChunks; ++c) {
            int64_t lo = 0, hi = 0;
            chunkRange(n, c, lo, hi);
            double s = 0.0;
            for (int64_t i = lo; i < hi; ++i) s += f(i);
            partial[static_cast<size_t>(c)] = s;
        }
    } else {
        // dynamic,1 只影响哪个线程领到哪一块，不影响块的划分与合并次序
#pragma omp parallel for schedule(dynamic, 1)
        for (int c = 0; c < kReduceChunks; ++c) {
            int64_t lo = 0, hi = 0;
            chunkRange(n, c, lo, hi);
            double s = 0.0;
            for (int64_t i = lo; i < hi; ++i) s += f(i);
            partial[static_cast<size_t>(c)] = s;
        }
    }

    double total = 0.0;
    for (int c = 0; c < kReduceChunks; ++c) total += partial[static_cast<size_t>(c)];
    return total;
}

/// 确定性并行求和 + 副作用：body(i, acc) 在累加的同时可以写出别的结果，
/// 避免为了归约再多扫一趟。用于"算梯度顺便统计梯度范数"这类场合。
template <typename F>
double deterministicSumWithWork(int64_t n, F&& body) {
    if (n <= 0) return 0.0;
    std::vector<double> partial(static_cast<size_t>(kReduceChunks), 0.0);

    if (n < kReduceSerialBelow) {
        for (int c = 0; c < kReduceChunks; ++c) {
            int64_t lo = 0, hi = 0;
            chunkRange(n, c, lo, hi);
            double s = 0.0;
            for (int64_t i = lo; i < hi; ++i) body(i, s);
            partial[static_cast<size_t>(c)] = s;
        }
    } else {
#pragma omp parallel for schedule(dynamic, 1)
        for (int c = 0; c < kReduceChunks; ++c) {
            int64_t lo = 0, hi = 0;
            chunkRange(n, c, lo, hi);
            double s = 0.0;
            for (int64_t i = lo; i < hi; ++i) body(i, s);
            partial[static_cast<size_t>(c)] = s;
        }
    }

    double total = 0.0;
    for (int c = 0; c < kReduceChunks; ++c) total += partial[static_cast<size_t>(c)];
    return total;
}

}  // namespace sp
