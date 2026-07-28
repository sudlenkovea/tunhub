#include "tunhub/ipc.h"

#include <windows.h>
#include <sddl.h>

#include <vector>

#include "tunhub/constants.h"
#include "tunhub/str.h"

namespace tunhub::ipc {
namespace {

constexpr DWORD kBufferSize = 64 * 1024;

/// Read one newline-terminated JSON message.
bool readMessage(HANDLE pipe, std::string& out) {
    out.clear();
    char buf[4096];
    DWORD read = 0;
    while (true) {
        if (!ReadFile(pipe, buf, sizeof(buf), &read, nullptr) || read == 0) return false;
        out.append(buf, read);
        if (out.find('\n') != std::string::npos) {
            out.erase(out.find('\n'));
            return true;
        }
        if (out.size() > kBufferSize) return false;   // malformed / oversized
    }
}

bool writeMessage(HANDLE pipe, const std::string& text) {
    const auto payload = text + "\n";
    DWORD written = 0;
    return WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
           written == payload.size();
}

}  // namespace

std::string Response::encode() const {
    Json j = Json::object();
    j.set("ok", Json(ok));
    if (!error.empty()) j.set("error", Json(error));
    if (!payload.isNull()) j.set("payload", payload);
    return j.dump();
}

Response Response::decode(const std::string& text) {
    std::string err;
    const Json j = Json::parse(text, &err);
    if (!err.empty()) return Response::failure("malformed response: " + err);
    Response r;
    r.ok = j["ok"].asBool(false);
    r.error = j["error"].asString("");
    r.payload = j["payload"];
    return r;
}

// ── server ───────────────────────────────────────────────────────────────────

Server::~Server() { stop(); }

bool Server::start(std::string* error) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) {
        if (error) *error = "CreateEvent failed";
        return false;
    }
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop() {
    stopping_ = true;
    if (stopEvent_) SetEvent(static_cast<HANDLE>(stopEvent_));
    // Unblock a ConnectNamedPipe that is waiting for a client.
    HANDLE h = CreateFileW(ipc::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) {
        CloseHandle(static_cast<HANDLE>(stopEvent_));
        stopEvent_ = nullptr;
    }
}

void Server::acceptLoop() {
    // The service runs as LocalSystem while the UI runs unprivileged, so the pipe must grant
    // interactive users access. Authenticated Users get read/write on the pipe itself; the
    // service still decides what each request is allowed to do.
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;AU)(A;;GA;;;BA)(A;;GA;;;SY)", SDDL_REVISION_1, &sd, nullptr)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = sd;
        sa.bInheritHandle = FALSE;
    }

    while (!stopping_) {
        HANDLE pipe = CreateNamedPipeW(ipc::kPipeName,
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       kBufferSize, kBufferSize, 0,
                                       sd ? &sa : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ||
                               GetLastError() == ERROR_PIPE_CONNECTED;
        if (stopping_) {
            CloseHandle(pipe);
            break;
        }
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        // One request per connection keeps the server trivially reentrant.
        serveClient(pipe);
    }
    if (sd) LocalFree(sd);
}

void Server::serveClient(void* pipeHandle) {
    HANDLE pipe = static_cast<HANDLE>(pipeHandle);
    std::string text;
    if (readMessage(pipe, text)) {
        std::string err;
        const Json j = Json::parse(text, &err);
        Response response = err.empty()
                                ? handler_({j["method"].asString(""), j["payload"]})
                                : Response::failure("malformed request: " + err);
        writeMessage(pipe, response.encode());
        FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

// ── client ───────────────────────────────────────────────────────────────────

bool Client::call(const std::string& method, const Json& payload, Response* out, int timeoutMs) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (GetTickCount() < deadline) {
        pipe = CreateFileW(ipc::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        // ERROR_PIPE_BUSY means every instance is in use; anything else means the service
        // isn't there, and retrying won't help.
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        if (!WaitNamedPipeW(ipc::kPipeName, 200)) continue;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    Json request = Json::object();
    request.set("method", Json(method));
    if (!payload.isNull()) request.set("payload", payload);

    bool ok = false;
    if (writeMessage(pipe, request.dump())) {
        std::string text;
        if (readMessage(pipe, text)) {
            if (out) *out = Response::decode(text);
            ok = true;
        }
    }
    CloseHandle(pipe);
    return ok;
}

bool Client::ping() {
    Response r;
    return call(method::kVersion, Json(), &r, 1500) && r.ok;
}

}  // namespace tunhub::ipc
