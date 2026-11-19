#pragma once

#include <atomic>
#include <format>
#include <string_view>
#include <utility>

// Messages go to stderr and, on Android, to logcat under the tag `android-imgui`.
// `ANDROID_IMGUI_LOG` picks the level: trace, debug, info (the default), warn, error, off.
class Logger {
public:
    enum class Level { Off, Error, Warn, Info, Debug, Trace };

    // Starts at the level the environment names, so nothing has to be called first.
    Logger();

    Level GetLevel() const {
        return level_.load(std::memory_order_relaxed);
    }

    void SetLevel(Level level) {
        level_.store(level, std::memory_order_relaxed);
    }

    bool IsEnabled(Level level) const {
        return level != Level::Off && level <= GetLevel();
    }

    void Write(Level level, std::string_view text) const;

    template <class... Args>
    void Error(std::format_string<Args...> format, Args&&... args) const {
        Say(Level::Error, format, std::forward<Args>(args)...);
    }

    template <class... Args>
    void Warn(std::format_string<Args...> format, Args&&... args) const {
        Say(Level::Warn, format, std::forward<Args>(args)...);
    }

    template <class... Args>
    void Info(std::format_string<Args...> format, Args&&... args) const {
        Say(Level::Info, format, std::forward<Args>(args)...);
    }

    template <class... Args>
    void Debug(std::format_string<Args...> format, Args&&... args) const {
        Say(Level::Debug, format, std::forward<Args>(args)...);
    }

    template <class... Args>
    void Trace(std::format_string<Args...> format, Args&&... args) const {
        say(Level::Trace, format, std::forward<Args>(args)...);
    }

private:
    // Words a message only when its level is on.
    template <class... Args>
    void Say(Level level, std::format_string<Args...> format, Args&&... args) const {
        if (IsEnabled(level)) {
            Write(level, std::format(format, std::forward<Args>(args)...));
        }
    }

    // Nothing here needs tearing down, so the logger may be used for as long as the process runs.
    std::atomic<Level> level_;
};

inline Logger logger;
