#include "tunhub/uapi.h"

#include <windows.h>

#include <cstdlib>   // strtoull / strtoll

#include "tunhub/str.h"
#include "tunhub/wgkey.h"

namespace tunhub::uapi {
namespace {

/// Append "key=value" when the optional holds a value.
template <typename T>
void put(std::string& out, const char* key, const std::optional<T>& v) {
    if (v) out += std::string(key) + "=" + std::to_string(*v) + "\n";
}

void putStr(std::string& out, const char* key, const std::optional<std::string>& v) {
    if (v && !v->empty()) out += std::string(key) + "=" + *v + "\n";
}

}  // namespace

std::wstring pipeName(const std::string& interfaceName, TunnelKind kind) {
    // amneziawg-go serves \\.\pipe\ProtectedPrefix\Administrators\AmneziaWG\<iface>, while
    // wireguard-go uses ...\WireGuard\<iface>. Using the wrong prefix means the pipe never
    // appears and the tunnel dies on a timeout with no other symptom.
    const wchar_t* family = (kind == TunnelKind::AmneziaWg) ? L"AmneziaWG" : L"WireGuard";
    return std::wstring(L"\\\\.\\pipe\\ProtectedPrefix\\Administrators\\") + family + L"\\" +
           str::widen(interfaceName);
}

bool waitForPipe(const std::wstring& pipe, int timeoutMs) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
    while (GetTickCount() < deadline) {
        if (WaitNamedPipeW(pipe.c_str(), 100)) return true;
        // ERROR_FILE_NOT_FOUND simply means the core hasn't created it yet.
        Sleep(100);
    }
    return false;
}

std::optional<std::string> request(const std::wstring& pipe, const std::string& body,
                                   std::string* error) {
    HANDLE h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open UAPI pipe (error " + std::to_string(GetLastError()) + ")";
        return std::nullopt;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

    DWORD written = 0;
    if (!WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &written, nullptr)) {
        if (error) *error = "UAPI write failed";
        CloseHandle(h);
        return std::nullopt;
    }

    std::string response;
    char buf[4096];
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        response.append(buf, read);
        // The protocol terminates a response with a blank line.
        if (response.size() >= 2 && response.compare(response.size() - 2, 2, "\n\n") == 0) break;
    }
    CloseHandle(h);
    return response;
}

bool set(const std::wstring& pipe, const std::string& config, std::string* error) {
    auto response = request(pipe, config, error);
    if (!response) return false;
    for (const auto& line : str::split(*response, '\n')) {
        if (!str::startsWith(line, "errno=")) continue;
        if (line == "errno=0") return true;
        if (error) *error = "uapi set failed: " + line;
        return false;
    }
    if (error) *error = "uapi set: no errno in response";
    return false;
}

std::vector<PeerRuntime> get(const std::wstring& pipe) {
    std::vector<PeerRuntime> peers;
    auto response = request(pipe, "get=1\n\n", nullptr);
    if (!response) return peers;

    std::optional<PeerRuntime> current;
    for (const auto& line : str::split(*response, '\n')) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);

        if (key == "public_key") {
            if (current) peers.push_back(*current);
            current = PeerRuntime{};
            auto b64 = wgkey::hexToBase64(value);
            current->publicKey = b64.empty() ? value : b64;
        } else if (!current) {
            continue;                                  // device-level fields
        } else if (key == "endpoint") {
            current->endpoint = value;
        } else if (key == "rx_bytes") {
            current->rxBytes = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "tx_bytes") {
            current->txBytes = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "last_handshake_time_sec") {
            const auto sec = std::strtoll(value.c_str(), nullptr, 10);
            if (sec > 0) current->lastHandshake = sec;
        }
    }
    if (current) peers.push_back(*current);
    return peers;
}

std::string renderSetRequest(const ResolvedTunnelSpec& spec,
                             const std::map<int, std::string>& resolvedEndpoints,
                             std::string* error) {
    const auto privHex = wgkey::base64ToHex(spec.privateKey);
    if (privHex.empty()) {
        if (error) *error = "bad private key";
        return {};
    }

    std::string out = "set=1\n";
    out += "private_key=" + privHex + "\n";
    if (spec.listenPort) out += "listen_port=" + std::to_string(*spec.listenPort) + "\n";

    if (spec.kind == TunnelKind::AmneziaWg && spec.awg && !spec.awg->empty()) {
        const auto& a = *spec.awg;
        put(out, "jc", a.jc);
        put(out, "jmin", a.jmin);
        put(out, "jmax", a.jmax);
        put(out, "s1", a.s1);
        put(out, "s2", a.s2);
        put(out, "s3", a.s3);
        put(out, "s4", a.s4);
        // Magic headers. The core parses these as ranges ("N" or "N-M") and, unlike jc/s1,
        // does not log applying them — so a missing header here is invisible in the core log
        // and shows up only as a handshake the server never answers.
        put(out, "h1", a.h1);
        put(out, "h2", a.h2);
        put(out, "h3", a.h3);
        put(out, "h4", a.h4);
        putStr(out, "i1", a.i1);
        putStr(out, "i2", a.i2);
        putStr(out, "i3", a.i3);
        putStr(out, "i4", a.i4);
        putStr(out, "i5", a.i5);
        put(out, "itime", a.itime);
    }

    out += "replace_peers=true\n";
    for (size_t i = 0; i < spec.peers.size(); ++i) {
        const auto& p = spec.peers[i];
        const auto pubHex = wgkey::base64ToHex(p.publicKey);
        if (pubHex.empty()) {
            if (error) *error = "bad peer public key";
            return {};
        }
        out += "public_key=" + pubHex + "\n";
        if (p.presharedKey) {
            const auto pskHex = wgkey::base64ToHex(*p.presharedKey);
            if (!pskHex.empty()) out += "preshared_key=" + pskHex + "\n";
        }
        if (auto it = resolvedEndpoints.find(static_cast<int>(i)); it != resolvedEndpoints.end())
            out += "endpoint=" + it->second + "\n";
        out += "replace_allowed_ips=true\n";
        for (const auto& r : p.allowedIPs) out += "allowed_ip=" + r.canonical() + "\n";
        if (p.keepalive && *p.keepalive > 0)
            out += "persistent_keepalive_interval=" + std::to_string(*p.keepalive) + "\n";
    }
    out += "\n";
    if (error) error->clear();
    return out;
}

}  // namespace tunhub::uapi
