#include "tunhub/openvpn.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>   // strtoull
#include <filesystem>
#include <fstream>
#include <random>

#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/util.h"

namespace tunhub {
namespace {

unsigned short pickManagementPort() {
    // Loopback-only, so a random high port with a retry on bind failure is sufficient.
    static std::mt19937 gen{std::random_device{}()};
    return static_cast<unsigned short>(std::uniform_int_distribution<int>(24800, 25600)(gen));
}

std::string randomPassword() {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::mt19937 gen{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 61);
    std::string out;
    for (int i = 0; i < 24; ++i) out += alphabet[dist(gen)];
    return out;
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(std::filesystem::path(str::widen(path)), std::ios::binary | std::ios::trunc);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

}  // namespace

OpenVpnSession::OpenVpnSession(FileLog& log, ResolvedTunnelSpec spec)
    : log_(log), spec_(std::move(spec)) {}

OpenVpnSession::~OpenVpnSession() {
    stop();
}

void OpenVpnSession::writeConfigFile() {
    configPath_ = paths::tempDir() + "\\ovpn-" + spec_.id + ".conf";
    managementPasswordPath_ = paths::tempDir() + "\\ovpn-" + spec_.id + ".mgmt";
    managementPassword_ = randomPassword();

    std::string cfg = spec_.openVpn ? spec_.openVpn->configText : std::string{};
    cfg += "\n# --- added by TunHub ---\n";
    cfg += "management 127.0.0.1 " + std::to_string(managementPort_) + " \"" +
           managementPasswordPath_ + "\"\n";
    cfg += "management-hold\n";
    cfg += "management-query-passwords\n";
    // Fail fast instead of retrying forever with credentials the server already rejected —
    // the UI needs the chance to re-prompt.
    cfg += "auth-retry none\n";
    cfg += "pull-filter ignore \"block-outside-dns\"\n";

    writeFile(configPath_, cfg);
    writeFile(managementPasswordPath_, managementPassword_ + "\n");
}

void OpenVpnSession::cleanupFiles() {
    std::error_code ec;
    if (!configPath_.empty())
        std::filesystem::remove(std::filesystem::path(str::widen(configPath_)), ec);
    if (!managementPasswordPath_.empty())
        std::filesystem::remove(std::filesystem::path(str::widen(managementPasswordPath_)), ec);
}

bool OpenVpnSession::start(std::string* error) {
    managementPort_ = pickManagementPort();
    writeConfigFile();

    const auto exe = paths::coreBinary("openvpn.exe");
    std::vector<std::string> args{"--config", configPath_};

    if (!process_.start(exe, args, {}, [this](const std::string& l) { onCoreOutput(l); }, error)) {
        cleanupFiles();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        since_ = util::nowUnix();
        phase_ = TunnelPhase::Starting;
    }
    managementThread_ = std::thread([this] { managementLoop(); });
    return true;
}

void OpenVpnSession::stop() {
    stopping_ = true;
    if (socket_) {
        // `signal SIGTERM` lets OpenVPN tear down its routes and adapter cleanly.
        send("signal SIGTERM");
        Sleep(300);
        closesocket(reinterpret_cast<SOCKET>(socket_));
        socket_ = nullptr;
    }
    if (managementThread_.joinable()) managementThread_.join();
    process_.terminate(3000);
    cleanupFiles();
    finished_ = true;
}

void OpenVpnSession::onCoreOutput(const std::string& line) {
    // openvpn.exe also writes to stdout; keep it at debug so Normal capture stays quiet.
    log_.debug("core:" + spec_.name, line);
}

void OpenVpnSession::managementLoop() {
    SOCKET s = INVALID_SOCKET;
    // The management listener appears a moment after launch.
    for (int attempt = 0; attempt < 50 && !stopping_; ++attempt) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(managementPort_);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
        Sleep(100);
    }
    if (s == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = TunnelPhase::Failed;
        error_ = "could not reach the OpenVPN management interface";
        finished_ = true;
        return;
    }
    socket_ = reinterpret_cast<void*>(s);

    send(managementPassword_);        // management interface expects the password first
    send("state on");
    send("bytecount 2");
    send("hold release");

    std::string pending;
    char buf[4096];
    while (!stopping_) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            auto line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            handleManagementLine(line);
        }
    }
    finished_ = true;
}

void OpenVpnSession::send(const std::string& command) {
    if (!socket_) return;
    const auto line = command + "\n";
    ::send(reinterpret_cast<SOCKET>(socket_), line.data(), static_cast<int>(line.size()), 0);
}

void OpenVpnSession::sendCredentials() {
    const auto& c = spec_.openVpn;
    if (!c) return;
    auto password = c->password;
    // A static challenge is transmitted as SCRV1:<base64 pass>:<base64 otp>.
    if (!c->otp.empty()) {
        std::vector<unsigned char> p(password.begin(), password.end());
        std::vector<unsigned char> o(c->otp.begin(), c->otp.end());
        password = "SCRV1:" + str::base64Encode(p) + ":" + str::base64Encode(o);
    }
    // Always send BOTH lines. Sending only the password leaves OpenVPN blocked waiting for
    // the username it already asked for, and the connection hangs with no error.
    send("username \"Auth\" " + c->username);
    send("password \"Auth\" " + password);
}

void OpenVpnSession::handleManagementLine(const std::string& line) {
    if (str::startsWith(line, ">PASSWORD:Need 'Auth'")) {
        sendCredentials();
        return;
    }
    if (str::startsWith(line, ">PASSWORD:Verification Failed")) {
        std::lock_guard<std::mutex> lock(mutex_);
        authFailed_ = true;
        phase_ = TunnelPhase::Failed;
        error_ = "authentication failed — check the username and password";
        log_.error("ovpn", spec_.name + ": " + error_);
        return;
    }
    if (str::startsWith(line, ">STATE:")) {
        // >STATE:<time>,<state>,<description>,<local ip>,<remote ip>,…
        const auto parts = str::split(line.substr(7), ',', /*keepEmpty=*/true);
        if (parts.size() >= 2) {
            const auto& state = parts[1];
            std::lock_guard<std::mutex> lock(mutex_);
            if (state == "CONNECTED") {
                phase_ = TunnelPhase::Up;
                since_ = util::nowUnix();
                log_.info("ovpn", spec_.name + " connected");
            } else if (state == "RECONNECTING") {
                phase_ = TunnelPhase::Degraded;
            } else if (state == "EXITING") {
                if (phase_ != TunnelPhase::Failed) phase_ = TunnelPhase::Stopped;
            } else if (phase_ != TunnelPhase::Failed) {
                phase_ = TunnelPhase::Starting;
            }
        }
        return;
    }
    if (str::startsWith(line, ">BYTECOUNT:")) {
        const auto parts = str::split(line.substr(11), ',');
        if (parts.size() >= 2) {
            std::lock_guard<std::mutex> lock(mutex_);
            rx_ = std::strtoull(parts[0].c_str(), nullptr, 10);
            tx_ = std::strtoull(parts[1].c_str(), nullptr, 10);
        }
        return;
    }
    if (str::startsWith(line, ">LOG:")) {
        const auto parts = str::split(line.substr(5), ',', /*keepEmpty=*/true);
        if (parts.size() >= 3) {
            const auto& text = parts[2];
            // Surface the adapter name and pushed routes for the UI.
            if (str::icontains(text, "open_tun") || str::icontains(text, "TAP-Windows") ||
                str::icontains(text, "Wintun")) {
                log_.debug("ovpn", text);
            }
            if (str::icontains(text, "error") || str::icontains(text, "cannot") ||
                str::icontains(text, "failed")) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (error_.empty()) error_ = text;
                log_.warn("ovpn", spec_.name + ": " + text);
            }
        }
        return;
    }
    if (str::startsWith(line, ">FATAL:")) {
        std::lock_guard<std::mutex> lock(mutex_);
        phase_ = TunnelPhase::Failed;
        error_ = line.substr(7);
        log_.error("ovpn", spec_.name + ": " + error_);
    }
}

TunnelRuntimeState OpenVpnSession::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TunnelRuntimeState s;
    s.id = spec_.id;
    s.name = spec_.name;
    s.phase = phase_;
    s.interfaceName = adapter_;
    s.errorMessage = error_;
    s.since = since_;
    s.routes = pushedRoutes_;
    // OpenVPN reports totals rather than per-peer counters; present the server as one peer so
    // the UI's traffic graph works the same for every tunnel kind.
    PeerRuntime p;
    p.publicKey = spec_.openVpn ? "openvpn" : "";
    p.rxBytes = rx_;
    p.txBytes = tx_;
    p.lastHandshake = (phase_ == TunnelPhase::Up) ? since_ : 0;
    s.peers.push_back(std::move(p));
    return s;
}

}  // namespace tunhub
