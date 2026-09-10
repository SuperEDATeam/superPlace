// 极简断言工具：不引入外部测试框架，保持零依赖。
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace sptest {

inline int g_failures = 0;
inline int g_checks   = 0;

inline void reportEq(bool ok, const char* expr, const std::string& got,
                     const std::string& want, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s:%d\n  expr : %s\n  got  : %s\n  want : %s\n",
                 file, line, expr, got.c_str(), want.c_str());
}

template <typename T>
std::string str(const T& v) {
    return std::to_string(v);
}
inline std::string str(const std::string& v) { return v; }
inline std::string str(const char* v) { return v; }

#define CHECK_EQ(actual, expected)                                                     \
    ::sptest::reportEq((actual) == (expected), #actual, ::sptest::str(actual),         \
                       ::sptest::str(expected), __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tol)                                              \
    ::sptest::reportEq(std::fabs(double(actual) - double(expected)) <= (tol), #actual,  \
                       ::sptest::str(double(actual)), ::sptest::str(double(expected)), \
                       __FILE__, __LINE__)

#define CHECK_TRUE(cond)                                                               \
    ::sptest::reportEq(bool(cond), #cond, "false", "true", __FILE__, __LINE__)

inline int summary(const char* name) {
    if (g_failures == 0) {
        std::printf("[PASS] %s (%d checks)\n", name, g_checks);
        return 0;
    }
    std::printf("[FAIL] %s (%d/%d checks failed)\n", name, g_failures, g_checks);
    return 1;
}

}  // namespace sptest
