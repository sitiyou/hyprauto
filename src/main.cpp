#include "HyprlandCompat.hpp"
#include "Capture.hpp"
#include "Socket.hpp"
#include <src/Compositor.hpp>
#include <src/debug/log/Logger.hpp>
#include <map>
#include <algorithm>
#include <src/protocols/core/Seat.hpp>
#include <src/protocols/CursorShape.hpp>
#include <src/helpers/CursorShapes.hpp>
#include <src/protocols/core/Compositor.hpp>
#include <src/managers/SeatManager.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/desktop/state/ViewState.hpp>
#include <src/desktop/view/WLSurface.hpp>
#include <src/output/Monitor.hpp>
#include <hyprutils/math/Box.hpp>
#include <src/desktop/state/ViewQuery.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/helpers/time/Time.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <nlohmann/json.hpp>
#include <linux/input-event-codes.h>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <format>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <tuple>
#include <utility>

using Hyprutils::Utils::CScopeGuard;

static HANDLE                        handle             = nullptr;
static bool                          automationDispatch = false;
static std::vector<CFunctionHook*>   hooks;
static SP<Hyprauto::Compat::Command> command;
static CHyprSignalListener           windowClose;
static CFunctionHook*                keymapHook       = nullptr;
static CFunctionHook*                repeatHook       = nullptr;
static CFunctionHook*                setCursorHook    = nullptr;
static CFunctionHook*                pointerEnterHook = nullptr;
static CFunctionHook*                shapePointerHook = nullptr;
static CFunctionHook*                setShapeHook     = nullptr;
struct ShapePointer {
    wl_listener            destroy;
    wl_resource*           resource;
    WP<CWLPointerResource> pointer;

    ~ShapePointer() {
        wl_list_remove(&destroy.link);
    }
};
static_assert(std::is_standard_layout_v<ShapePointer>);
static std::map<wl_resource*, std::unique_ptr<ShapePointer>> shapePointers;
static CHyprSignalListener                                   hostCursorListener;
static CHyprSignalListener                                   pointerFocusListener;
static CHyprSignalListener                                   cursorShapeListener;
static uint64_t                                              hostCursorUpdates = 0;

template <typename Resource>
struct InputBinding {
    WP<Resource>           resource;
    bool                   hostEntered  = false;
    bool                   automated    = false;
    uint32_t               cursorSerial = 0;
    WP<CWLSurfaceResource> cursorSurface;

    SP<Resource>           lock() const {
        return resource.lock();
    }
};

struct Session {
    uint64_t                                       id = 0;
    std::unique_ptr<Hyprauto::Capture::Job>        capture;
    wl_client*                                     client = nullptr;
    PHLWINDOWREF                                   window;
    WP<CWLSurfaceResource>                         surface;
    std::vector<InputBinding<CWLKeyboardResource>> keyboards;
    std::vector<InputBinding<CWLPointerResource>>  pointers;
    std::set<uint32_t>                             keys, buttons;
    bool                                           blockInput = false;
    Vector2D                                       position   = {1, 1};
    xkb_state*                                     xkb        = nullptr;
    std::string                                    keymap;
    Hyprutils::OS::CFileDescriptor                 keymapFD;
    CHyprSignalListener                            surfaceDestroy;
    wl_event_source*                               clickTimer = nullptr;
    Hyprauto::Socket::Reply                         clickReply;
    std::string                                    clickRelease;
    std::chrono::steady_clock::time_point           clickDeadline;
};

static std::map<uint64_t, std::unique_ptr<Session>> sessions;
static std::unique_ptr<Hyprauto::Socket>            server;

static Session*                                     clientSession(wl_client* client) {
    for (const auto& [id, session] : sessions)
        if (session->client == client)
            return session.get();
    return nullptr;
}

static uint32_t now() {
    return static_cast<uint32_t>(Time::millis(Time::steadyNow()));
}

static CFunctionHook* installHook(const std::string& name, void* destination) {
    auto matches = HyprlandAPI::findFunctionsByName(handle, name.substr(name.rfind("::") + 2));
    std::erase_if(matches, [&](const auto& match) { return !match.demangled.starts_with(name + "("); });
    if (matches.size() != 1)
        throw std::runtime_error("Expected exactly one hook symbol: " + name);
    auto hook = HyprlandAPI::createFunctionHook(handle, matches.front().address, destination);
    if (!hook || !hook->hook())
        throw std::runtime_error("Cannot hook " + name);
    hooks.push_back(hook);
    return hook;
}

static void refreshResources(Session& session);

static void enterPointer(CWLPointerResource* pointer, SP<CWLSurfaceResource> surface, const Vector2D& local, bool repeat = false) {
    if (!repeat && pointer->m_currentSurface == surface)
        return;
    auto old = pointer->m_currentSurface.lock();
    if (old && old != surface && old->getResource()->resource())
        pointer->m_resource->sendLeave(g_pSeatManager->nextSerial(pointer->m_owner.lock()), old->getResource().get());
    pointer->m_currentSurface.reset();
    pointer->m_listeners.destroySurface.reset();
    pointer->sendEnter(surface, local);
}

static void enterKeyboard(CWLKeyboardResource* keyboard, SP<CWLSurfaceResource> surface, wl_array* keys, bool repeat = false) {
    if (!repeat && keyboard->m_currentSurface == surface)
        return;
    auto old = keyboard->m_currentSurface.lock();
    if (old && old != surface && old->getResource()->resource())
        keyboard->m_resource->sendLeave(g_pSeatManager->nextSerial(keyboard->m_owner.lock()), old->getResource().get());
    keyboard->m_currentSurface.reset();
    keyboard->m_listeners.destroySurface.reset();
    keyboard->sendEnter(surface, keys);
}

template <typename Resource>
static auto& bindings(Session& session) {
    if constexpr (std::is_same_v<Resource, CWLKeyboardResource>)
        return session.keyboards;
    else
        return session.pointers;
}

template <typename Resource>
static InputBinding<Resource>* bindingFor(Session& session, Resource* resource) {
    for (auto& binding : bindings<Resource>(session))
        if (binding.lock().get() == resource)
            return &binding;
    return nullptr;
}

static Vector2D hostPointerPosition(SP<CWLSurfaceResource> surface) {
    auto desktopSurface = Desktop::View::CWLSurface::fromResource(surface);
    auto box            = desktopSurface ? desktopSurface->getSurfaceBoxGlobal() : std::nullopt;
    return box ? g_pInputManager->getMouseCoordsInternal() - box->pos() : g_pSeatManager->m_lastLocalCoords;
}

static std::vector<uint32_t> enteredKeys(const Session& session, bool includeHost, bool includeAutomation = true) {
    std::set<uint32_t> keys = includeAutomation ? session.keys : std::set<uint32_t>{};
    if (includeHost) {
        auto hostKeys = g_pInputManager->getKeysFromAllKBs();
        keys.insert(hostKeys.begin(), hostKeys.end());
    }
    return {keys.begin(), keys.end()};
}

template <int ID, typename Resource, typename... Args>
struct Gate {
    static inline CFunctionHook* hook       = nullptr;
    static constexpr bool        frameEvent = ID == 8;

    static void                  call(Resource* self, Args... args) {
        if (!automationDispatch && self->m_owner) {
            if (auto session = clientSession(self->m_owner->client())) {
                refreshResources(*session);
                auto       binding   = bindingFor(*session, self);
                const auto hostFocus = (std::is_same_v<Resource, CWLKeyboardResource> ? g_pSeatManager->m_state.keyboardFocus : g_pSeatManager->m_state.pointerFocus).lock();
                if constexpr (ID == 1 || ID == 5) {
                    if (binding)
                        binding->hostEntered = false;
                    return;
                } else if constexpr (ID == 0 || ID == 4) {
                    if (!binding) {
                        reinterpret_cast<void (*)(Resource*, Args...)>(hook->m_original)(self, args...);
                        return;
                    }
                    const bool repeat           = !binding->hostEntered;
                    binding->hostEntered        = true;
                    const bool previousDispatch = automationDispatch;
                    automationDispatch          = true;
                    CScopeGuard restore([&] { automationDispatch = previousDispatch; });
                    if constexpr (ID == 0) {
                        auto     keys = enteredKeys(*session, !session->blockInput, std::get<0>(std::tuple(args...)) == session->surface);
                        wl_array array{keys.size() * sizeof(uint32_t), keys.size() * sizeof(uint32_t), keys.data()};
                        enterKeyboard(self, std::get<0>(std::tuple(args...)), &array, repeat);
                    } else
                        enterPointer(self, args..., repeat);
                    return;
                } else {
                    if (session->blockInput || (!frameEvent && (!hostFocus || hostFocus->client() != session->client)))
                        return;
                    if constexpr (!frameEvent) {
                        const bool previousDispatch = automationDispatch;
                        automationDispatch          = true;
                        CScopeGuard restore([&] { automationDispatch = previousDispatch; });
                        if constexpr (std::is_same_v<Resource, CWLKeyboardResource>) {
                            auto     keys = enteredKeys(*session, true, hostFocus == session->surface);
                            wl_array array{keys.size() * sizeof(uint32_t), keys.size() * sizeof(uint32_t), keys.data()};
                            enterKeyboard(self, hostFocus, &array);
                        } else
                            enterPointer(self, hostFocus, hostPointerPosition(hostFocus));
                    }
                }
            }
        }
        reinterpret_cast<void (*)(Resource*, Args...)>(hook->m_original)(self, args...);
    }

    static void install(const std::string& name) {
        hook = installHook(name, reinterpret_cast<void*>(&call));
    }
};

static void sendModifiers(Session& session) {
    for (const auto& weak : session.keyboards) {
        if (auto keyboard = weak.lock())
            keyboard->sendMods(xkb_state_serialize_mods(session.xkb, XKB_STATE_MODS_DEPRESSED), xkb_state_serialize_mods(session.xkb, XKB_STATE_MODS_LATCHED),
                               xkb_state_serialize_mods(session.xkb, XKB_STATE_MODS_LOCKED), xkb_state_serialize_layout(session.xkb, XKB_STATE_LAYOUT_EFFECTIVE));
    }
}

static std::vector<SP<CWLSeatResource>> clientSeats(wl_client* client) {
    std::vector<SP<CWLSeatResource>> seats;
    wl_client_for_each_resource(
        client,
        [](wl_resource* resource, void* data) {
            if (std::string_view(wl_resource_get_class(resource)) == "wl_seat") {
                if (auto seat = CWLSeatResource::fromResource(resource))
                    static_cast<std::vector<SP<CWLSeatResource>>*>(data)->push_back(seat);
            }
            return WL_ITERATOR_CONTINUE;
        },
        &seats);
    return seats;
}

static void refreshResources(Session& session) {
    std::erase_if(session.keyboards, [](const auto& binding) { return !binding.lock(); });
    std::erase_if(session.pointers, [](const auto& binding) { return !binding.lock(); });
    for (const auto& seat : clientSeats(session.client)) {
        for (const auto& weak : seat->m_keyboards) {
            if (auto keyboard = weak.lock(); keyboard && !bindingFor(session, keyboard.get()))
                session.keyboards.push_back({keyboard, keyboard->m_currentSurface && keyboard->m_currentSurface == g_pSeatManager->m_state.keyboardFocus});
        }
        for (const auto& weak : seat->m_pointers) {
            if (auto pointer = weak.lock(); pointer && !bindingFor(session, pointer.get()))
                session.pointers.push_back({pointer, pointer->m_currentSurface && pointer->m_currentSurface == g_pSeatManager->m_state.pointerFocus});
        }
    }
}

static void ensureKeyboard(Session& session) {
    auto     keys = enteredKeys(session, !session.blockInput && g_pSeatManager->m_state.keyboardFocus == session.surface);
    wl_array array{keys.size() * sizeof(uint32_t), keys.size() * sizeof(uint32_t), keys.data()};
    for (auto& binding : session.keyboards) {
        if (auto keyboard = binding.lock()) {
            if (keyboard->m_lastKeymap != session.keymap) {
                keyboard->m_lastKeymap = session.keymap;
                keyboard->m_resource->sendKeymap(WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, session.keymapFD.get(), session.keymap.size() + 1);
            }
            enterKeyboard(keyboard.get(), session.surface.lock(), &array, !binding.automated && !session.keys.empty());
            binding.automated = true;
        }
    }
    sendModifiers(session);
}

static void ensurePointer(Session& session) {
    ensureKeyboard(session);
    for (auto& binding : session.pointers) {
        if (auto pointer = binding.lock()) {
            enterPointer(pointer.get(), session.surface.lock(), session.position);
            for (auto button : session.buttons)
                if (std::ranges::find(pointer->m_pressedButtons, button) == pointer->m_pressedButtons.end())
                    pointer->sendButton(now(), button, WL_POINTER_BUTTON_STATE_PRESSED);
            binding.automated = true;
        }
    }
}

template <typename Callback>
static void forEachHostKeyboard(Callback callback) {
    wl_client* client = nullptr;
    wl_client_for_each(client, wl_display_get_client_list(g_pCompositor->m_wlDisplay)) {
        if (clientSession(client))
            continue;
        for (const auto& seat : clientSeats(client))
            for (const auto& weak : seat->m_keyboards)
                if (auto keyboard = weak.lock())
                    callback(keyboard);
    }
}

static void updateHostKeymaps(CWLSeatProtocol* self) {
    if (std::ranges::none_of(sessions, [](const auto& entry) { return entry.second->client; })) {
        reinterpret_cast<void (*)(CWLSeatProtocol*)>(keymapHook->m_original)(self);
        return;
    }
    forEachHostKeyboard([](const auto& keyboard) { keyboard->sendKeymap(g_pSeatManager->m_keyboard.lock()); });
}

static void updateHostRepeatInfo(CWLSeatProtocol* self, uint32_t rate, uint32_t delay) {
    if (std::ranges::none_of(sessions, [](const auto& entry) { return entry.second->client; })) {
        reinterpret_cast<void (*)(CWLSeatProtocol*, uint32_t, uint32_t)>(repeatHook->m_original)(self, rate, delay);
        return;
    }
    forEachHostKeyboard([&](const auto& keyboard) { keyboard->repeatInfo(rate, delay); });
}

static void clearTarget(Session& session, bool restoreHostFocus = true) {
    session.capture->cancel();
    if (session.clickTimer) {
        wl_event_source_remove(session.clickTimer);
        session.clickTimer = nullptr;
    }
    auto clickReply = std::exchange(session.clickReply, {});
    if (clickReply)
        clickReply("error: click cancelled: target cleared");
    if (!session.client)
        return;
    const bool previousDispatch = automationDispatch;
    automationDispatch          = true;
    CScopeGuard restore([&] { automationDispatch = previousDispatch; });
    session.surfaceDestroy.reset();
    refreshResources(session);
    for (const auto& weak : session.keyboards) {
        if (auto keyboard = weak.lock()) {
            if (weak.automated) {
                if (session.surface) {
                    auto     keys = enteredKeys(session, false);
                    wl_array array{keys.size() * sizeof(uint32_t), keys.size() * sizeof(uint32_t), keys.data()};
                    enterKeyboard(keyboard.get(), session.surface.lock(), &array);
                }
                for (auto key : session.keys)
                    keyboard->sendKey(now(), key, WL_KEYBOARD_KEY_STATE_RELEASED);
                keyboard->sendMods(0, 0, 0, 0);
                keyboard->sendLeave();
            }
            if (auto nativeKeyboard = g_pSeatManager->m_keyboard.lock()) {
                keyboard->sendKeymap(nativeKeyboard);
                keyboard->repeatInfo(nativeKeyboard->m_repeatRate, nativeKeyboard->m_repeatDelay);
            }
        }
    }
    for (const auto& weak : session.pointers) {
        if (auto pointer = weak.lock(); pointer && weak.automated) {
            if (session.surface)
                enterPointer(pointer.get(), session.surface.lock(), session.position);
            for (auto button : session.buttons)
                if (std::ranges::find(pointer->m_pressedButtons, button) != pointer->m_pressedButtons.end())
                    pointer->sendButton(now(), button, WL_POINTER_BUTTON_STATE_RELEASED);
            pointer->sendLeave();
            pointer->sendFrame();
        }
    }
    auto keyboardFocus = g_pSeatManager->m_state.keyboardFocus.lock();
    if (restoreHostFocus && keyboardFocus && keyboardFocus->client() == session.client && g_pSeatManager->m_keyboard) {
        auto     pressed = g_pInputManager->getKeysFromAllKBs();
        wl_array keys{pressed.size() * sizeof(uint32_t), pressed.size() * sizeof(uint32_t), pressed.data()};
        for (const auto& weak : session.keyboards) {
            if (auto keyboard = weak.lock(); keyboard && weak.automated) {
                keyboard->sendEnter(keyboardFocus, &keys);
                const auto& mods = g_pSeatManager->m_keyboard->m_modifiersState;
                keyboard->sendMods(mods.depressed, mods.latched, mods.locked, mods.group);
            }
        }
    }
    auto pointerFocus = g_pSeatManager->m_state.pointerFocus.lock();
    if (restoreHostFocus && pointerFocus && pointerFocus->client() == session.client && session.window) {
        auto local = hostPointerPosition(pointerFocus);
        for (const auto& weak : session.pointers) {
            if (auto pointer = weak.lock(); pointer && weak.automated) {
                pointer->sendEnter(pointerFocus, local);
                pointer->sendFrame();
            }
        }
    }
    xkb_state_unref(session.xkb);
    auto       capture = std::move(session.capture);
    const auto id      = session.id;
    session            = {};
    session.id         = id;
    session.capture    = std::move(capture);
}

static uint32_t surfaceID(const WP<CWLSurfaceResource>& surface);

static void     recordPointerEnter(CWlPointer* self, uint32_t serial, CWlSurface* surface, wl_fixed_t x, wl_fixed_t y) {
    if (auto pointer = CWLPointerResource::fromResource(self->resource())) {
        if (auto session = clientSession(wl_resource_get_client(self->resource()))) {
            refreshResources(*session);
            if (auto binding = bindingFor(*session, pointer.get())) {
                binding->cursorSerial  = serial;
                binding->cursorSurface = CWLSurfaceResource::fromResource(surface->resource());
                Log::logger->log(Log::DEBUG, "[hyprauto] pointer enter: session {}, serial {}, surface {}, compositor pointer focus surface {}", session->id, serial,
                                 surfaceID(binding->cursorSurface), surfaceID(g_pSeatManager->m_state.pointerFocus));
            }
        }
    }
    reinterpret_cast<void (*)(CWlPointer*, uint32_t, CWlSurface*, wl_fixed_t, wl_fixed_t)>(pointerEnterHook->m_original)(self, serial, surface, x, y);
}

static bool cursorAllowed(Session& session, uint32_t serial, CWLPointerResource* requestedPointer = nullptr) {
    auto focus = g_pSeatManager->m_state.pointerFocus.lock();
    if (!focus || focus->client() != session.client)
        return false;
    refreshResources(session);
    for (const auto& binding : session.pointers) {
        auto pointer = binding.lock();
        if (!pointer || (requestedPointer && pointer.get() != requestedPointer) || pointer->m_currentSurface != focus)
            continue;
        if (!binding.cursorSerial || (binding.cursorSerial == serial && binding.cursorSurface == focus))
            return true;
    }
    return false;
}

static void recordShapePointer(CCursorShapeProtocol* self, CWpCursorShapeManagerV1* manager, uint32_t id, wl_resource* pointer) {
    reinterpret_cast<void (*)(CCursorShapeProtocol*, CWpCursorShapeManagerV1*, uint32_t, wl_resource*)>(shapePointerHook->m_original)(self, manager, id, pointer);
    if (std::string_view(wl_resource_get_class(pointer)) != "wl_pointer")
        return;
    if (auto device = wl_client_get_object(manager->client(), id)) {
        auto binding            = std::make_unique<ShapePointer>();
        binding->resource       = device;
        binding->pointer        = CWLPointerResource::fromResource(pointer);
        binding->destroy.notify = [](wl_listener* listener, void*) {
            auto binding = reinterpret_cast<ShapePointer*>(listener);
            shapePointers.erase(binding->resource);
        };
        wl_resource_add_destroy_listener(device, &binding->destroy);
        shapePointers[device] = std::move(binding);
    }
}

static void traceSetShape(CCursorShapeProtocol* self, CWpCursorShapeDeviceV1* device, uint32_t serial, wpCursorShapeDeviceV1Shape shape) {
    auto       session = clientSession(device->client());
    auto       it      = shapePointers.find(device->resource());
    auto       pointer = it == shapePointers.end() ? nullptr : it->second->pointer.lock();
    const bool allowed = !session || ((it == shapePointers.end() || pointer) && cursorAllowed(*session, serial, pointer.get()));
    Log::logger->log(Log::DEBUG, "[hyprauto] cursor-shape request: serial {}, shape {}, request session {}, session accepts {}", serial, static_cast<uint32_t>(shape),
                     session ? std::to_string(session->id) : "none", allowed);
    if (!allowed && static_cast<uint32_t>(shape) > 0 && static_cast<uint32_t>(shape) < CURSOR_SHAPE_NAMES.size())
        return;
    reinterpret_cast<void (*)(CCursorShapeProtocol*, CWpCursorShapeDeviceV1*, uint32_t, wpCursorShapeDeviceV1Shape)>(setShapeHook->m_original)(self, device, serial, shape);
}

static void traceSetCursor(CSeatManager* self, SP<CWLSeatResource> seat, uint32_t serial, SP<CWLSurfaceResource> cursorSurface, const Vector2D& hotspot) {
    auto       focusResource   = self->m_state.pointerFocusResource.lock();
    auto       focusSurface    = self->m_state.pointerFocus.lock();
    auto       requestSession  = seat ? clientSession(seat->client()) : nullptr;
    auto       focusSession    = focusSurface ? clientSession(focusSurface->client()) : nullptr;
    const bool acceptedByFocus = seat && focusResource && seat->client() == focusResource->client();
    const bool allowed         = !requestSession || cursorAllowed(*requestSession, serial);
    Log::logger->log(Log::DEBUG,
                     "[hyprauto] set_cursor request: serial {}, cursor surface {}, request session {}, compositor pointer focus surface {}, focus session {}, focus accepts {}, "
                     "session accepts {}",
                     serial, surfaceID(cursorSurface), requestSession ? std::to_string(requestSession->id) : "none", surfaceID(focusSurface),
                     focusSession ? std::to_string(focusSession->id) : "none", acceptedByFocus, allowed);
    if (!allowed)
        return;
    reinterpret_cast<void (*)(CSeatManager*, SP<CWLSeatResource>, uint32_t, SP<CWLSurfaceResource>, const Vector2D&)>(setCursorHook->m_original)(self, std::move(seat), serial,
                                                                                                                                                 std::move(cursorSurface), hotspot);
}

static std::string setTarget(Session& current, const std::string& selector) {
    auto window = Desktop::viewState()->query().mappedOnly().selector(selector).runWindow();
    if (!window || Hyprauto::Compat::isX11(window))
        return "error: expected a mapped native Wayland window";
    auto surface = window->wlSurface()->resource();
    auto client  = surface->client();
    if (window == current.window.lock())
        return "ok";
    if (auto owner = clientSession(client); owner && owner != &current)
        return "error: target client is already owned by another session";
    Session session;
    if (!g_pSeatManager->m_keyboard || !g_pSeatManager->m_mouse)
        return "error: keyboard and pointer capabilities required";
    session.client = client;
    refreshResources(session);
    if (session.keyboards.empty() || session.pointers.empty())
        return "error: target must bind keyboard and pointer before beginning";
    session.xkb = xkb_state_new(g_pSeatManager->m_keyboard->m_xkbKeymap);
    if (!session.xkb)
        return "error: cannot create independent XKB state";
    session.keymap   = g_pSeatManager->m_keyboard->m_xkbKeymapV1String;
    session.keymapFD = g_pSeatManager->m_keyboard->m_xkbKeymapV1FD.duplicate();
    if (!session.keymapFD.isValid()) {
        xkb_state_unref(session.xkb);
        return "error: cannot retain session keymap";
    }
    session.window  = window;
    session.surface = surface;
    clearTarget(current);
    session.id             = current.id;
    session.capture        = std::move(current.capture);
    current                = std::move(session);
    current.surfaceDestroy = surface->m_events.destroy.listen([id = current.id] {
        if (auto it = sessions.find(id); it != sessions.end())
            clearTarget(*it->second, false);
    });
    Log::logger->log(Log::DEBUG, "[hyprauto] session {} bound target surface {}; synthetic focus deferred until input", current.id, surfaceID(current.surface));
    return "ok";
}

static uint32_t surfaceID(const WP<CWLSurfaceResource>& surface) {
    return surface ? wl_resource_get_id(surface->getResource()->resource()) : 0;
}

static std::string status(const Session& session) {
    auto       window   = Desktop::focusState()->window();
    auto       cursor   = g_pInputManager->getMouseCoordsInternal();
    const auto keyboard = g_pSeatManager->m_keyboard.lock();
    return nlohmann::json{
        {"session_id", session.id},
        {"target", session.window ? Hyprauto::Compat::appID(session.window.lock()) : ""},
        {"active", session.client != nullptr},
        {"block_input", session.blockInput},
        {"keys", session.keys},
        {"buttons", session.buttons},
        {"position", {session.position.x, session.position.y}},
        {"host",
         {
             {"window", window ? Hyprauto::Compat::appID(window) : ""},
             {"keyboard_surface", surfaceID(g_pSeatManager->m_state.keyboardFocus)},
             {"pointer_surface", surfaceID(g_pSeatManager->m_state.pointerFocus)},
             {"cursor", {cursor.x, cursor.y}},
             {"cursor_updates", hostCursorUpdates},
             {"keys", g_pInputManager->getKeysFromAllKBs()},
             {"modifiers", keyboard ? keyboard->m_modifiersState.depressed : 0},
         }},
    }
        .dump();
}

static std::string dispatchSession(uint64_t id, const std::string& request, Hyprauto::Socket::Reply reply = {});

static int releaseClick(void* data) {
    auto& session = *static_cast<Session*>(data);
    const auto remaining = session.clickDeadline - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero()) {
        const auto delay = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        if (wl_event_source_timer_update(session.clickTimer, delay) == 0)
            return 0;
        clearTarget(session);
        return 0;
    }
    wl_event_source_remove(session.clickTimer);
    session.clickTimer = nullptr;
    auto reply = std::exchange(session.clickReply, {});
    const auto result = dispatchSession(session.id, session.clickRelease);
    if (result.starts_with("error:"))
        clearTarget(session);
    reply(result);
    return 0;
}

static std::string dispatchSession(uint64_t id, const std::string& request, Hyprauto::Socket::Reply reply) {
    auto&              session = *sessions.at(id);
    std::istringstream input(request);
    std::string        operation;
    input >> operation;
    if (operation == "status" || operation == "heartbeat" || operation == "end") {
        std::string extra;
        if (input >> extra)
            return "error: command accepts no arguments";
        if (operation == "status")
            return status(session);
        if (operation == "heartbeat")
            return "ok";
    }
    const bool previousDispatch = automationDispatch;
    automationDispatch = true;
    CScopeGuard restore([previousDispatch] { automationDispatch = previousDispatch; });
    if (operation == "end") {
        clearTarget(session);
        return "ok";
    }
    if (session.clickReply)
        return "error: click in progress";
    if (operation == "set-target") {
        std::string selector, extra;
        if (!(input >> selector) || input >> extra)
            return "error: set-target requires one window selector";
        return setTarget(session, selector);
    }
    if (!session.client || !session.surface)
        return "error: no active session";
    if (operation == "screenshot") {
        std::string path, extra;
        input >> std::ws;
        if (!input.eof() && (!(input >> std::quoted(path)) || path.empty() || input >> extra))
            return "error: screenshot accepts one optional PNG path";
        auto window = session.window.lock();
        return window ? session.capture->start(window, path) : "error: session target is unavailable";
    }
    if (operation == "screenshot-status" || operation == "screenshot-release" || operation == "screenshot-read") {
        uint64_t    id;
        std::string extra;
        if (!(input >> id))
            return "error: screenshot command requires an ID";
        if (operation == "screenshot-read") {
            size_t offset, length;
            if (!(input >> offset >> length) || input >> extra)
                return "error: screenshot-read requires ID, offset and length";
            return session.capture->read(id, offset, length);
        }
        if (input >> extra)
            return "error: screenshot command accepts one ID";
        return operation == "screenshot-status" ? session.capture->status(id) : session.capture->release(id);
    }
    if (operation == "block-input") {
        std::string mode, extra;
        if (!(input >> mode) || input >> extra || (mode != "on" && mode != "off"))
            return "error: block-input requires on/off";
        session.blockInput = mode == "on";
        return "ok";
    }
    if (operation == "click" || operation == "click-key") {
        int code, hold;
        double x = 0, y = 0;
        std::string extra;
        if ((operation == "click" && !(input >> x >> y)) || !(input >> code >> hold) || input >> extra || hold < 0 || hold > 10000 ||
            (operation == "click-key" ? code < 1 || code > KEY_MAX : code < BTN_MOUSE || code >= BTN_JOYSTICK))
            return "error: expected valid click arguments and hold duration 0..10000 ms";
        if ((operation == "click-key" ? session.keys : session.buttons).contains(code))
            return "error: unpaired input state";
        if (!reply)
            return "error: click requires asynchronous reply";
        auto timer = wl_event_loop_add_timer(wl_display_get_event_loop(g_pCompositor->m_wlDisplay), releaseClick, &session);
        if (!timer)
            return "error: cannot create click timer";
        const auto operationName = operation == "click-key" ? "key " : "button ";
        std::string result = "ok";
        if (operation == "click")
            result = dispatchSession(id, std::format("move {} {}", x, y));
        if (result == "ok")
            result = dispatchSession(id, std::format("{}{} down", operationName, code));
        if (result != "ok") {
            wl_event_source_remove(timer);
            return result;
        }
        session.clickRelease = std::format("{}{} up", operationName, code);
        if (hold == 0) {
            wl_event_source_remove(timer);
            result = dispatchSession(id, session.clickRelease);
            if (result.starts_with("error:"))
                clearTarget(session);
            return result;
        }
        session.clickTimer = timer;
        session.clickDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold);
        if (wl_event_source_timer_update(timer, hold) != 0) {
            clearTarget(session);
            return "error: cannot arm click timer";
        }
        session.clickReply = std::move(reply);
        return {};
    }
    if (operation == "key" || operation == "button") {
        int         code;
        std::string state, extra;
        if (!(input >> code >> state) || input >> extra || (state != "down" && state != "up") ||
            (operation == "key" ? code < 1 || code > KEY_MAX : code < BTN_MOUSE || code >= BTN_JOYSTICK))
            return "error: expected valid evdev code and down/up";
        auto& pressed = operation == "key" ? session.keys : session.buttons;
        bool  down    = state == "down";
        if (pressed.contains(code) == down)
            return "error: unpaired input state";
        refreshResources(session);
        if (operation == "key" ? session.keyboards.empty() : session.pointers.empty())
            return "error: target has no live input resource";
        if (operation == "key")
            ensureKeyboard(session);
        else
            ensurePointer(session);
        if (down)
            pressed.insert(code);
        else
            pressed.erase(code);
        if (operation == "key") {
            sendModifiers(session);
            for (const auto& weak : session.keyboards)
                if (auto keyboard = weak.lock())
                    keyboard->sendKey(now(), code, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            xkb_state_update_key(session.xkb, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
            sendModifiers(session);
        } else {
            for (const auto& weak : session.pointers) {
                if (auto pointer = weak.lock()) {
                    pointer->sendMotion(now(), session.position);
                    pointer->sendButton(now(), code, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
                    pointer->sendFrame();
                }
            }
        }
        return "ok";
    }
    if (operation == "move") {
        double      x, y;
        std::string extra;
        if (!(input >> x >> y) || input >> extra || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= session.surface->m_current.size.x ||
            y >= session.surface->m_current.size.y)
            return "error: coordinates must be inside the target surface";
        refreshResources(session);
        if (session.pointers.empty())
            return "error: target has no live pointer";
        session.position = {x, y};
        ensurePointer(session);
        for (const auto& weak : session.pointers) {
            if (auto pointer = weak.lock()) {
                pointer->sendMotion(now(), session.position);
                pointer->sendFrame();
            }
        }
        return "ok";
    }
    return "error: unknown session command";
}

static std::string dispatchManagement(const std::string& request) {
    std::istringstream input(request);
    std::string        prefix, operation, extra;
    input >> prefix >> operation;
    if (prefix != "hyprauto" || !server)
        return "error: hyprauto service is unavailable";
    if (operation == "end") {
        uint64_t id;
        if (!(input >> id) || input >> extra || !sessions.contains(id))
            return "error: expected an active session ID";
        server->disconnect(id);
        return "ok";
    }
    if (input >> extra)
        return "error: command accepts no arguments";
    if (operation == "status")
        return nlohmann::json{{"socket", server->path()}, {"protocol", 1}, {"sessions", sessions.size()}, {"lease_ms", 30000}}.dump();
    if (operation == "sessions") {
        auto result = nlohmann::json::array();
        for (const auto& [id, session] : sessions)
            result.push_back(nlohmann::json::parse(status(*session)));
        return result.dump();
    }
    return "error: use status, sessions or end <session-id>";
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE pluginHandle) {
    handle = pluginHandle;
    if (HyprlandAPI::getHyprlandVersion(handle).hash != GIT_COMMIT_HASH)
        throw std::runtime_error("Hyprland source revision mismatch");
    Gate<0, CWLKeyboardResource, SP<CWLSurfaceResource>, wl_array*>::install("CWLKeyboardResource::sendEnter");
    Gate<1, CWLKeyboardResource>::install("CWLKeyboardResource::sendLeave");
    Gate<2, CWLKeyboardResource, uint32_t, uint32_t, wl_keyboard_key_state>::install("CWLKeyboardResource::sendKey");
    Gate<3, CWLKeyboardResource, uint32_t, uint32_t, uint32_t, uint32_t>::install("CWLKeyboardResource::sendMods");
    Gate<4, CWLPointerResource, SP<CWLSurfaceResource>, const Vector2D&>::install("CWLPointerResource::sendEnter");
    Gate<5, CWLPointerResource>::install("CWLPointerResource::sendLeave");
    Gate<6, CWLPointerResource, uint32_t, const Vector2D&>::install("CWLPointerResource::sendMotion");
    Gate<7, CWLPointerResource, uint32_t, uint32_t, wl_pointer_button_state>::install("CWLPointerResource::sendButton");
    Gate<8, CWLPointerResource>::install("CWLPointerResource::sendFrame");
    Gate<9, CWLPointerResource, uint32_t, wl_pointer_axis, double>::install("CWLPointerResource::sendAxis");
    Gate<10, CWLPointerResource, wl_pointer_axis_source>::install("CWLPointerResource::sendAxisSource");
    Gate<11, CWLPointerResource, uint32_t, wl_pointer_axis>::install("CWLPointerResource::sendAxisStop");
    Gate<12, CWLPointerResource, wl_pointer_axis, int32_t>::install("CWLPointerResource::sendAxisDiscrete");
    Gate<13, CWLPointerResource, wl_pointer_axis, int32_t>::install("CWLPointerResource::sendAxisValue120");
    Gate<14, CWLPointerResource, wl_pointer_axis, wl_pointer_axis_relative_direction>::install("CWLPointerResource::sendAxisRelativeDirection");
    pointerEnterHook = installHook("CWlPointer::sendEnter", reinterpret_cast<void*>(&recordPointerEnter));
    shapePointerHook = installHook("CCursorShapeProtocol::createCursorShapeDevice", reinterpret_cast<void*>(&recordShapePointer));
    setShapeHook     = installHook("CCursorShapeProtocol::onSetShape", reinterpret_cast<void*>(&traceSetShape));
    setCursorHook    = installHook("CSeatManager::onSetCursor", reinterpret_cast<void*>(&traceSetCursor));
    keymapHook = installHook("CWLSeatProtocol::updateKeymap", reinterpret_cast<void*>(&updateHostKeymaps));
    repeatHook = installHook("CWLSeatProtocol::updateRepeatInfo", reinterpret_cast<void*>(&updateHostRepeatInfo));
    command    = Hyprauto::Compat::registerCommand(handle, "hyprauto", dispatchManagement);
    if (!command)
        throw std::runtime_error("Cannot register hyprauto command");
    hostCursorListener   = g_pSeatManager->m_events.setCursor.listen([](const auto& event) {
        ++hostCursorUpdates;
        Log::logger->log(Log::DEBUG, "[hyprauto] compositor accepted set_cursor: cursor surface {}, pointer focus surface {}, cursor hotspot ({:.1f}, {:.1f})",
                         surfaceID(event.surf), surfaceID(g_pSeatManager->m_state.pointerFocus), event.hotspot.x, event.hotspot.y);
    });
    pointerFocusListener = g_pSeatManager->m_events.pointerFocusChange.listen([] {
        auto focus   = g_pSeatManager->m_state.pointerFocus.lock();
        auto session = focus ? clientSession(focus->client()) : nullptr;
        Log::logger->log(Log::DEBUG, "[hyprauto] compositor pointer focus changed: surface {}, hyprauto session {}", surfaceID(g_pSeatManager->m_state.pointerFocus),
                         session ? std::to_string(session->id) : "none");
    });
    cursorShapeListener  = PROTO::cursorShape->m_events.setShape.listen([](const CCursorShapeProtocol::SSetShapeEvent& event) {
        auto       focusResource   = g_pSeatManager->m_state.pointerFocusResource.lock();
        auto       focus           = g_pSeatManager->m_state.pointerFocus.lock();
        auto       requestSession  = clientSession(event.pMgr->client());
        auto       focusSession    = focus ? clientSession(focus->client()) : nullptr;
        const bool acceptedByFocus = focusResource && event.pMgr->client() == focusResource->client();
        if (acceptedByFocus)
            ++hostCursorUpdates;
        Log::logger->log(Log::DEBUG, "[hyprauto] cursor-shape request: shape {}, request session {}, compositor pointer focus surface {}, focus session {}, focus accepts {}",
                         event.shapeName, requestSession ? std::to_string(requestSession->id) : "none", surfaceID(focus), focusSession ? std::to_string(focusSession->id) : "none",
                         acceptedByFocus);
    });
    windowClose        = Event::bus()->m_events.window.close.listen([](PHLWINDOW window) {
        for (const auto& [id, session] : sessions)
            if (window == session->window.lock())
                clearTarget(*session, false);
    });
    server             = std::make_unique<Hyprauto::Socket>(
        wl_display_get_event_loop(g_pCompositor->m_wlDisplay), g_pCompositor->m_instancePath + "/.hyprauto.sock",
        [](uint64_t id) {
            auto session     = std::make_unique<Session>();
            session->id      = id;
            session->capture = std::make_unique<Hyprauto::Capture::Job>();
            sessions.emplace(id, std::move(session));
        },
        [](uint64_t id) {
            if (auto it = sessions.find(id); it != sessions.end()) {
                clearTarget(*it->second);
                sessions.erase(it);
            }
        },
        [](uint64_t id, const std::string& request, Hyprauto::Socket::Reply reply) {
            auto result = dispatchSession(id, request, reply);
            if (!result.empty())
                reply(result);
        });
    Log::logger->log(Log::DEBUG, "[hyprauto] plugin initialized; pointer focus and set_cursor tracing enabled");
    return {"hyprauto", "Independent background Wayland input sessions", "hyprauto contributors", HYPRAUTO_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    server.reset();
    windowClose.reset();
    hostCursorListener.reset();
    pointerFocusListener.reset();
    cursorShapeListener.reset();
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
    command.reset();
    for (auto hook : hooks)
        HyprlandAPI::removeFunctionHook(handle, hook);
    hooks.clear();
    shapePointers.clear();
}
