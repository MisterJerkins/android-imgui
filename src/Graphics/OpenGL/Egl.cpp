#include "Graphics/OpenGL/Egl.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <format>
#include <string>
#include <utility>

std::string Egl::Error() {
    return std::format("EGL error 0x{:04x}", eglGetError());
}

Result<Egl> Egl::Create(ANativeWindow* window) {
    Egl egl;
    egl.display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display_ == EGL_NO_DISPLAY || !eglInitialize(egl.display_, nullptr, nullptr)) {
        return Err("EGL is unavailable ({})", Error());
    }

    for (const auto [version, bit] : {std::pair{3, 0x0040 /* EGL_OPENGL_ES3_BIT */}, std::pair{2, EGL_OPENGL_ES2_BIT}}) {
        const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, bit, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (!eglChooseConfig(egl.display_, attributes, &config, 1, &count) || count == 0) {
            continue;
        }
        const EGLint wanted[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
        if (EGLContext context = eglCreateContext(egl.display_, config, EGL_NO_CONTEXT, wanted); context != EGL_NO_CONTEXT) {
            egl.version_ = version;
            egl.config_ = config;
            egl.context_ = context;
            break;
        }
    }
    if (!egl.context_) {
        return Err("no OpenGL ES context could be created ({})", Error());
    }

    egl.surface_ = eglCreateWindowSurface(egl.display_, egl.config_, window, nullptr);
    if (egl.surface_ == EGL_NO_SURFACE || !eglMakeCurrent(egl.display_, egl.surface_, egl.surface_, egl.context_)) {
        return Err("EGL window surface could not be created ({})", Error());
    }
    eglSwapInterval(egl.display_, 1);
    // Starting up leaves an error behind on some drivers, which the renderer would report as
    // its own.
    // A driver keeps at most a handful of errors waiting to be read.
    for (int tries = 0; tries < 16 && glGetError() != GL_NO_ERROR; tries++) {
    }
    return egl;
}

Egl::Egl(Egl&& other) noexcept : display_(other.display_), config_(other.config_), context_(std::exchange(other.context_, nullptr)), surface_(std::exchange(other.surface_, nullptr)), version_(other.version_) {
}

Egl::~Egl() {
    if (!display_ || (!context_ && !surface_)) {
        return;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface_) {
        eglDestroySurface(display_, surface_);
    }
    if (context_) {
        eglDestroyContext(display_, context_);
    }
}

Result<> Egl::Rebind(ANativeWindow* window) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display_, surface_);
    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE || !eglMakeCurrent(display_, surface_, surface_, context_)) {
        return Err("EGL window surface could not be recreated ({})", Error());
    }
    eglSwapInterval(display_, 1);
    return {};
}

Result<> Egl::Swap() const {
    if (!eglSwapBuffers(display_, surface_)) {
        return Err("eglSwapBuffers failed ({})", Error());
    }
    return {};
}
