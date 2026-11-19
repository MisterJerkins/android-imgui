#include "Utils/System.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <paths.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

Result<Library> Library::Open(const char* name) {
    void* handle = dlopen(name, RTLD_NOW);
    if (!handle) {
        const char* message = dlerror();
        return Err("dlopen \"{}\" failed: {}", name, message ? message : "unknown error");
    }
    return Library(handle);
}

void* Library::Find(const char* symbol) const {
    return dlsym(handle_, symbol);
}

std::optional<std::string> System::GetProperty(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    const int length = __system_property_get(name, value);
    if (length <= 0) {
        return std::nullopt;
    }
    return std::string(value, std::min<size_t>(static_cast<size_t>(length), PROP_VALUE_MAX));
}

std::optional<double> System::GetPropertyNumber(const char* name) {
    const auto text = GetProperty(name);
    if (!text) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text->c_str(), &end);
    if (end == text->c_str()) {
        return std::nullopt;
    }
    return value;
}

uint32_t System::GetApiLevel() {
    return static_cast<uint32_t>(GetPropertyNumber("ro.build.version.sdk").value_or(0));
}

float System::GetDensity() {
    const double dpi = GetPropertyNumber("ro.sf.lcd_density").or_else([] { return GetPropertyNumber("qemu.sf.lcd_density"); }).value_or(160.0);
    return std::clamp(static_cast<float>(dpi / 160.0), 0.5f, 8.0f);
}

std::string System::RunCommand(const char* command) {
    std::string output;
    if (FILE* pipe = popen(command, "r")) {
        char chunk[4096];
        for (size_t read = 0; (read = fread(chunk, 1, sizeof chunk, pipe)) > 0;) {
            output.append(chunk, read);
        }
        pclose(pipe);
    }
    return output;
}

std::optional<DirectoryWatch> DirectoryWatch::Create(std::span<const char* const> directories) {
    Fd events(inotify_init1(IN_CLOEXEC));
    if (!events) {
        return std::nullopt;
    }
    // Every directory is tried: one that cannot be watched does not stop the others.
    size_t watched = 0;
    for (const char* directory : directories) {
        if (inotify_add_watch(events.Get(), directory, IN_CREATE | IN_DELETE | IN_MOVED_TO) >= 0) {
            watched++;
        }
    }
    if (watched == 0) {
        return std::nullopt;
    }
    return DirectoryWatch(std::move(events));
}

bool DirectoryWatch::Wait() {
    char events[4096];
    return read(events_.Get(), events, sizeof events) > 0;
}

extern char** environ;

struct ChildProcess::Pipe {
    Fd read;
    Fd write;

    static Result<Pipe> Make() {
        int ends[2];
        if (pipe2(ends, O_CLOEXEC) != 0) {
            return Err("cannot make a pipe: {}", std::strerror(errno));
        }
        return Pipe{Fd(ends[0]), Fd(ends[1])};
    }
};

Result<std::string> ChildProcess::FindProgram(const std::string& name) {
    if (name.contains('/')) {
        return name;
    }
    const char* path = std::getenv("PATH");
    for (std::string_view rest = path ? path : _PATH_DEFPATH; !rest.empty();) {
        const size_t end = rest.find(':');
        const std::string_view folder = rest.substr(0, end);
        std::string file = std::string(folder.empty() ? "." : folder) + "/" + name;
        if (access(file.c_str(), X_OK) == 0) {
            return file;
        }
        if (end == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(end + 1);
    }
    return Err("{} is not on PATH", name);
}

Result<ChildProcess> ChildProcess::Spawn(const Start& start) {
    // Writing to a child that has gone must fail, not end this process.
    static std::once_flag once;
    std::call_once(once, [] { signal(SIGPIPE, SIG_IGN); });
    if (start.command.empty()) {
        return Err("no command to run");
    }
    const std::string program = TRY(FindProgram(start.command[0]));
    Pipe input = TRY(Pipe::Make());
    Pipe output = TRY(Pipe::Make());

    // Everything the child needs is laid out before the fork: between it and the exec only
    // system calls are safe in a process with other threads.
    std::vector<std::string> variables;
    for (char** variable = environ; variable && *variable; variable++) {
        const std::string_view text(*variable);
        const std::string_view name = text.substr(0, text.find('='));
        const bool replaced = std::ranges::any_of(start.environment, [&](const auto& set) { return set.first == name; });
        if (!replaced) {
            variables.emplace_back(text);
        }
    }
    for (const auto& [name, value] : start.environment) {
        variables.push_back(name + "=" + value);
    }
    std::vector<char*> arguments, environment;
    for (const std::string& argument : start.command) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    for (const std::string& variable : variables) {
        environment.push_back(const_cast<char*>(variable.c_str()));
    }
    arguments.push_back(nullptr);
    environment.push_back(nullptr);

    const pid_t process = fork();
    if (process < 0) {
        return Err("cannot start {}: {}", start.command[0], std::strerror(errno));
    }
    if (process == 0) {
        signal(SIGPIPE, SIG_DFL);
        const int nothing = open("/dev/null", O_WRONLY);
        if (dup2(input.read.Get(), STDIN_FILENO) < 0 || dup2(output.write.Get(), STDOUT_FILENO) < 0 || (nothing >= 0 && dup2(nothing, STDERR_FILENO) < 0)) {
            _exit(126);
        }
        if (start.user && (setgid(*start.user) != 0 || setuid(*start.user) != 0)) {
            _exit(126);
        }
        execve(program.c_str(), arguments.data(), environment.data());
        _exit(127);
    }
    return ChildProcess(process, std::move(input.write), std::move(output.read));
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept : process_(std::exchange(other.process_, -1)), input_(std::move(other.input_)), output_(std::move(other.output_)) {
}

ChildProcess::~ChildProcess() {
    if (process_ < 0) {
        return;
    }
    Kill();
    int status = 0;
    while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {
    }
}

bool ChildProcess::WriteLine(std::string_view line) {
    if (!input_) {
        return false;
    }
    std::string text(line);
    text.push_back('\n');
    for (size_t sent = 0; sent < text.size();) {
        const ssize_t wrote = write(input_.Get(), text.data() + sent, text.size() - sent);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
        sent += static_cast<size_t>(wrote);
    }
    return true;
}

void ChildProcess::CloseInput() {
    input_.Reset();
}

Fd ChildProcess::TakeOutput() {
    return std::move(output_);
}

bool ChildProcess::IsRunning() {
    if (process_ < 0) {
        return false;
    }
    int status = 0;
    const pid_t ended = waitpid(process_, &status, WNOHANG);
    if (ended == 0) {
        return true;
    }
    process_ = -1;
    return false;
}

void ChildProcess::Kill() {
    if (process_ >= 0) {
        ::kill(process_, SIGKILL);
    }
}
