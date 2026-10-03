#pragma once
#include "HyprlandCompat.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace Hyprauto::Capture {
    void shutdown();

    class Job {
      public:
        Job();
        ~Job();
        Job(const Job&)                   = delete;
        Job&        operator=(const Job&) = delete;
        std::string start(PHLWINDOW window, const std::string& path);
        std::string status(uint64_t id);
        std::string read(uint64_t id, size_t offset, size_t length);
        std::string release(uint64_t id);
        void        cancel();

      private:
        struct State;
        std::unique_ptr<State> state;
    };
}
