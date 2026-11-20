#include "Window/LibGui.h"

#include "Utils/Logger.h"
#include "Utils/System.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

constexpr int32_t FormatRgba8888 = 1;
constexpr uint32_t FlagSkipScreenshot = 0x40;
constexpr uint32_t FlagSecure = 0x80;
constexpr uint32_t FlagContainer = 0x4000;
constexpr uint32_t FlagEffect = 0x20000;
constexpr int32_t WindowTypeDontScreenshot = 441'731;
constexpr uint32_t MetadataWindowType = 2;
constexpr int32_t TrustedOverlayEnabled = 2;
constexpr size_t ObjectSize = 4096;
constexpr size_t GuardSize = 32;
constexpr unsigned char GuardByte = 0xA5;

// Room for a C++ object of the system's, whose real size is not known here.
template <size_t N>
struct Storage {
    alignas(16) unsigned char bytes[N] = {};

    GuiPtr Get() {
        return bytes;
    }
};

template <class Construct>
Result<GuiPtr> LibGui::ConstructInPlace(const char* what, Construct construct) {
    void* object = nullptr;
    if (posix_memalign(&object, 16, ObjectSize + GuardSize) != 0 || !object) {
        return Err("out of memory");
    }
    std::memset(object, 0, ObjectSize);
    auto* guard = static_cast<unsigned char*>(object) + ObjectSize;
    std::memset(guard, GuardByte, GuardSize);
    construct(object);
    for (size_t index = 0; index < GuardSize; index++) {
        if (guard[index] != GuardByte) {
            return Err("{} is larger than expected on this system", what);
        }
    }
    return object;
}

void LibGui::StartBinderThreads() {
    const auto binder = Library::Open("libbinder_ndk.so");
    if (!binder) {
        return;
    }
    if (const auto start = binder->Func<void (*)()>("ABinderProcess_startThreadPool")) {
        start();
    }
}

const LibGui::CreateVariant LibGui::CreateVariants[] = {
    {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEiRKNS4_INS_7IBinderEEERKNS_3gui13LayerMetadataEPj", CreateKind::CheckedMetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEiRKNS4_INS_7IBinderEEENS_3gui13LayerMetadataEPj", CreateKind::CheckedMetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjRKNS4_INS_7IBinderEEENS_13LayerMetadataEPj", CreateKind::CheckedMetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_NS_13LayerMetadataEPj", CreateKind::CheckedMetadataHint, Parent::Control}, {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_NS_13LayerMetadataE", CreateKind::CheckedMetadata}, {"_ZN7android21SurfaceComposerClient20createSurfaceCheckedERKNS_7String8EjjiPNS_2spINS_14SurfaceControlEEEjPS5_ii", CreateKind::CheckedWindowType}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEERKNS_3gui13LayerMetadataEPj", CreateKind::MetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjiiRKNS_2spINS_7IBinderEEENS_3gui13LayerMetadataEPj", CreateKind::MetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijRKNS_2spINS_7IBinderEEENS_13LayerMetadataEPj", CreateKind::MetadataHint, Parent::Handle}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataEPj", CreateKind::MetadataHint, Parent::Control}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlENS_13LayerMetadataE", CreateKind::Metadata}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEii", CreateKind::WindowType, Parent::Control, -1}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8EjjijPNS_14SurfaceControlEjj", CreateKind::WindowType, Parent::Control, 0}, {"_ZN7android21SurfaceComposerClient13createSurfaceERKNS_7String8Ejjij", CreateKind::Plain},
};

// Looks symbols up and remembers which of several spellings was there.
class LibGui::Resolver {
public:
    Resolver(const Library& library, std::vector<SymbolPick>& picks) : library_(library), picks_(picks) {
    }

    void* Find(const char* symbol) const {
        return library_.Find(symbol);
    }
    void Note(const char* what, const char* symbol) {
        picks_.push_back({what, symbol});
    }

    template <class F>
    bool Optional(F& target, const char* what, std::initializer_list<const char*> symbols) {
        for (const char* symbol : symbols) {
            if (void* address = library_.Find(symbol)) {
                target = reinterpret_cast<F>(address);
                Note(what, symbol);
                return true;
            }
        }
        return false;
    }

    template <class F>
    Result<> Required(F& target, const char* what, std::initializer_list<const char*> symbols) {
        if (Optional(target, what, symbols)) {
            return {};
        }
        return Err("{} is missing from the system library", what);
    }

private:
    const Library& library_;
    std::vector<SymbolPick>& picks_;
};

GuiHandle GuiHandle::Adopt(GuiPtr object) {
    return GuiHandle(object);
}

GuiHandle GuiHandle::Share(const LibGui& gui, GuiPtr object) {
    gui.Retain(object);
    return GuiHandle(object);
}

GuiHandle::GuiHandle(const GuiHandle& other) {
    const auto gui = LibGui::Get();
    if (gui && other.object_) {
        (*gui)->Retain(other.object_);
        object_ = other.object_;
    }
}

GuiHandle::~GuiHandle() {
    if (!object_) {
        return;
    }
    if (const auto gui = LibGui::Get()) {
        (*gui)->Release(object_);
    }
}

Result<const LibGui*> LibGui::Get() {
    // Never taken down: a window may be let go of while the program is ending.
    static const Result<LibGui>& instance = *new Result<LibGui>(Load());
    if (!instance) {
        return std::unexpected(instance.error());
    }
    return &*instance;
}

Result<LibGui> LibGui::Load() {
    LibGui gui;
    gui.api_ = System::GetApiLevel();
    const Library utilsLibrary = TRY(Library::Open("libutils.so"));
    const Library guiLibrary = TRY(Library::Open("libgui.so"));
    Resolver utils(utilsLibrary, gui.picks_);
    Resolver lib(guiLibrary, gui.picks_);

    TRY(utils.Required(gui.retain_, "RefBase::incStrong", {"_ZNK7android7RefBase9incStrongEPKv"}));
    TRY(utils.Required(gui.release_, "RefBase::decStrong", {"_ZNK7android7RefBase9decStrongEPKv"}));
    TRY(utils.Required(gui.string8New_, "String8::String8", {"_ZN7android7String8C1EPKc"}));
    TRY(utils.Required(gui.string8Drop_, "String8::~String8", {"_ZN7android7String8D1Ev"}));

    lib.Optional(gui.clientDefault_, "SurfaceComposerClient::getDefault", {"_ZN7android21SurfaceComposerClient10getDefaultEv"});
    lib.Optional(gui.clientNew_, "SurfaceComposerClient::SurfaceComposerClient", {"_ZN7android21SurfaceComposerClientC1Ev"});
    if (!gui.clientDefault_ && !gui.clientNew_) {
        return Err("SurfaceComposerClient cannot be constructed on this system");
    }
    lib.Optional(gui.clientCheck_, "SurfaceComposerClient::initCheck", {"_ZNK7android21SurfaceComposerClient9initCheckEv"});

    for (const CreateVariant& variant : CreateVariants) {
        if (void* address = lib.Find(variant.symbol)) {
            gui.create_ = address;
            gui.createVariant_ = variant;
            lib.Note("SurfaceComposerClient::createSurface", variant.symbol);
            break;
        }
    }
    if (!gui.create_) {
        return Err("no known SurfaceComposerClient::createSurface variant (API {})", gui.api_);
    }

    lib.Optional(gui.metadataNew_, "LayerMetadata::LayerMetadata", {"_ZN7android3gui13LayerMetadataC1Ev", "_ZN7android13LayerMetadataC1Ev"});
    lib.Optional(gui.metadataSet_, "LayerMetadata::setInt32", {"_ZN7android3gui13LayerMetadata8setInt32Eji", "_ZN7android13LayerMetadata8setInt32Eji"});
    const CreateKind kind = gui.createVariant_.kind;
    const bool needsMetadata = kind == CreateKind::Metadata || kind == CreateKind::MetadataHint || kind == CreateKind::CheckedMetadata || kind == CreateKind::CheckedMetadataHint;
    if (needsMetadata && !gui.metadataNew_) {
        return Err("LayerMetadata constructor is missing (API {})", gui.api_);
    }

    lib.Optional(gui.validate_, "SurfaceControl::validate", {"_ZNK7android14SurfaceControl8validateEv"});
    TRY(lib.Required(gui.surface_, "SurfaceControl::getSurface", {"_ZN7android14SurfaceControl10getSurfaceEv", "_ZNK7android14SurfaceControl10getSurfaceEv"}));
    lib.Optional(gui.disconnect_, "SurfaceControl::disconnect", {"_ZN7android14SurfaceControl10disconnectEv"});
    lib.Optional(gui.updateSize_, "SurfaceControl::updateDefaultBufferSize", {"_ZN7android14SurfaceControl23updateDefaultBufferSizeEjj"});

    TRY(gui.ResolveToken(lib));
    gui.stateIsModern_ = lib.Optional(gui.state_, "SurfaceComposerClient::getDisplayState", {"_ZN7android21SurfaceComposerClient15getDisplayStateERKNS_2spINS_7IBinderEEEPNS_2ui12DisplayStateE"});
    if (!gui.stateIsModern_) {
        TRY(lib.Required(gui.state_, "SurfaceComposerClient::getDisplayInfo", {"_ZN7android21SurfaceComposerClient14getDisplayInfoERKNS_2spINS_7IBinderEEEPNS_11DisplayInfoE"}));
    }
    TRY(gui.ResolveOps(lib));
    if (gui.transact_) {
        gui.ResolveMirroring(lib);
    }
    return gui;
}

Result<> LibGui::ResolveToken(Resolver& gui) {
    gui.Optional(token_.ids, "SurfaceComposerClient::getPhysicalDisplayIds", {"_ZN7android21SurfaceComposerClient21getPhysicalDisplayIdsEv"});
    if (gui.Optional(token_.internal, "SurfaceComposerClient::getInternalDisplayToken", {"_ZN7android21SurfaceComposerClient23getInternalDisplayTokenEv"})) {
        return {};
    }
    if (token_.ids && gui.Optional(token_.physical, "SurfaceComposerClient::getPhysicalDisplayToken", {"_ZN7android21SurfaceComposerClient23getPhysicalDisplayTokenENS_17PhysicalDisplayIdE"})) {
        return {};
    }
    return gui.Required(token_.builtIn, "SurfaceComposerClient::getBuiltInDisplay", {"_ZN7android21SurfaceComposerClient17getBuiltInDisplayEi"});
}

void LibGui::ResolveMirroring(Resolver& gui) {
    Mirroring found;
    if (!gui.Optional(found.copy, "SurfaceComposerClient::mirrorSurface", {"_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_14SurfaceControlE"}) && !gui.Optional(found.copyCropped, "SurfaceComposerClient::mirrorSurface", {"_ZN7android21SurfaceComposerClient13mirrorSurfaceEPNS_14SurfaceControlES2_S2_"})) {
        return;
    }
    const bool complete = gui.Optional(found.setStack, "Transaction::setLayerStack", {"_ZN7android21SurfaceComposerClient11Transaction13setLayerStackERKNS_2spINS_14SurfaceControlEEENS_2ui10LayerStackE"}) && gui.Optional(found.setMatrix, "Transaction::setMatrix", {"_ZN7android21SurfaceComposerClient11Transaction9setMatrixERKNS_2spINS_14SurfaceControlEEEffff"}) && gui.Optional(found.setPosition, "Transaction::setPosition", {"_ZN7android21SurfaceComposerClient11Transaction11setPositionERKNS_2spINS_14SurfaceControlEEEff"});
    if (complete) {
        mirroring_ = found;
    }
}

Result<> LibGui::ResolveOps(Resolver& gui) {
    Transact transact;
    if (!gui.Optional(transact.setLayer, "Transaction::setLayer", {"_ZN7android21SurfaceComposerClient11Transaction8setLayerERKNS_2spINS_14SurfaceControlEEEi"})) {
        Global global;
        TRY(gui.Required(global.open, "SurfaceComposerClient::openGlobalTransaction", {"_ZN7android21SurfaceComposerClient21openGlobalTransactionEv"}));
        TRY(gui.Required(global.close, "SurfaceComposerClient::closeGlobalTransaction", {"_ZN7android21SurfaceComposerClient22closeGlobalTransactionEb"}));
        TRY(gui.Required(global.setLayer, "SurfaceControl::setLayer", {"_ZN7android14SurfaceControl8setLayerEi", "_ZN7android14SurfaceControl8setLayerEj"}));
        TRY(gui.Required(global.show, "SurfaceControl::show", {"_ZN7android14SurfaceControl4showEv"}));
        TRY(gui.Required(global.hide, "SurfaceControl::hide", {"_ZN7android14SurfaceControl4hideEv"}));
        gui.Optional(global.setSize, "SurfaceControl::setSize", {"_ZN7android14SurfaceControl7setSizeEjj"});
        global_ = global;
        return {};
    }

    const auto android = Library::Open("libandroid.so");
    if (android) {
        transact.createNdk = android->Func<GuiPtr (*)()>("ASurfaceTransaction_create");
    }
    if (!transact.createNdk && !gui.Optional(transact.construct, "Transaction::Transaction", {"_ZN7android21SurfaceComposerClient11TransactionC1Ev"})) {
        TRY(gui.Required(transact.constructCopy, "Transaction::Transaction(const Transaction&)", {"_ZN7android21SurfaceComposerClient11TransactionC1ERKS1_"}));
    }
    if (!gui.Optional(transact.applyOneWay, "Transaction::apply", {"_ZN7android21SurfaceComposerClient11Transaction5applyEbb"})) {
        TRY(gui.Required(transact.apply, "Transaction::apply", {"_ZN7android21SurfaceComposerClient11Transaction5applyEb"}));
    }
    if (!gui.Optional(transact.trustedFlag, "Transaction::setTrustedOverlay", {"_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEEb"})) {
        gui.Optional(transact.trustedMode, "Transaction::setTrustedOverlay", {"_ZN7android21SurfaceComposerClient11Transaction17setTrustedOverlayERKNS_2spINS_14SurfaceControlEEENS_3gui14TrustedOverlayE"});
    }
    TRY(gui.Required(transact.show, "Transaction::show", {"_ZN7android21SurfaceComposerClient11Transaction4showERKNS_2spINS_14SurfaceControlEEE"}));
    TRY(gui.Required(transact.hide, "Transaction::hide", {"_ZN7android21SurfaceComposerClient11Transaction4hideERKNS_2spINS_14SurfaceControlEEE"}));
    gui.Optional(transact.setSize, "Transaction::setSize", {"_ZN7android21SurfaceComposerClient11Transaction7setSizeERKNS_2spINS_14SurfaceControlEEEjj"});
    gui.Optional(transact.reparent, "Transaction::reparent", {"_ZN7android21SurfaceComposerClient11Transaction8reparentERKNS_2spINS_14SurfaceControlEEES6_"});
    gui.Optional(transact.setColor, "Transaction::setColor", {"_ZN7android21SurfaceComposerClient11Transaction8setColorERKNS_2spINS_14SurfaceControlEEERKNS_7details5TVec3INS_4halfEEE"});
    gui.Optional(transact.setDesiredPresentTime, "Transaction::setDesiredPresentTime", {"_ZN7android21SurfaceComposerClient11Transaction21setDesiredPresentTimeEl", "_ZN7android21SurfaceComposerClient11Transaction21setDesiredPresentTimeEx"});
    gui.Optional(transact.setBlurRegions, "Transaction::setBlurRegions", {"_ZN7android21SurfaceComposerClient11Transaction14setBlurRegionsERKNS_2spINS_14SurfaceControlEEERKNSt3__16vectorINS_10BlurRegionENS7_9allocatorIS9_EEEE"});
    transact_ = transact;
    return {};
}

void LibGui::Retain(GuiPtr object) const {
    retain_(object, object);
}

void LibGui::Release(GuiPtr object) const {
    release_(object, object);
}

Result<GuiPtr> LibGui::Connect() const {
    StartBinderThreads();
    GuiPtr client = clientDefault_ ? clientDefault_().value : nullptr;
    if (!client) {
        if (!clientNew_) {
            return Err("SurfaceComposerClient::getDefault returned null");
        }
        client = TRY(ConstructInPlace("SurfaceComposerClient", clientNew_));
        Retain(client);
    }
    if (clientCheck_) {
        if (const int32_t status = clientCheck_(client); status != 0) {
            return Err("SurfaceFlinger connection failed (status {})", status);
        }
    }
    return client;
}

Result<std::pair<GuiPtr, std::optional<uint64_t>>> LibGui::GetDisplayToken() const {
    // The first id of the `std::vector<PhysicalDisplayId>` the composer returns: the main display.
    const auto mainDisplay = [this]() -> std::optional<uint64_t> {
        if (!token_.ids) {
            return std::nullopt;
        }
        const std::array<GuiPtr, 3> ids = token_.ids().value;
        std::optional<uint64_t> id;
        if (ids[0] && ids[0] != ids[1]) {
            uint64_t first = 0;
            std::memcpy(&first, ids[0], sizeof first);
            id = first;
        }
        std::free(ids[0]);
        return id;
    };

    GuiPtr token = nullptr;
    std::optional<uint64_t> id;
    if (token_.internal) {
        token = token_.internal().value;
        id = mainDisplay();
    } else if (token_.physical) {
        id = mainDisplay();
        if (!id) {
            return Err("SurfaceFlinger reported no physical displays");
        }
        token = token_.physical(*id).value;
    } else {
        token = token_.builtIn(0).value;
    }
    if (!token) {
        return Err("SurfaceFlinger returned no display token");
    }
    return std::pair{token, id};
}

Result<DisplayState> LibGui::GetDisplayState(GuiPtr token) const {
    alignas(8) uint8_t data[512] = {};
    const auto read = [&data](size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, data + offset, sizeof value);
        return value;
    };

    if (stateIsModern_) {
        if (const int32_t status = state_(&token, data); status != 0) {
            return Err("getDisplayState failed (status {})", status);
        }
        return DisplayState{read(8), read(12), static_cast<uint8_t>(read(4) & 3), std::nullopt};
    }
    if (const int32_t status = state_(&token, data); status != 0) {
        return Err("getDisplayInfo failed (status {})", status);
    }
    const uint8_t rotation = data[24] & 3;
    uint32_t width = read(0), height = read(4);
    if (rotation & 1) {
        std::swap(width, height);
    }
    float rate = 0;
    std::memcpy(&rate, data + 16, sizeof rate);
    return DisplayState{width, height, rotation, std::isfinite(rate) && rate >= 1.0f ? std::optional(rate) : std::nullopt};
}

Result<GuiPtr> LibGui::NewTransaction() const {
    if (!transact_) {
        return nullptr;
    }
    if (transact_->createNdk) {
        GuiPtr transaction = transact_->createNdk();
        if (!transaction) {
            return Err("ASurfaceTransaction_create returned null");
        }
        return transaction;
    }
    if (transact_->construct) {
        return ConstructInPlace("Transaction", transact_->construct);
    }
    return ConstructInPlace("Transaction", [this](GuiPtr object) { transact_->constructCopy(object, object); });
}

Result<GuiPtr> LibGui::CreateSurface(GuiPtr client, const char* name, uint32_t width, uint32_t height, Capture capture, LayerKind layerKind) const {
    const CreateKind kind = createVariant_.kind;
    const Parent parentKind = createVariant_.parent;
    const bool hidden = capture == Capture::Hidden;
    uint32_t flags = (capture == Capture::Secure ? FlagSecure : 0) | (layerKind == LayerKind::Container ? FlagContainer : layerKind == LayerKind::Effect ? FlagEffect : 0);
    Storage<512> metadata;
    Storage<64> name8;

    const bool hinted = kind == CreateKind::MetadataHint || kind == CreateKind::CheckedMetadataHint;
    const bool byMetadata = kind == CreateKind::Metadata || kind == CreateKind::CheckedMetadata || (hinted && parentKind == Parent::Control);
    const bool byFlag = hinted && parentKind == Parent::Handle;

    if (metadataNew_) {
        metadataNew_(metadata.Get());
    }
    if (hidden && byMetadata) {
        if (metadataSet_) {
            metadataSet_(metadata.Get(), MetadataWindowType, WindowTypeDontScreenshot);
        } else {
            logger.Warn("capture hiding is unavailable: LayerMetadata::setInt32 is missing");
        }
    }
    if (hidden && byFlag) {
        flags |= FlagSkipScreenshot;
    }
    // Android 8.0 already takes the window type that hides a layer, and does nothing with it.
    if (hidden && api_ < 27) {
        logger.Warn("a layer cannot be kept out of captures before Android 8.1");
    }

    string8New_(name8.Get(), name);
    const GuiPtr title = name8.Get();
    GuiPtr noHandle = nullptr;
    const GuiPtr parent = parentKind == Parent::Control ? nullptr : static_cast<GuiPtr>(&noHandle);
    GuiPtr control = nullptr;
    GuiPtr* const out = &control;
    const GuiPtr meta = metadata.Get();
    const int32_t fallback = createVariant_.windowType;
    const int32_t windowType = hidden ? WindowTypeDontScreenshot : fallback;
    const int32_t format = FormatRgba8888;

    int32_t status = 0;
    switch (kind) {
        case CreateKind::Plain:
            control = reinterpret_cast<Returned<GuiPtr> (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t)>(create_)(client, title, width, height, format, flags).value;
            break;
        case CreateKind::WindowType:
            control = reinterpret_cast<Returned<GuiPtr> (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, int32_t, int32_t)>(create_)(client, title, width, height, format, flags, nullptr, windowType, fallback).value;
            break;
        case CreateKind::Metadata:
            control = reinterpret_cast<Returned<GuiPtr> (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, GuiPtr)>(create_)(client, title, width, height, format, flags, nullptr, meta).value;
            break;
        case CreateKind::MetadataHint:
            control = reinterpret_cast<Returned<GuiPtr> (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, uint32_t, GuiPtr, GuiPtr, uint32_t*)>(create_)(client, title, width, height, format, flags, parent, meta, nullptr).value;
            break;
        case CreateKind::CheckedWindowType:
            status = reinterpret_cast<int32_t (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, int32_t, int32_t)>(create_)(client, title, width, height, format, out, flags, nullptr, hidden ? WindowTypeDontScreenshot : -1, -1);
            break;
        case CreateKind::CheckedMetadata:
            status = reinterpret_cast<int32_t (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, GuiPtr)>(create_)(client, title, width, height, format, out, flags, nullptr, meta);
            break;
        case CreateKind::CheckedMetadataHint:
            status = reinterpret_cast<int32_t (*)(GuiPtr, GuiPtr, uint32_t, uint32_t, int32_t, GuiPtr*, uint32_t, GuiPtr, GuiPtr, uint32_t*)>(create_)(client, title, width, height, format, out, flags, parent, meta, nullptr);
            break;
    }
    string8Drop_(title);

    if (status != 0 || !control) {
        return Err("SurfaceFlinger refused to create a {}x{} layer (status {})", width, height, status);
    }
    if (validate_) {
        if (const int32_t valid = validate_(control); valid != 0) {
            Release(control);
            return Err("created layer is not valid (status {})", valid);
        }
    }
    return control;
}

Result<GuiPtr> LibGui::GetSurface(GuiPtr control) const {
    const GuiPtr surface = surface_(control).value;
    if (!surface) {
        return Err("SurfaceControl::getSurface returned null");
    }
    return surface;
}

void LibGui::Disconnect(GuiPtr control) const {
    if (disconnect_) {
        disconnect_(control);
    }
}

Result<> LibGui::Present(GuiPtr transaction, GuiPtr control, int32_t z) const {
    if (transact_) {
        transact_->setLayer(transaction, &control, z);
        if (transact_->trustedFlag) {
            transact_->trustedFlag(transaction, &control, true);
        } else if (transact_->trustedMode) {
            transact_->trustedMode(transaction, &control, TrustedOverlayEnabled);
        }
        transact_->show(transaction, &control);
        return Apply(*transact_, transaction);
    }
    global_->open();
    global_->setLayer(control, z);
    global_->show(control);
    global_->close(false);
    return {};
}

Result<> LibGui::SetVisible(GuiPtr transaction, GuiPtr control, bool visible) const {
    if (transact_) {
        (visible ? transact_->show : transact_->hide)(transaction, &control);
        return Apply(*transact_, transaction);
    }
    global_->open();
    (visible ? global_->show : global_->hide)(control);
    global_->close(false);
    return {};
}

Result<std::array<GuiHandle, 2>> LibGui::Mirror(GuiPtr client, GuiPtr transaction, GuiPtr layer, std::array<uint32_t, 2> size, uint32_t stack) const {
    if (!CanMirror()) {
        return Err("layers cannot be mirrored on this system");
    }
    const GuiPtr copied = mirroring_->copy ? mirroring_->copy(client, layer).value : mirroring_->copyCropped(client, layer, nullptr, nullptr).value;
    if (!copied) {
        return Err("SurfaceFlinger refused to mirror the layer");
    }
    GuiHandle copy = GuiHandle::Adopt(copied);
    GuiHandle root = GuiHandle::Adopt(TRY(CreateSurface(client, "android-imgui mirror", size[0], size[1], Capture::Visible, LayerKind::Container)));
    const GuiPtr inner = copy.Get(), outer = root.Get();
    transact_->setLayer(transaction, &outer, INT32_MAX);
    mirroring_->setStack(transaction, &outer, stack);
    transact_->show(transaction, &outer);
    mirroring_->setStack(transaction, &inner, stack);
    transact_->show(transaction, &inner);
    transact_->reparent(transaction, &inner, &outer);
    TRY(Apply(*transact_, transaction));
    return std::array{std::move(copy), std::move(root)};
}

Result<> LibGui::Place(GuiPtr transaction, GuiPtr layer, float scale, std::array<float, 2> offset) const {
    if (!CanMirror()) {
        return Err("layers cannot be placed on this system");
    }
    mirroring_->setMatrix(transaction, &layer, scale, 0.0f, 0.0f, scale);
    mirroring_->setPosition(transaction, &layer, offset[0], offset[1]);
    return Apply(*transact_, transaction);
}

Result<GuiHandle> LibGui::CreateBlurLayer(GuiPtr client, GuiPtr transaction, GuiPtr parent, Capture capture) const {
    if (!CanBlur()) {
        return Err("this system does not blur what is under a layer");
    }
    // Android 12 takes such a layer only without a size.
    GuiHandle made = GuiHandle::Adopt(TRY(CreateSurface(client, "android-imgui blur", 0, 0, capture, LayerKind::Effect)));
    const GuiPtr layer = made.Get();
    transact_->reparent(transaction, &layer, &parent);
    // Under the layer inside the same parent that has the window's picture, and without the
    // colour such a layer is otherwise filled with: minus one, three times over, is no colour.
    const std::array<uint16_t, 3> none = {0xbc00, 0xbc00, 0xbc00};
    transact_->setLayer(transaction, &layer, -1);
    transact_->setColor(transaction, &layer, &none);
    transact_->show(transaction, &layer);
    TRY(Apply(*transact_, transaction));
    return made;
}

Result<> LibGui::SetBlur(GuiPtr transaction, GuiPtr control, std::span<const BlurRegion> regions, std::optional<std::chrono::steady_clock::time_point> shownAt) const {
    if (!CanBlur()) {
        return Err("this system does not blur what is under a layer");
    }
    // The call copies the vector it is given. A vector of the system's library is three
    // pointers - first, one past the last, end of storage - and that is what it gets.
    const std::array<const BlurRegion*, 3> list = {regions.data(), regions.data() + regions.size(), regions.data() + regions.size()};
    transact_->setBlurRegions(transaction, &control, &list);
    if (shownAt && transact_->setDesiredPresentTime) {
        transact_->setDesiredPresentTime(transaction, std::chrono::duration_cast<std::chrono::nanoseconds>(shownAt->time_since_epoch()).count());
    }
    return Apply(*transact_, transaction);
}

Result<bool> LibGui::Resize(GuiPtr transaction, GuiPtr control, uint32_t width, uint32_t height) const {
    if (updateSize_) {
        updateSize_(control, width, height);
        return true;
    }
    if (transact_) {
        if (!transact_->setSize) {
            return false;
        }
        transact_->setSize(transaction, &control, width, height);
        TRY(Apply(*transact_, transaction, true));
        return true;
    }
    if (!global_->setSize) {
        return false;
    }
    global_->open();
    global_->setSize(control, width, height);
    global_->close(true);
    return true;
}

// With `wait`, returns only after SurfaceFlinger has applied the transaction.
Result<> LibGui::Apply(const Transact& transact, GuiPtr transaction, bool wait) const {
    const int32_t status = transact.applyOneWay ? transact.applyOneWay(transaction, wait, false) : transact.apply(transaction, wait);
    if (status != 0) {
        return Err("SurfaceFlinger rejected the transaction (status {})", status);
    }
    return {};
}
