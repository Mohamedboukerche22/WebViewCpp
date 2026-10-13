#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

namespace wvc {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

using LogSink = std::function<void(LogLevel, std::string_view)>;

// Routes logs to a user sink (e.g. a "log" IPC channel) or stderr. The platform
// backends use this instead of printf so that a host app can capture everything
// the runtime emits.
class Log {
public:
    static void set_sink(LogSink new_sink) { sink_ref() = std::move(new_sink); }
    static void set_min_level(LogLevel lvl) { min_level_ref() = lvl; }
    static LogLevel min_level() { return min_level_ref(); }

    static void write(LogLevel level, std::string_view message) {
        if (level < min_level_ref()) return;
        if (auto& s = sink_ref()) { s(level, message); return; }
        std::fprintf(stderr, "[wvc][%s] %.*s\n", name(level),
                     static_cast<int>(message.size()), message.data());
    }

    static const char* name(LogLevel lvl) {
        switch (lvl) {
            case LogLevel::Trace: return "trace";
            case LogLevel::Debug: return "debug";
            case LogLevel::Info:  return "info";
            case LogLevel::Warn:  return "warn";
            case LogLevel::Error: return "error";
        }
        return "?";
    }

private:
    static LogSink& sink_ref() { static LogSink s; return s; }
    static LogLevel& min_level_ref() { static LogLevel l = LogLevel::Info; return l; }
};

#define WVC_LOG(level, msg) ::wvc::Log::write(level, msg)
#define WVC_TRACE(msg) WVC_LOG(::wvc::LogLevel::Trace, msg)
#define WVC_DEBUG(msg) WVC_LOG(::wvc::LogLevel::Debug, msg)
#define WVC_INFO(msg)  WVC_LOG(::wvc::LogLevel::Info,  msg)
#define WVC_WARN(msg)  WVC_LOG(::wvc::LogLevel::Warn,  msg)
#define WVC_ERROR(msg) WVC_LOG(::wvc::LogLevel::Error, msg)

}  // namespace wvc
