#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <wayland-server-core.h>

namespace Hyprauto {
    class Socket {
      public:
        using Open     = std::function<void(uint64_t)>;
        using Close    = std::function<void(uint64_t)>;
        using Reply    = std::function<void(const std::string&)>;
        using Dispatch = std::function<void(uint64_t, const std::string&, Reply)>;

        Socket(wl_event_loop* loop, std::string path, Open open, Close close, Dispatch dispatch);
        ~Socket();
        Socket(const Socket&)                       = delete;
        Socket&            operator=(const Socket&) = delete;
        void               disconnect(uint64_t id);
        const std::string& path() const;

      private:
        struct State;
        std::unique_ptr<State> state;
    };
}
