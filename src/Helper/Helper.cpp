#include "Helper/Helper.h"

#include "Utils/Logger.h"
#include "Utils/System.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Empty in a build without the helper, which then never starts one.
constexpr unsigned char Dex[] = {
#ifdef ANDROID_IMGUI_HELPER
#include "Helper/Helper.dex.inc"
#else
    0
#endif
};
#ifdef ANDROID_IMGUI_HELPER
constexpr bool Carried = true;
#else
constexpr bool Carried = false;
#endif
constexpr std::string_view Watching = "watching";

// Lines from the helper's listener to whoever waits for them.
class Channel {
public:
    void Send(std::string line) {
        {
            const std::scoped_lock lock(mutex_);
            lines_.push_back(std::move(line));
        }
        ready_.notify_one();
    }

    std::optional<std::string> Receive(Clock::time_point until) {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_until(lock, until, [this] { return !lines_.empty(); })) {
            return std::nullopt;
        }
        std::string line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
};

struct Shared {
    Channel clips;
    Channel status;
    std::atomic<bool> up{false};
};

struct Helper::Process {
    ChildProcess child;
    std::string dex;
    std::shared_ptr<Shared> shared;
    std::jthread listener;
    Clock::time_point started = Clock::now();
    Clock::time_point used = started;
    uint32_t asked = 0;
    bool answered = false;

    // Reads the helper's lines until it closes its output.
    static void Listen(Fd replies, std::string dex, Touches touches, std::shared_ptr<Shared> shared) {
        pthread_setname_np(pthread_self(), "helper");
        std::string pending;
        bool first = true, settled = false;
        char chunk[4096];
        const auto take = [&](std::string_view line) {
            if (line.starts_with("clip ")) {
                shared->clips.Send(std::string(line.substr(5)));
            } else if (const auto motion = touches ? Motion::Parse(line) : std::nullopt) {
                touches(*motion);
            } else if (line == Watching || line.starts_with("error")) {
                // Whether touches are being watched is settled by the first of these two.
                if (!std::exchange(settled, true)) {
                    shared->status.Send(std::string(line));
                }
                if (line != Watching) {
                    logger.Warn("helper: {}", line);
                }
            } else {
                logger.Debug("helper: {}", line);
            }
        };
        for (;;) {
            const ssize_t got = ::read(replies.Get(), chunk, sizeof chunk);
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            pending.append(chunk, static_cast<size_t>(got));
            for (size_t end = 0; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                if (std::exchange(first, false)) {
                    // The file is only needed until the helper has loaded it, which its first line shows.
                    unlink(dex.c_str());
                    shared->up.store(true, std::memory_order_relaxed);
                }
                std::string_view line(pending.data(), end);
                if (line.ends_with('\r')) {
                    line.remove_suffix(1);
                }
                take(line);
            }
        }
        if (first) {
            unlink(dex.c_str());
        }
    }

    static Result<std::unique_ptr<Process>> Start(HelperUser user, Touches touches) {
        if (!Carried) {
            return Err("this build carries no helper");
        }
        std::string dex = std::format("/data/local/tmp/.android-imgui-{}-{}.dex", getpid(), std::to_underlying(user));
        {
            const Fd file(open(dex.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
            if (!file || ::write(file.Get(), Dex, sizeof Dex) != static_cast<ssize_t>(sizeof Dex) || fchmod(file.Get(), 0644) != 0) {
                return Err("cannot unpack the helper: {}", std::strerror(errno));
            }
        }
        ChildProcess::Start start{.command = {"app_process", "/system/bin", "imgui.Helper"}, .environment = {{"CLASSPATH", dex}}, .user = std::nullopt};
        if (touches) {
            start.command.emplace_back("touches");
        }
        if (user == HelperUser::Shell) {
            // The user id of the shell.
            start.user = 2000;
        }
        auto child = ChildProcess::Spawn(start);
        if (!child) {
            unlink(dex.c_str());
            return Err("cannot start the helper: {}", child.error().message);
        }
        auto shared = std::make_shared<Shared>();
        std::jthread listener(Listen, child->TakeOutput(), dex, std::move(touches), shared);
        return std::unique_ptr<Process>(new Process{std::move(*child), std::move(dex), std::move(shared), std::move(listener)});
    }

    // Until when to wait for the helper: it is slow only while it starts.
    Clock::time_point Patience() const {
        // A helper may take five seconds to come up and one to answer once it has. Both are far
        // more than it needs: on a busy system a paste that waits is better than one that brings
        // the wrong text.
        return std::max(started + 5s, Clock::now() + 1s);
    }

    // The helper leaves by itself once its input closes, after doing what it was still asked to.
    ~Process() {
        child.CloseInput();
        const Clock::time_point deadline = Patience();
        while (Clock::now() < deadline && child.IsRunning()) {
            std::this_thread::sleep_for(5ms);
        }
        child.Kill();
        if (listener.joinable()) {
            listener.join();
        }
        // Long gone if the helper came up; one that did not has left it behind.
        unlink(dex.c_str());
    }
};

Helper::Helper(HelperUser user) : user_(user) {
}
Helper::Helper(Helper&&) noexcept = default;
Helper& Helper::operator=(Helper&&) noexcept = default;
Helper::~Helper() = default;

Result<Helper> Helper::WatchTouches(Touches touches) {
    Helper helper(HelperUser::Root);
    helper.process_ = TRY(Process::Start(HelperUser::Root, std::move(touches)));
    // Where touches come from nowhere else, the helper is given longer to say it has them.
    const auto status = helper.process_->shared->status.Receive(Clock::now() + 10s);
    if (!status) {
        return Err("the helper did not come up");
    }
    if (*status != Watching) {
        return Err("{}", *status);
    }
    helper.watching_ = true;
    return helper;
}

void Helper::Activity() {
    Send("activity");
}

void Helper::Rest() {
    // A helper nobody has asked anything for this long is let go; it holds tens of megabytes.
    if (watching_ || !process_ || Clock::now() - process_->used < 30s) {
        return;
    }
    // Its leaving takes a moment that the caller, drawing frames, should not wait out.
    std::thread([leaving = std::move(process_)]() mutable {
        pthread_setname_np(pthread_self(), "helper");
        leaving.reset();
    }).detach();
}

Helper::Process* Helper::GetProcess() {
    if (!process_ && !failed_) {
        auto started = Process::Start(user_, nullptr);
        if (started) {
            process_ = std::move(*started);
        } else {
            logger.Warn("{}", started.error().message);
            failed_ = true;
        }
    }
    return process_.get();
}

// A helper that went away is started again by the next command, unless it never came up.
bool Helper::Send(std::string_view command) {
    Process* running = GetProcess();
    if (!running) {
        return false;
    }
    const bool sent = running->child.WriteLine(command);
    running->used = Clock::now();
    if (!sent) {
        failed_ = !running->shared->up.load(std::memory_order_relaxed);
        process_.reset();
    }
    return sent;
}

void Helper::Prepare() {
    if (Process* running = GetProcess()) {
        running->used = Clock::now();
    }
}

std::optional<std::string> Helper::Read() {
    Process* running = GetProcess();
    if (!running) {
        return std::nullopt;
    }
    running->asked++;
    const uint32_t asked = running->asked;
    const std::string request = std::format("paste {}", asked);
    // A helper found dead is started again at once, so the paste need not be repeated.
    if (!Send(request) && !Send(request)) {
        return std::nullopt;
    }
    running = process_.get();
    if (!running) {
        return std::nullopt;
    }
    const Clock::time_point deadline = running->Patience();
    for (;;) {
        const auto reply = running->shared->clips.Receive(deadline);
        if (!reply) {
            // One that has answered before is only slow this once; one that never has is not asked again.
            if (!running->answered) {
                logger.Warn("the helper does not answer, the system clipboard stays out of reach");
                running->child.Kill();
                process_.reset();
                failed_ = true;
            }
            return std::nullopt;
        }
        const size_t space = reply->find(' ');
        const std::string_view number = std::string_view(*reply).substr(0, space);
        if (number != std::to_string(asked)) {
            continue;
        }
        running->answered = true;
        if (space == std::string::npos) {
            return std::nullopt;
        }
        return Utils::FromHex(std::string_view(*reply).substr(space + 1));
    }
}

void Helper::Write(std::string_view text) {
    // Text travels as hexadecimal UTF-8, which keeps line breaks out of the line protocol.
    Send(std::format("copy {}", Utils::ToHex(text)));
}
