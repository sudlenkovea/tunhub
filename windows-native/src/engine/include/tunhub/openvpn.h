#pragma once
// OpenVPN session: spawns openvpn.exe and drives it over the management interface.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tunhub/log.h"
#include "tunhub/models.h"
#include "tunhub/proc.h"

namespace tunhub {

class OpenVpnSession {
public:
    OpenVpnSession(FileLog& log, ResolvedTunnelSpec spec);
    ~OpenVpnSession();

    OpenVpnSession(const OpenVpnSession&) = delete;
    OpenVpnSession& operator=(const OpenVpnSession&) = delete;

    bool start(std::string* error);
    void stop();

    TunnelRuntimeState snapshot() const;
    bool finished() const { return finished_; }

private:
    void onCoreOutput(const std::string& line);
    void managementLoop();
    void handleManagementLine(const std::string& line);
    void send(const std::string& command);
    void sendCredentials();
    void writeConfigFile();
    void cleanupFiles();

    FileLog& log_;
    ResolvedTunnelSpec spec_;
    ChildProcess process_;
    std::string configPath_;
    std::string managementPasswordPath_;
    std::string managementPassword_;
    unsigned short managementPort_ = 0;

    void* socket_ = nullptr;              // SOCKET to the management interface
    std::thread managementThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> finished_{false};

    mutable std::mutex mutex_;
    TunnelPhase phase_ = TunnelPhase::Starting;
    std::string error_;
    std::string adapter_;
    int64_t since_ = 0;
    uint64_t rx_ = 0, tx_ = 0;
    std::vector<std::string> pushedRoutes_;
    /// Set when the server rejects our credentials, so the UI can re-prompt instead of
    /// looping forever (openvpn is started with `auth-retry none`).
    bool authFailed_ = false;
};

}  // namespace tunhub
