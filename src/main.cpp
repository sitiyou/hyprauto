#include "HyprlandCompat.hpp"
#include <src/protocols/core/Seat.hpp>
#include <src/protocols/core/Compositor.hpp>
#include <src/managers/SeatManager.hpp>
#include <src/managers/input/InputManager.hpp>
#include <src/desktop/state/ViewState.hpp>
#include <src/desktop/state/ViewQuery.hpp>
#include <src/desktop/state/FocusState.hpp>
#include <src/devices/IKeyboard.hpp>
#include <src/helpers/time/Time.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <nlohmann/json.hpp>
#include <linux/input-event-codes.h>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>

using Hyprutils::Utils::CScopeGuard;

static HANDLE                        handle             = nullptr;
static bool                          automationDispatch = false;
static std::vector<CFunctionHook*>   hooks;
static SP<Hyprauto::Compat::Command> command;
static CHyprSignalListener           windowClose;
static CFunctionHook*                keymapHook = nullptr;
static CFunctionHook*                repeatHook = nullptr;
static CHyprSignalListener           hostCursorListener;
static uint64_t                      hostCursorUpdates = 0;

struct Session {
    wl_client*                           client = nullptr;
    PHLWINDOWREF                         window;
    WP<CWLSurfaceResource>               surface;
    std::vector<WP<CWLKeyboardResource>> keyboards;
    std::vector<WP<CWLPointerResource>>  pointers;
    std::set<uint32_t>                   keys, buttons;
    bool                                 blockInput = false;
    Vector2D                             position   = {1, 1};
    xkb_state*                           xkb        = nullptr;
    CHyprSignalListener                  surfaceDestroy;
};

static Session  session;

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

template <int ID, typename Resource, typename... Args>
struct Gate {
    static inline CFunctionHook* hook       = nullptr;
    static constexpr bool        focusEvent = ID == 0 || ID == 1 || ID == 4 || ID == 5;
    static constexpr bool        frameEvent = ID == 8;

    static void                  call(Resource* self, Args... args) {
        if (!automationDispatch && session.client && self->m_owner && self->m_owner->client() == session.client) {
            const auto& hostFocus = std::is_same_v<Resource, CWLKeyboardResource> ? g_pSeatManager->m_state.keyboardFocus : g_pSeatManager->m_state.pointerFocus;
            if (focusEvent || session.blockInput || (!frameEvent && (!hostFocus || hostFocus->client() != session.client)))
                return;
        }
        reinterpret_cast<void (*)(Resource*, Args...)>(hook->m_original)(self, args...);
    }

    static void install(const std::string& name) {
        hook = installHook(name, reinterpret_cast<void*>(&call));
    }
};

static void sendModifiers() {
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

template <typename Callback>
static void forEachHostKeyboard(Callback callback) {
    wl_client* client = nullptr;
    wl_client_for_each(client, wl_display_get_client_list(wl_client_get_display(session.client))) {
        if (client == session.client)
            continue;
        for (const auto& seat : clientSeats(client))
            for (const auto& weak : seat->m_keyboards)
                if (auto keyboard = weak.lock())
                    callback(keyboard);
    }
}

static void updateHostKeymaps(CWLSeatProtocol* self) {
    if (!session.client) {
        reinterpret_cast<void (*)(CWLSeatProtocol*)>(keymapHook->m_original)(self);
        return;
    }
    forEachHostKeyboard([](const auto& keyboard) { keyboard->sendKeymap(g_pSeatManager->m_keyboard.lock()); });
}

static void updateHostRepeatInfo(CWLSeatProtocol* self, uint32_t rate, uint32_t delay) {
    if (!session.client) {
        reinterpret_cast<void (*)(CWLSeatProtocol*, uint32_t, uint32_t)>(repeatHook->m_original)(self, rate, delay);
        return;
    }
    forEachHostKeyboard([&](const auto& keyboard) { keyboard->repeatInfo(rate, delay); });
}

static void endSession(bool restoreHostFocus = true) {
    if (!session.client)
        return;
    const bool previousDispatch = automationDispatch;
    automationDispatch          = true;
    CScopeGuard restore([&] { automationDispatch = previousDispatch; });
    session.surfaceDestroy.reset();
    for (const auto& weak : session.keyboards) {
        if (auto keyboard = weak.lock()) {
            for (auto key : session.keys)
                keyboard->sendKey(now(), key, WL_KEYBOARD_KEY_STATE_RELEASED);
            keyboard->sendMods(0, 0, 0, 0);
            keyboard->sendLeave();
            if (auto nativeKeyboard = g_pSeatManager->m_keyboard.lock()) {
                keyboard->sendKeymap(nativeKeyboard);
                keyboard->repeatInfo(nativeKeyboard->m_repeatRate, nativeKeyboard->m_repeatDelay);
            }
        }
    }
    for (const auto& weak : session.pointers) {
        if (auto pointer = weak.lock()) {
            for (auto button : session.buttons)
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
            if (auto keyboard = weak.lock()) {
                keyboard->sendEnter(keyboardFocus, &keys);
                const auto& mods = g_pSeatManager->m_keyboard->m_modifiersState;
                keyboard->sendMods(mods.depressed, mods.latched, mods.locked, mods.group);
            }
        }
    }
    auto pointerFocus = g_pSeatManager->m_state.pointerFocus.lock();
    if (restoreHostFocus && pointerFocus && pointerFocus->client() == session.client && session.window) {
        auto local = g_pInputManager->getMouseCoordsInternal() - session.window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        for (const auto& weak : session.pointers) {
            if (auto pointer = weak.lock()) {
                pointer->sendEnter(pointerFocus, local);
                pointer->sendFrame();
            }
        }
    }
    xkb_state_unref(session.xkb);
    session = {};
}

static std::string beginSession(const std::string& selector) {
    if (session.client)
        return "error: session already active";
    auto window = Desktop::viewState()->query().mappedOnly().selector(selector).runWindow();
    if (!window || Hyprauto::Compat::isX11(window))
        return "error: expected a mapped native Wayland window";
    auto surface = window->wlSurface()->resource();
    auto client  = surface->client();
    if (!g_pSeatManager->m_keyboard || !g_pSeatManager->m_mouse)
        return "error: keyboard and pointer capabilities required";
    for (const auto& seat : clientSeats(client)) {
        for (const auto& keyboard : seat->m_keyboards)
            if (keyboard)
                session.keyboards.push_back(keyboard);
        for (const auto& pointer : seat->m_pointers)
            if (pointer)
                session.pointers.push_back(pointer);
    }
    if (session.keyboards.empty() || session.pointers.empty()) {
        session = {};
        return "error: target must bind keyboard and pointer before beginning";
    }
    session.xkb = xkb_state_new(g_pSeatManager->m_keyboard->m_xkbKeymap);
    if (!session.xkb) {
        session = {};
        return "error: cannot create independent XKB state";
    }
    session.client         = client;
    session.window         = window;
    session.surface        = surface;
    session.surfaceDestroy = surface->m_events.destroy.listen([] { endSession(false); });
    wl_array keys{};
    for (const auto& weak : session.keyboards) {
        if (auto keyboard = weak.lock()) {
            keyboard->sendKeymap(g_pSeatManager->m_keyboard.lock());
            keyboard->sendEnter(surface, &keys);
        }
    }
    sendModifiers();
    for (const auto& weak : session.pointers) {
        if (auto pointer = weak.lock()) {
            pointer->sendEnter(surface, session.position);
            pointer->sendFrame();
        }
    }
    return "ok";
}

static uint32_t surfaceID(const WP<CWLSurfaceResource>& surface) {
    return surface ? wl_resource_get_id(surface->getResource()->resource()) : 0;
}

static std::string status() {
    auto       window   = Desktop::focusState()->window();
    auto       cursor   = g_pInputManager->getMouseCoordsInternal();
    const auto keyboard = g_pSeatManager->m_keyboard.lock();
    return nlohmann::json{
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

static std::string dispatch(const std::string& request) {
    std::istringstream input(request);
    std::string        prefix, operation;
    input >> prefix >> operation;
    if (prefix != "hyprauto")
        return "error: unknown command";
    if (operation == "status")
        return status();
    automationDispatch = true;
    CScopeGuard restore([] { automationDispatch = false; });
    if (operation == "end") {
        endSession();
        return "ok";
    }
    if (operation == "begin") {
        std::string selector, extra;
        if (!(input >> selector) || input >> extra)
            return "error: begin requires one window selector";
        return beginSession(selector);
    }
    if (!session.client || !session.surface)
        return "error: no active session";
    if (operation == "block-input") {
        std::string mode, extra;
        if (!(input >> mode) || input >> extra || (mode != "on" && mode != "off"))
            return "error: block-input requires on/off";
        session.blockInput = mode == "on";
        return "ok";
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
        if (down)
            pressed.insert(code);
        else
            pressed.erase(code);
        if (operation == "key") {
            sendModifiers();
            for (const auto& weak : session.keyboards)
                if (auto keyboard = weak.lock())
                    keyboard->sendKey(now(), code, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            xkb_state_update_key(session.xkb, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
            sendModifiers();
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
        session.position = {x, y};
        for (const auto& weak : session.pointers) {
            if (auto pointer = weak.lock()) {
                pointer->sendMotion(now(), session.position);
                pointer->sendFrame();
            }
        }
        return "ok";
    }
    return "error: use begin, key, move, button, block-input, end or status";
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
    keymapHook = installHook("CWLSeatProtocol::updateKeymap", reinterpret_cast<void*>(&updateHostKeymaps));
    repeatHook = installHook("CWLSeatProtocol::updateRepeatInfo", reinterpret_cast<void*>(&updateHostRepeatInfo));
    command    = Hyprauto::Compat::registerCommand(handle, "hyprauto", dispatch);
    if (!command)
        throw std::runtime_error("Cannot register hyprauto command");
    hostCursorListener = g_pSeatManager->m_events.setCursor.listen([](const auto&) { ++hostCursorUpdates; });
    windowClose        = Event::bus()->m_events.window.close.listen([](PHLWINDOW window) {
        if (window == session.window.lock())
            endSession(false);
    });
    return {"hyprauto", "Independent background Wayland input sessions", "hyprauto contributors", HYPRAUTO_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    endSession();
    windowClose.reset();
    hostCursorListener.reset();
    HyprlandAPI::unregisterHyprCtlCommand(handle, command);
    command.reset();
    for (auto hook : hooks)
        HyprlandAPI::removeFunctionHook(handle, hook);
    hooks.clear();
}
