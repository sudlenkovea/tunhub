#pragma once
// Tunnel persistence and secret storage.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "tunhub/models.h"

namespace tunhub::app {

/// Secrets for one tunnel. Never written into the config file.
struct TunnelSecrets {
    std::string privateKey;
    std::map<std::string, std::string> presharedKeys;   // peer id → PSK
    std::string openVpnUsername;
    std::string openVpnPassword;
};

/// Settings that belong to the person using the machine, not to the tunnels.
struct AppSettings {
    std::string language = "system";   // "system" | "en" | "ru"
    bool launchAtLogin = false;
    bool killSwitchGlobal = true;

    void load();
    void save() const;
};

class Store {
public:
    /// Configs live in %ProgramData%\TunHub\tunnels so the service can read them too.
    std::vector<TunnelConfig> loadTunnels() const;
    bool saveTunnel(const TunnelConfig& config, std::string* error) const;
    bool deleteTunnel(const std::string& id) const;

    /// Secrets are encrypted with DPAPI under the local machine key, so the service can read
    /// them as well; the ciphertext is useless on any other machine.
    std::optional<TunnelSecrets> loadSecrets(const std::string& tunnelId) const;
    bool saveSecrets(const std::string& tunnelId, const TunnelSecrets& secrets) const;
    void deleteSecrets(const std::string& tunnelId) const;

    /// Config + secrets → the spec handed to the service.
    std::optional<ResolvedTunnelSpec> resolve(const TunnelConfig& config,
                                              const std::string& otp,
                                              std::string* error) const;

private:
    std::string tunnelPath(const std::string& id) const;
    std::string secretsPath(const std::string& id) const;
};

}  // namespace tunhub::app
