#include <src/plugins/PluginAPI.hpp>
#undef PLUGIN_INIT
#define PLUGIN_INIT testSupportInit
#include <hyprtester/plugin/src/main.cpp>
#undef PLUGIN_INIT
#define PLUGIN_INIT pluginInit

#include <src/render/Renderer.hpp>
#include <src/protocols/core/Compositor.hpp>
#include <algorithm>
#include <stdexcept>

static CFunctionHook* renderMonitorHook;
static bool           pauseComposition = false;
static bool           nativeFenceSync;

static void           renderMonitor(Render::IHyprRenderer* self, PHLMONITOR monitor, bool commit) {
    if (!pauseComposition)
        reinterpret_cast<void (*)(Render::IHyprRenderer*, PHLMONITOR, bool)>(renderMonitorHook->m_original)(self, monitor, commit);
}

static int captureRender(lua_State* L) {
    pauseComposition = !lua_toboolean(L, 1);
    return 0;
}

static int captureBuffers(lua_State* L) {
    const auto expected = static_cast<size_t>(luaL_checkinteger(L, 1));
    const auto actual   = g_pHyprRenderer->m_usedAsyncBuffers.size();
    return luaResult(L, actual == expected ? SDispatchResult{} : SDispatchResult{.success = false, .error = std::format("Expected {} retained buffers, got {}", expected, actual)});
}

static int captureFenceFailure(lua_State* L) {
    const auto count = std::to_string(luaL_optinteger(L, 1, 1));
    setenv("HYPRAUTO_TEST_FAIL_FENCE", count.c_str(), 1);
    return 0;
}

static int captureNativeSync(lua_State* L) {
    const bool enable = lua_isnoneornil(L, 1) ? nativeFenceSync : lua_toboolean(L, 1);
    if (enable && !nativeFenceSync)
        return luaL_error(L, "native fence sync unavailable");
    Render::GL::g_pHyprOpenGL->m_exts.EGL_ANDROID_native_fence_sync_ext = enable;
    return 0;
}

static int captureHoldFences(lua_State* L) {
    if (lua_toboolean(L, 1))
        setenv("HYPRAUTO_TEST_HOLD_FENCES", "1", 1);
    else
        unsetenv("HYPRAUTO_TEST_HOLD_FENCES");
    return 0;
}

static int captureKeepBuffer(lua_State* L) {
    const auto name = std::string{luaL_checkstring(L, 1)};
    for (const auto& window : Desktop::windowState()->windows()) {
        if (window->m_class == name) {
            g_pHyprRenderer->m_usedAsyncBuffers.emplace_back(window->wlSurface()->resource()->m_current.buffer);
            return 0;
        }
    }
    return luaL_error(L, "buffer target unavailable");
}

static int captureDropBuffers(lua_State* L) {
    g_pHyprRenderer->m_usedAsyncBuffers.clear();
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    auto description = testSupportInit(handle);
    g_pInputManager->updateCapabilities();
    nativeFenceSync = Render::GL::g_pHyprOpenGL->m_exts.EGL_ANDROID_native_fence_sync_ext;
    auto matches    = HyprlandAPI::findFunctionsByName(handle, "renderMonitor");
    std::erase_if(matches, [](const auto& match) { return !match.demangled.starts_with("Render::IHyprRenderer::renderMonitor("); });
    if (matches.size() != 1)
        throw std::runtime_error("Cannot resolve renderMonitor");
    renderMonitorHook = HyprlandAPI::createFunctionHook(handle, matches.front().address, reinterpret_cast<void*>(&renderMonitor));
    if (!renderMonitorHook || !renderMonitorHook->hook())
        throw std::runtime_error("Cannot hook renderMonitor");
    if (!HyprlandAPI::addLuaFunction(handle, "test", "capture_render", captureRender) || !HyprlandAPI::addLuaFunction(handle, "test", "capture_buffers", captureBuffers) ||
        !HyprlandAPI::addLuaFunction(handle, "test", "capture_fence_failure", captureFenceFailure) ||
        !HyprlandAPI::addLuaFunction(handle, "test", "capture_native_sync", captureNativeSync) ||
        !HyprlandAPI::addLuaFunction(handle, "test", "capture_hold_fences", captureHoldFences) ||
        !HyprlandAPI::addLuaFunction(handle, "test", "capture_keep_buffer", captureKeepBuffer) ||
        !HyprlandAPI::addLuaFunction(handle, "test", "capture_drop_buffers", captureDropBuffers))
        throw std::runtime_error("Cannot register capture test functions");
    return description;
}
