#include "Utils/Logger.h"

#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <utility>

#ifdef __ANDROID__
#include <android/log.h>
#endif

Logger::Logger() {
    const char* name = std::getenv("ANDROID_IMGUI_LOG");
    const std::string_view level = name ? name : "";
    level_ = level == "trace" ? Level::Trace : level == "debug" ? Level::Debug : level == "warn" ? Level::Warn : level == "error" ? Level::Error : level == "off" ? Level::Off : Level::Info;
}

void Logger::Write(Level level, std::string_view text) const {
    static constexpr char Tags[] = {' ', 'E', 'W', 'I', 'D', 'T'};
    std::println(stderr, "[{}] {}", Tags[std::to_underlying(level)], text);
#ifdef __ANDROID__
    static constexpr int Priorities[] = {ANDROID_LOG_SILENT, ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO, ANDROID_LOG_DEBUG, ANDROID_LOG_VERBOSE};
    __android_log_write(Priorities[std::to_underlying(level)], "android-imgui", std::string(text).c_str());
#endif
}
