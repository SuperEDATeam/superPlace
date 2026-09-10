// 极简日志。禁止在热路径中使用（05 §6）。
#pragma once

#include <cstdio>
#include <string>

namespace sp {

enum class LogLevel { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

class Logger {
public:
    static Logger& get() {
        static Logger inst;
        return inst;
    }
    void setLevel(LogLevel lv) { level_ = lv; }
    LogLevel level() const { return level_; }

private:
    LogLevel level_ = LogLevel::kInfo;
};

#define SP_LOG_IMPL(lv, tag, ...)                                     \
    do {                                                              \
        if (::sp::Logger::get().level() <= (lv)) {                    \
            std::fprintf(stdout, "[%s] ", tag);                       \
            std::fprintf(stdout, __VA_ARGS__);                        \
            std::fprintf(stdout, "\n");                               \
        }                                                             \
    } while (0)

#define SP_DEBUG(...) SP_LOG_IMPL(::sp::LogLevel::kDebug, "DEBUG", __VA_ARGS__)
#define SP_INFO(...)  SP_LOG_IMPL(::sp::LogLevel::kInfo,  "INFO ", __VA_ARGS__)
#define SP_WARN(...)  SP_LOG_IMPL(::sp::LogLevel::kWarn,  "WARN ", __VA_ARGS__)
#define SP_ERROR(...) SP_LOG_IMPL(::sp::LogLevel::kError, "ERROR", __VA_ARGS__)

}  // namespace sp
