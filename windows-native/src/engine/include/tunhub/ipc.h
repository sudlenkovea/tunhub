#pragma once
// UI ↔ privileged service IPC: one JSON line per request and per response over a named pipe.

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "tunhub/json.h"

namespace tunhub::ipc {

namespace method {
inline constexpr const char* kVersion       = "version";
inline constexpr const char* kStartTunnel   = "startTunnel";
inline constexpr const char* kStopTunnel    = "stopTunnel";
inline constexpr const char* kStopAll       = "stopAll";
inline constexpr const char* kRuntimeStates = "runtimeStates";
inline constexpr const char* kSetKillSwitch = "setKillSwitch";
inline constexpr const char* kRecentLog     = "recentLog";
inline constexpr const char* kSetLogMode    = "setLogMode";
}  // namespace method

struct Request {
    std::string method;
    Json payload;
};

struct Response {
    bool ok = false;
    std::string error;
    Json payload;

    static Response success(Json p = Json()) { return {true, {}, std::move(p)}; }
    static Response failure(std::string e) { return {false, std::move(e), Json()}; }

    std::string encode() const;
    static Response decode(const std::string& text);
};

/// Server side (runs in the service).
class Server {
public:
    using Handler = std::function<Response(const Request&)>;

    explicit Server(Handler handler) : handler_(std::move(handler)) {}
    ~Server();

    bool start(std::string* error);
    void stop();

private:
    void acceptLoop();
    void serveClient(void* pipe);

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    void* stopEvent_ = nullptr;
};

/// Client side (runs in the UI). Each call opens, uses and closes a pipe instance — the UI
/// polls at ~1 Hz, so connection reuse would buy nothing and complicate reconnection.
class Client {
public:
    /// Returns false when the service is unreachable (not installed, stopped, or busy).
    bool call(const std::string& method, const Json& payload, Response* out,
              int timeoutMs = 5000);

    bool ping();
};

}  // namespace tunhub::ipc
