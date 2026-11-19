#pragma once

#include "Utils/Utils.h"
#include "Window/ANativeWindowCreator.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using GuiPtr = void*;

class LibGui;

// A strong reference to one of the system's reference-counted objects, given up when dropped.
class GuiHandle {
public:
    GuiHandle() = default;
    // Takes over a reference the caller already holds.
    static GuiHandle Adopt(GuiPtr object);
    // Takes a reference of its own.
    static GuiHandle Share(const LibGui& gui, GuiPtr object);

    GuiHandle(const GuiHandle& other);
    GuiHandle(GuiHandle&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {
    }
    GuiHandle& operator=(GuiHandle other) noexcept {
        std::swap(object_, other.object_);
        return *this;
    }
    ~GuiHandle();

    GuiPtr Get() const {
        return object_;
    }

private:
    explicit GuiHandle(GuiPtr object) : object_(object) {
    }

    GuiPtr object_ = nullptr;
};

struct DisplayState {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t rotation = 0;
    std::optional<float> refreshRate;
};

// Which symbol of the system library was taken for a job, for diagnostics.
struct SymbolPick {
    const char* what;
    const char* symbol;
};

// The private C++ interface of SurfaceFlinger's client library. Every call is picked by which
// symbol the library actually has, not by the Android version, so an unusual system gives an
// error instead of a crash.
class LibGui {
public:
    // The library, loaded once. The error is the same every time where it cannot be used.
    static Result<const LibGui*> Get();

    uint32_t GetApi() const {
        return api_;
    }
    std::span<const SymbolPick> GetPicks() const {
        return picks_;
    }

    void Retain(GuiPtr object) const;
    void Release(GuiPtr object) const;

    Result<GuiPtr> Connect() const;
    // The main display's token and, where the system has such a thing, its physical id.
    Result<std::pair<GuiPtr, std::optional<uint64_t>>> GetDisplayToken() const;
    Result<DisplayState> GetDisplayState(GuiPtr token) const;
    Result<GuiPtr> NewTransaction() const;

    // Creates a layer with buffers to draw into, or a bare container for other layers.
    Result<GuiPtr> CreateSurface(GuiPtr client, const char* name, uint32_t width, uint32_t height, Capture capture, bool container) const;
    // The layer's `android::Surface`, with a reference of the caller's own.
    Result<GuiPtr> GetSurface(GuiPtr control) const;
    void Disconnect(GuiPtr control) const;

    Result<> Present(GuiPtr transaction, GuiPtr control, int32_t z) const;
    Result<> SetVisible(GuiPtr transaction, GuiPtr control, bool visible) const;
    // Returns once SurfaceFlinger has taken the new size: a window surface made before that
    // would still be given the old one by some EGL implementations. `false` where this
    // Android version cannot resize a layer in place.
    Result<bool> Resize(GuiPtr transaction, GuiPtr control, uint32_t width, uint32_t height) const;

    bool CanMirror() const {
        return mirroring_.has_value() && transact_.has_value();
    }
    // Shows a copy of `layer` on the display with the given layer stack, inside a container
    // that `place` can scale and move. Returns the copy and its container.
    Result<std::array<GuiHandle, 2>> Mirror(GuiPtr client, GuiPtr transaction, GuiPtr layer, std::array<uint32_t, 2> size, uint32_t stack) const;
    // Scales a layer and moves it within its display.
    Result<> Place(GuiPtr transaction, GuiPtr layer, float scale, std::array<float, 2> offset) const;

private:
    // Non-trivial C++ values come back through a hidden pointer. A struct this large is
    // returned the same way on every Android ABI, so the callee sees the pointer it expects.
    template <class T>
    struct Returned {
        T value;
        size_t pad[3];
    };

    using RetainCall = void (*)(const void*, const void*);
    using Method = void (*)(GuiPtr);
    using Status = int32_t (*)(GuiPtr);
    using GetShared = Returned<GuiPtr> (*)();
    using Shared = Returned<GuiPtr> (*)(GuiPtr);
    using TxControl = GuiPtr (*)(GuiPtr, const GuiPtr*);
    using TxInt = GuiPtr (*)(GuiPtr, const GuiPtr*, int32_t);
    using TxSize = GuiPtr (*)(GuiPtr, const GuiPtr*, uint32_t, uint32_t);
    using TxBool = GuiPtr (*)(GuiPtr, const GuiPtr*, bool);
    using DisplayIds = Returned<std::array<GuiPtr, 3>> (*)();
    using Query = int32_t (*)(const GuiPtr*, uint8_t*);

    enum class Parent { Control, Handle };
    enum class CreateKind { Plain, WindowType, Metadata, MetadataHint, CheckedWindowType, CheckedMetadata, CheckedMetadataHint };

    struct CreateVariant {
        const char* symbol;
        CreateKind kind;
        Parent parent = Parent::Control;
        int32_t windowType = 0;
    };
    static const CreateVariant CreateVariants[];

    // Makes an object of the system's in memory of our own. Its size is a guess, so a guard after
    // it tells when the guess was too small.
    template <class Construct>
    static Result<GuiPtr> ConstructInPlace(const char* what, Construct construct);

    static void StartBinderThreads();

    // How the main display is found: one of three ways, by what the library offers.
    struct Token {
        GetShared internal = nullptr;
        DisplayIds ids = nullptr;
        Returned<GuiPtr> (*physical)(uint64_t) = nullptr;
        Returned<GuiPtr> (*builtIn)(int32_t) = nullptr;
    };

    // WindowLayer changes through transaction objects (Android 9 and later).
    struct Transact {
        GuiPtr (*createNdk)() = nullptr;
        Method construct = nullptr;
        void (*constructCopy)(GuiPtr, GuiPtr) = nullptr;
        TxInt setLayer = nullptr;
        TxControl show = nullptr;
        TxControl hide = nullptr;
        TxSize setSize = nullptr;
        TxBool trustedFlag = nullptr;
        TxInt trustedMode = nullptr;
        int32_t (*apply)(GuiPtr, bool) = nullptr;
        int32_t (*applyOneWay)(GuiPtr, bool, bool) = nullptr;
    };

    // WindowLayer changes through the one global transaction (before Android 9).
    struct Global {
        void (*open)() = nullptr;
        void (*close)(bool) = nullptr;
        int32_t (*setLayer)(GuiPtr, int32_t) = nullptr;
        Status show = nullptr;
        Status hide = nullptr;
        int32_t (*setSize)(GuiPtr, uint32_t, uint32_t) = nullptr;
    };

    // What it takes to show a copy of a layer on another display.
    struct Mirroring {
        Returned<GuiPtr> (*copy)(GuiPtr, GuiPtr) = nullptr;
        Returned<GuiPtr> (*copyCropped)(GuiPtr, GuiPtr, GuiPtr, GuiPtr) = nullptr;
        GuiPtr (*setStack)(GuiPtr, const GuiPtr*, uint32_t) = nullptr;
        GuiPtr (*reparent)(GuiPtr, const GuiPtr*, const GuiPtr*) = nullptr;
        GuiPtr (*setMatrix)(GuiPtr, const GuiPtr*, float, float, float, float) = nullptr;
        GuiPtr (*setPosition)(GuiPtr, const GuiPtr*, float, float) = nullptr;
    };

    class Resolver;

    LibGui() = default;
    static Result<LibGui> Load();
    Result<> ResolveToken(Resolver& gui);
    Result<> ResolveOps(Resolver& gui);
    void ResolveMirroring(Resolver& gui);
    Result<> Apply(const Transact& transact, GuiPtr transaction, bool wait = false) const;

    uint32_t api_ = 0;
    std::vector<SymbolPick> picks_;
    RetainCall retain_ = nullptr;
    RetainCall release_ = nullptr;
    void (*string8New_)(GuiPtr, const char*) = nullptr;
    Method string8Drop_ = nullptr;
    GetShared clientDefault_ = nullptr;
    Method clientNew_ = nullptr;
    Status clientCheck_ = nullptr;
    void* create_ = nullptr;
    CreateVariant createVariant_{};
    Method metadataNew_ = nullptr;
    void (*metadataSet_)(GuiPtr, uint32_t, int32_t) = nullptr;
    Status validate_ = nullptr;
    Shared surface_ = nullptr;
    Method disconnect_ = nullptr;
    void (*updateSize_)(GuiPtr, uint32_t, uint32_t) = nullptr;
    Token token_;
    Query state_ = nullptr;
    bool stateIsModern_ = false;
    std::optional<Transact> transact_;
    std::optional<Global> global_;
    std::optional<Mirroring> mirroring_;
};
