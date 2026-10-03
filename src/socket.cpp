#include "Socket.hpp"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace Hyprauto {
    using nlohmann::json;
    using Clock = std::chrono::steady_clock;

    struct Socket::State {
        struct Connection {
            State*            owner;
            uint64_t          id;
            int               fd;
            wl_event_source*  source = nullptr;
            std::string       input, output;
            Clock::time_point renewed = Clock::now();
            bool              closing = false;
        };

        wl_event_loop*                                  loop;
        std::string                                     path;
        Open                                            open;
        Close                                           close;
        Dispatch                                        dispatch;
        int                                             listener = -1;
        wl_event_source*                                source   = nullptr;
        wl_event_source*                                timer    = nullptr;
        bool                                            bound    = false;
        uint64_t                                        nextID   = 0;
        std::map<uint64_t, std::unique_ptr<Connection>> connections;

        ~State() {
            if (timer)
                wl_event_source_remove(timer);
            if (source)
                wl_event_source_remove(source);
            while (!connections.empty())
                disconnect(connections.begin()->first);
            if (listener >= 0)
                ::close(listener);
            if (bound)
                unlink(path.c_str());
        }

        void disconnect(uint64_t id) {
            const auto it = connections.find(id);
            if (it == connections.end())
                return;
            auto connection = std::move(it->second);
            connections.erase(it);
            if (connection->source)
                wl_event_source_remove(connection->source);
            ::close(connection->fd);
            close(id);
        }

        static std::string frame(char type, const std::string& payload) {
            uint32_t length = htonl(payload.size() + 1);
            return std::string(reinterpret_cast<char*>(&length), sizeof(length)) + type + payload;
        }

        void reply(Connection& connection, uint64_t id, const std::string& result) {
            if (result.starts_with("data:")) {
                std::string payload;
                for (int shift = 56; shift >= 0; shift -= 8)
                    payload += static_cast<char>(id >> shift);
                payload.append(result, 5);
                connection.output += frame('B', payload);
            } else {
                json response{{"id", id}};
                if (result.starts_with("error:"))
                    response["error"] = result.substr(result.starts_with("error: ") ? 7 : 6);
                else
                    response["result"] = result;
                connection.output += frame('J', response.dump());
            }
        }

        static int clientEvent(int fd, uint32_t mask, void* data) {
            auto&      connection = *static_cast<Connection*>(data);
            auto&      self       = *connection.owner;
            const auto id         = connection.id;
            if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
                self.disconnect(id);
                return 0;
            }
            if ((mask & WL_EVENT_READABLE) && !connection.closing) {
                char       buffer[16384];
                const auto count = recv(fd, buffer, sizeof(buffer), 0);
                if (!count || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                    self.disconnect(id);
                    return 0;
                }
                if (count > 0)
                    connection.input.append(buffer, count);
                while (connection.input.size() >= 4) {
                    uint32_t length;
                    std::memcpy(&length, connection.input.data(), 4);
                    length = ntohl(length);
                    if (length < 2 || length > 16384) {
                        self.disconnect(id);
                        return 0;
                    }
                    if (connection.input.size() < length + 4)
                        break;
                    if (connection.input[4] != 'J') {
                        self.disconnect(id);
                        return 0;
                    }
                    const auto payload = connection.input.substr(5, length - 1);
                    connection.input.erase(0, length + 4);
                    try {
                        const auto request = json::parse(payload);
                        if (!request.is_object() || !request.contains("id") || !request["id"].is_number_unsigned() || request["id"] == 0 || !request.contains("command") ||
                            !request["command"].is_string())
                            throw std::runtime_error("Invalid request envelope");
                        const auto requestID = request["id"].get<uint64_t>();
                        const auto command   = request["command"].get<std::string>();
                        const auto result    = self.dispatch(id, command);
                        self.reply(connection, requestID, result);
                        if (!result.starts_with("error:"))
                            connection.renewed = Clock::now();
                        std::string operation;
                        std::istringstream(command) >> operation;
                        if (operation == "end" && result == "ok")
                            connection.closing = true;
                    } catch (const std::exception&) {
                        self.disconnect(id);
                        return 0;
                    }
                    if (connection.output.size() > 1048576) {
                        self.disconnect(id);
                        return 0;
                    }
                    if (connection.closing)
                        break;
                }
            }
            if (!connection.output.empty()) {
                const auto count = send(fd, connection.output.data(), connection.output.size(), MSG_NOSIGNAL);
                if (count < 0 && errno != EAGAIN && errno != EINTR) {
                    self.disconnect(id);
                    return 0;
                }
                if (count > 0)
                    connection.output.erase(0, count);
            }
            if (connection.closing && connection.output.empty()) {
                self.disconnect(id);
                return 0;
            }
            wl_event_source_fd_update(connection.source, (connection.closing ? 0 : WL_EVENT_READABLE) | (connection.output.empty() ? 0 : WL_EVENT_WRITABLE));
            return 0;
        }

        static int acceptEvent(int fd, uint32_t, void* data) {
            auto&     self   = *static_cast<State*>(data);
            const int client = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client < 0)
                return 0;
            ucred     credentials{};
            socklen_t size = sizeof(credentials);
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &size) || credentials.uid != getuid() || self.connections.size() >= 64) {
                ::close(client);
                return 0;
            }
            auto connection    = std::make_unique<Connection>();
            connection->owner  = &self;
            connection->id     = ++self.nextID;
            connection->fd     = client;
            connection->output = frame('J', json{{"id", 0}, {"session_id", connection->id}, {"lease_ms", 30000}, {"protocol", 1}}.dump());
            connection->source = wl_event_loop_add_fd(self.loop, client, WL_EVENT_READABLE | WL_EVENT_WRITABLE, clientEvent, connection.get());
            if (!connection->source) {
                ::close(client);
                return 0;
            }
            const auto id = connection->id;
            self.connections.emplace(id, std::move(connection));
            try {
                self.open(id);
            } catch (const std::exception&) { self.disconnect(id); }
            return 0;
        }

        static int timerEvent(void* data) {
            auto&                 self = *static_cast<State*>(data);
            std::vector<uint64_t> stale;
            for (const auto& [id, connection] : self.connections)
                if (Clock::now() - connection->renewed >= std::chrono::seconds(30))
                    stale.push_back(id);
            for (auto id : stale)
                self.disconnect(id);
            wl_event_source_timer_update(self.timer, 1000);
            return 0;
        }
    };

    Socket::Socket(wl_event_loop* loop, std::string path, Open open, Close close, Dispatch dispatch) : state(std::make_unique<State>()) {
        state->loop     = loop;
        state->path     = std::move(path);
        state->open     = std::move(open);
        state->close    = std::move(close);
        state->dispatch = std::move(dispatch);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (state->path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("Hyprauto socket path is too long");
        std::memcpy(address.sun_path, state->path.c_str(), state->path.size() + 1);
        state->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (state->listener < 0 || bind(state->listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
            throw std::runtime_error("Cannot bind hyprauto socket");
        state->bound = true;
        if (chmod(state->path.c_str(), 0600) || listen(state->listener, 16))
            throw std::runtime_error("Cannot listen on hyprauto socket");
        state->source = wl_event_loop_add_fd(loop, state->listener, WL_EVENT_READABLE, State::acceptEvent, state.get());
        state->timer  = wl_event_loop_add_timer(loop, State::timerEvent, state.get());
        if (!state->source || !state->timer || wl_event_source_timer_update(state->timer, 1000))
            throw std::runtime_error("Cannot register hyprauto socket events");
    }

    Socket::~Socket() = default;

    void Socket::disconnect(uint64_t id) {
        state->disconnect(id);
    }

    const std::string& Socket::path() const {
        return state->path;
    }
}
