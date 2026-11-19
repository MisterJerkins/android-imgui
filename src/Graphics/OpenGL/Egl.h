#pragma once

#include "Utils/Utils.h"

#include <string>

struct ANativeWindow;

// An OpenGL ES context drawing into a window: ES 3 where the device has it, ES 2 otherwise.
class Egl {
public:
    static Result<Egl> Create(ANativeWindow* window);

    Egl(Egl&& other) noexcept;
    Egl& operator=(Egl&&) = delete;
    ~Egl();

    int GetVersion() const {
        return version_;
    }

    // Moves the context to another window.
    Result<> Rebind(ANativeWindow* window);
    Result<> Swap() const;

private:
    Egl() = default;

    static std::string Error();

    void* display_ = nullptr;
    void* config_ = nullptr;
    void* context_ = nullptr;
    void* surface_ = nullptr;
    int version_ = 0;
};
