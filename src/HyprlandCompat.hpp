#pragma once

#include <src/plugins/PluginAPI.hpp>
#if __has_include(<src/desktop/view/window/Window.hpp>)
#include <src/desktop/view/window/Window.hpp>
#include <src/desktop/view/window/WindowMetadata.hpp>
#else
#include <src/desktop/view/Window.hpp>
#endif
#include <functional>

namespace Hyprauto::Compat {
    template <typename Window>
    bool isX11(const Window& window) {
        if constexpr (requires { window->backend().isX11(); })
            return window->backend().isX11();
        else
            return window->m_isX11;
    }

    template <typename Window>
    std::string appID(const Window& window) {
        if constexpr (requires { window->metadata().appID(); })
            return window->metadata().appID();
        else
            return window->m_class;
    }

#if __has_include(<src/ipc/s1/S1.hpp>)
    using Command = IPC::Socket1::SCommand;

    inline SP<Command> registerCommand(HANDLE handle, const std::string& name, std::function<std::string(const std::string&)> dispatch) {
        return HyprlandAPI::registerHyprCtlCommand(
            handle,
            {
                .name    = name,
                .match   = IPC::Socket1::COMMAND_MATCH_PREFIX,
                .handler = [dispatch](const IPC::Socket1::SRequest& request) -> IPC::Socket1::SResponse { return dispatch(request.command); },
            });
    }
#else
    using Command = SHyprCtlCommand;

    inline SP<Command> registerCommand(HANDLE handle, const std::string& name, std::function<std::string(const std::string&)> dispatch) {
        return HyprlandAPI::registerHyprCtlCommand(handle,
                                                   {
                                                       .name  = name,
                                                       .exact = false,
                                                       .fn    = [dispatch](eHyprCtlOutputFormat, std::string request) { return dispatch(request); },
                                                   });
    }
#endif
}
