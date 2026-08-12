// Logger.h — 最小文件日志。
//
// 约束（见 DEVELOPMENT.md §2.3 / §4.6）：
//   - 无异常（ExceptionHandling=false，HAS_EXCEPTIONS=0）：不得 throw / try-catch
//   - 静态 CRT：可用 <cstdio>，但不要依赖会拉入 VC 运行库的重型设施
//   - 日志失败必须静默降级，绝不影响游戏运行
#pragma once

#include <cstdio>
#include <cstdarg>

#include "GamePaths.h"

namespace Log {

    enum class Level { Off = 0, Error, Warn, Info, Debug };

    // 全局级别，由 Config 载入后设置；默认 Info。
    inline Level g_level = Level::Info;

    namespace detail {
        // 单一输出点。追加写入，打不开就静默丢弃（不崩游戏）。
        inline void Write(const char* tag, const char* fmt, va_list args) {
            GamePaths::EnsureDataDirectory();
            char path[MAX_PATH] = {};
            if (!GamePaths::Build(path, sizeof(path), "ra2hook\\ra2hook.log")) return;
            std::FILE* f = std::fopen(path, "a");
            if (!f) return;
            std::fputs(tag, f);
            std::vfprintf(f, fmt, args);
            std::fputc('\n', f);
            std::fclose(f);
        }
        inline void Line(Level lvl, const char* tag, const char* fmt, va_list args) {
            if (static_cast<int>(lvl) > static_cast<int>(g_level)) return;
            Write(tag, fmt, args);
        }
    }

    inline void Error(const char* fmt, ...) { va_list a; va_start(a, fmt); detail::Line(Level::Error, "[ERR ] ", fmt, a); va_end(a); }
    inline void Warn (const char* fmt, ...) { va_list a; va_start(a, fmt); detail::Line(Level::Warn,  "[WARN] ", fmt, a); va_end(a); }
    inline void Info (const char* fmt, ...) { va_list a; va_start(a, fmt); detail::Line(Level::Info,  "[INFO] ", fmt, a); va_end(a); }
    inline void Debug(const char* fmt, ...) { va_list a; va_start(a, fmt); detail::Line(Level::Debug, "[DBG ] ", fmt, a); va_end(a); }

}  // namespace Log
