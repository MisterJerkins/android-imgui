#pragma once

#include "Utils/Utils.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

// A file descriptor that is closed when its owner goes away.
class Fd {
public:
    Fd() = default;
    explicit Fd(int descriptor) : descriptor_(descriptor) {
    }
    Fd(Fd&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            Reset();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~Fd() {
        Reset();
    }

    explicit operator bool() const {
        return descriptor_ >= 0;
    }
    int Get() const {
        return descriptor_;
    }

    void Reset() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = -1;
    }

private:
    int descriptor_ = -1;
};

// A system library, opened by name and kept for as long as the process lives.
class Library {
public:
    static Result<Library> Open(const char* name);

    // The address of a symbol, or null where the library does not have it.
    void* Find(const char* symbol) const;

    // `F` must be the function pointer type matching the symbol's real ABI.
    template <class F>
    F Func(const char* symbol) const {
        return reinterpret_cast<F>(Find(symbol));
    }

private:
    explicit Library(void* handle) : handle_(handle) {
    }

    void* handle_;
};

// What the system says about itself.
class System {
public:
    // One of Android's system properties.
    static std::optional<std::string> GetProperty(const char* name);
    static std::optional<double> GetPropertyNumber(const char* name);

    static uint32_t GetApiLevel();

    // How many pixels a density-independent one takes on the main display.
    static float GetDensity();

    // What a shell command prints, all of it.
    static std::string RunCommand(const char* command);
};

// Tells when entries appear in directories or leave them, without looking at them in between.
class DirectoryWatch {
public:
    // Watches those of the directories that can be watched; nothing when none of them can.
    static std::optional<DirectoryWatch> Create(std::span<const char* const> directories);

    // Sleeps until something appeared or left. `false` when the watch itself is gone.
    bool Wait();

private:
    explicit DirectoryWatch(Fd events) : events_(std::move(events)) {
    }

    Fd events_;
};

// A child process whose standard input and output are pipes held here. It is killed when its
// owner goes away, if it has not left by then.
class ChildProcess {
public:
    struct Start {
        // The program, by its path or by a name to be found along PATH, then its arguments.
        std::vector<std::string> command;
        // Variables set on top of this process's own.
        std::vector<std::pair<std::string, std::string>> environment;
        // The user and group to run as, when not this process's own.
        std::optional<uint32_t> user;
    };

    static Result<ChildProcess> Spawn(const Start& start);

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&&) = delete;
    ~ChildProcess();

    // Sends a line to the child. `false` once it no longer reads.
    bool WriteLine(std::string_view line);
    void CloseInput();
    // The pipe the child writes to, for whoever is going to read it.
    Fd TakeOutput();

    // Whether the child is still there.
    bool IsRunning();
    void Kill();

private:
    // The two ends of a pipe.
    struct Pipe;

    ChildProcess(pid_t process, Fd input, Fd output) : process_(process), input_(std::move(input)), output_(std::move(output)) {
    }

    // The file to run for a program: one named without a folder is looked for along PATH, as a
    // shell does it, or where the system keeps its programs when there is no PATH.
    static Result<std::string> FindProgram(const std::string& name);

    pid_t process_ = -1;
    Fd input_;
    Fd output_;
};
