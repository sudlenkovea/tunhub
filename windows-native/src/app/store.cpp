#include "store.h"

#include <windows.h>
#include <dpapi.h>

#include <filesystem>
#include <fstream>

#include "tunhub/paths.h"
#include "tunhub/str.h"
#include "tunhub/util.h"

#pragma comment(lib, "crypt32.lib")

namespace tunhub::app {
namespace {

std::optional<std::string> readFile(const std::string& path) {
    std::ifstream f(std::filesystem::path(str::widen(path)), std::ios::binary);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool writeFileAtomic(const std::string& path, const std::string& content) {
    const auto tmp = path + ".tmp";
    {
        std::ofstream f(std::filesystem::path(str::widen(tmp)),
                        std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(str::widen(tmp)),
                            std::filesystem::path(str::widen(path)), ec);
    if (ec) {
        std::filesystem::remove(std::filesystem::path(str::widen(tmp)), ec);
        return false;
    }
    return true;
}

/// DPAPI with the machine scope: the service (LocalSystem) must be able to read what the UI
/// wrote, and vice versa. The ciphertext is bound to this machine.
bool protect(const std::string& plain, std::string& out) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB result{};
    if (!CryptProtectData(&in, L"TunHub", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE, &result))
        return false;
    out.assign(reinterpret_cast<char*>(result.pbData), result.cbData);
    LocalFree(result.pbData);
    return true;
}

bool unprotect(const std::string& cipher, std::string& out) {
    DATA_BLOB in{static_cast<DWORD>(cipher.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(cipher.data()))};
    DATA_BLOB result{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &result)) return false;
    out.assign(reinterpret_cast<char*>(result.pbData), result.cbData);
    SecureZeroMemory(result.pbData, result.cbData);
    LocalFree(result.pbData);
    return true;
}

}  // namespace

// ── AppSettings ──────────────────────────────────────────────────────────────

void AppSettings::load() {
    auto text = readFile(paths::settingsFile());
    if (!text) return;
    std::string err;
    const Json j = Json::parse(*text, &err);
    if (!err.empty()) return;
    language = j["language"].asString("system");
    launchAtLogin = j["launchAtLogin"].asBool(false);
    killSwitchGlobal = j["killSwitchGlobal"].asBool(true);
}

void AppSettings::save() const {
    Json j = Json::object();
    j.set("language", Json(language));
    j.set("launchAtLogin", Json(launchAtLogin));
    j.set("killSwitchGlobal", Json(killSwitchGlobal));
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(str::widen(paths::settingsFile())).parent_path(), ec);
    writeFileAtomic(paths::settingsFile(), j.dump(2));
}

// ── Store ────────────────────────────────────────────────────────────────────

std::string Store::tunnelPath(const std::string& id) const {
    return paths::tunnelsDir() + "\\" + id + ".json";
}

std::string Store::secretsPath(const std::string& id) const {
    return paths::stateDir() + "\\" + id + ".secret";
}

std::vector<TunnelConfig> Store::loadTunnels() const {
    std::vector<TunnelConfig> out;
    std::error_code ec;
    const auto dir = std::filesystem::path(str::widen(paths::tunnelsDir()));
    if (!std::filesystem::exists(dir, ec)) return out;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != L".json") continue;
        auto text = readFile(str::narrow(entry.path().wstring()));
        if (!text) continue;
        std::string err;
        const Json j = Json::parse(*text, &err);
        if (!err.empty()) continue;
        auto cfg = TunnelConfig::fromJson(j);
        if (!cfg.id.empty()) out.push_back(std::move(cfg));
    }
    std::sort(out.begin(), out.end(), [](const TunnelConfig& a, const TunnelConfig& b) {
        if (a.meta.sortOrder != b.meta.sortOrder) return a.meta.sortOrder < b.meta.sortOrder;
        return a.name < b.name;
    });
    return out;
}

bool Store::saveTunnel(const TunnelConfig& config, std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(str::widen(paths::tunnelsDir())), ec);
    if (!writeFileAtomic(tunnelPath(config.id), config.toJson().dump(2))) {
        if (error) *error = "could not write the tunnel configuration";
        return false;
    }
    return true;
}

bool Store::deleteTunnel(const std::string& id) const {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(str::widen(tunnelPath(id))), ec);
    deleteSecrets(id);
    return !ec;
}

std::optional<TunnelSecrets> Store::loadSecrets(const std::string& tunnelId) const {
    auto cipher = readFile(secretsPath(tunnelId));
    if (!cipher) return std::nullopt;
    std::string plain;
    if (!unprotect(*cipher, plain)) return std::nullopt;

    std::string err;
    const Json j = Json::parse(plain, &err);
    SecureZeroMemory(plain.data(), plain.size());
    if (!err.empty()) return std::nullopt;

    TunnelSecrets s;
    s.privateKey = j["privateKey"].asString("");
    for (const auto& [peerId, value] : j["presharedKeys"].fields())
        s.presharedKeys[peerId] = value.asString("");
    s.openVpnUsername = j["openVpnUsername"].asString("");
    s.openVpnPassword = j["openVpnPassword"].asString("");
    return s;
}

bool Store::saveSecrets(const std::string& tunnelId, const TunnelSecrets& secrets) const {
    Json j = Json::object();
    j.set("privateKey", Json(secrets.privateKey));
    Json psks = Json::object();
    for (const auto& [peerId, psk] : secrets.presharedKeys) psks.set(peerId, Json(psk));
    j.set("presharedKeys", psks);
    j.set("openVpnUsername", Json(secrets.openVpnUsername));
    j.set("openVpnPassword", Json(secrets.openVpnPassword));

    auto plain = j.dump();
    std::string cipher;
    const bool ok = protect(plain, cipher);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) return false;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(str::widen(paths::stateDir())), ec);
    return writeFileAtomic(secretsPath(tunnelId), cipher);
}

void Store::deleteSecrets(const std::string& tunnelId) const {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(str::widen(secretsPath(tunnelId))), ec);
}

std::optional<ResolvedTunnelSpec> Store::resolve(const TunnelConfig& config,
                                                 const std::string& otp,
                                                 std::string* error) const {
    auto secrets = loadSecrets(config.id);
    if (!secrets) {
        if (error) *error = "credentials for this tunnel are missing — re-import it";
        return std::nullopt;
    }

    ResolvedTunnelSpec spec;
    spec.id = config.id;
    spec.name = config.name;
    spec.kind = config.kind;
    spec.killSwitch = config.options.killSwitch;

    if (config.kind == TunnelKind::OpenVpn) {
        if (!config.openVpn) {
            if (error) *error = "the OpenVPN profile is missing";
            return std::nullopt;
        }
        ResolvedOpenVpn o;
        o.configText = config.openVpn->configText;
        o.username = secrets->openVpnUsername;
        o.password = secrets->openVpnPassword;
        o.otp = otp;
        spec.openVpn = std::move(o);
        spec.dnsServers = config.openVpn->dns;
        spec.dnsSearchDomains = config.openVpn->searchDomains;
        return spec;
    }

    if (secrets->privateKey.empty()) {
        if (error) *error = "the private key for this tunnel is missing";
        return std::nullopt;
    }
    spec.privateKey = secrets->privateKey;
    spec.addresses = config.iface.addresses;
    spec.listenPort = config.iface.listenPort;
    spec.mtu = config.iface.mtu;
    spec.dnsServers = config.iface.dns;
    spec.dnsSearchDomains = config.iface.dnsSearchDomains;
    spec.dnsMode = config.effectiveDnsMode();
    spec.routes = config.effectiveRoutes();
    spec.awg = config.awg;

    for (const auto& p : config.peers) {
        ResolvedPeer rp;
        rp.publicKey = p.publicKey;
        rp.endpoint = p.endpoint;
        rp.allowedIPs = p.allowedIPs;
        rp.keepalive = p.persistentKeepalive;
        if (auto it = secrets->presharedKeys.find(p.id);
            it != secrets->presharedKeys.end() && !it->second.empty())
            rp.presharedKey = it->second;
        spec.peers.push_back(std::move(rp));
    }
    return spec;
}

}  // namespace tunhub::app
