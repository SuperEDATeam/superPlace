// T13：零依赖断言宏（不引入 gtest，符合 06 §9.6 的要求）。
// 风格对齐算法侧 test/test_util.h：CHECK_EQ / CHECK_NEAR / CHECK_TRUE。
#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>

namespace viztest {

inline int& failures() { static int v = 0; return v; }
inline int& checks() { static int v = 0; return v; }

inline void report(const char* file, int line, const char* cond, const std::string& extra) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d  %s  %s\n", file, line, cond, extra.c_str());
}

template <class T>
inline std::string toStr(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_enum_v<T>) os << static_cast<long long>(v);
    else os << v;
    return os.str();
}

}  // namespace viztest

#define CHECK_TRUE(cond)                                                     \
    do {                                                                    \
        ++::viztest::checks();                                             \
        if (!(cond)) ::viztest::report(__FILE__, __LINE__, #cond, "");     \
    } while (0)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        ++::viztest::checks();                                             \
        const auto _va = (a);                                             \
        const auto _vb = (b);                                             \
        if (!(_va == _vb))                                                \
            ::viztest::report(__FILE__, __LINE__, #a " == " #b,           \
                              ::viztest::toStr(_va) + " != " +            \
                                  ::viztest::toStr(_vb));                 \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                    \
        ++::viztest::checks();                                             \
        if (std::fabs((a) - (b)) > (eps))                                 \
            ::viztest::report(__FILE__, __LINE__, #a " ≈ " #b,            \
                              ::viztest::toStr(a) + " vs " +             \
                                  ::viztest::toStr(b));                  \
    } while (0)

#define CHECK_STREQ(a, b)                                                   \
    do {                                                                    \
        ++::viztest::checks();                                             \
        if (std::string(a) != std::string(b))                            \
            ::viztest::report(__FILE__, __LINE__, #a " == " #b,          \
                              std::string(a) + " != " + std::string(b));\
    } while (0)
