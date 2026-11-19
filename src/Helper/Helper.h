#pragma once

#include "Input/Keyboard/Keyboard.h"
#include "Input/Touch/TouchParser.h"
#include "Utils/Utils.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// A small Java program run by `app_process`, for the few things only Android's framework
// can do: the system clipboard, touches where no touchscreen can be read, and telling the
// system the user is there. It is carried inside the binary, started when first needed, and
// done without when it does not come up.

// Whom a helper runs as, which decides what Android lets it do.
enum class HelperUser {
    Root,
    // The one user outside an app that the clipboard answers.
    Shell,
};

// A helper process, started when it is first asked for something.
class Helper final : public Clipboard {
public:
    using Touches = std::function<void(const Motion&)>;

    explicit Helper(HelperUser user);
    Helper(Helper&&) noexcept;
    Helper& operator=(Helper&&) noexcept;
    ~Helper() override;

    // Starts a helper now that reports every touch on the screen to `touches`, and waits for
    // the system to let it.
    static Result<Helper> WatchTouches(Touches touches);

    // Tells the system the user is active, which keeps the screen from timing out.
    void Activity();

    // Lets go of a helper that has not been needed for a while. It comes back when asked again.
    void Rest() override;

    void Prepare() override;
    std::optional<std::string> Read() override;
    void Write(std::string_view text) override;

private:
    struct Process;

    Process* GetProcess();
    bool Send(std::string_view command);

    HelperUser user_;
    std::unique_ptr<Process> process_;
    bool failed_ = false;
    bool watching_ = false;
};
