#pragma once
// Core-process lifecycle plus route/DNS/firewall orchestration.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tunhub/log.h"
#include "tunhub/models.h"
#include "tunhub/netconfig.h"
#include "tunhub/openvpn.h"
#include "tunhub/proc.h"

namespace tunhub {

class TunnelSupervisor {
public:
    explicit TunnelSupervisor(FileLog& log);
    ~TunnelSupervisor();

    TunnelSupervisor(const TunnelSupervisor&) = delete;
    TunnelSupervisor& operator=(const TunnelSupervisor&) = delete;

    bool start(const ResolvedTunnelSpec& spec, std::string* error);
    void stop(const std::string& id);
    void stopAll();

    std::vector<TunnelRuntimeState> states() const;

    void setKillSwitchEnabled(bool enabled);
    bool killSwitchEnabled() const { return killSwitchEnabled_; }

    void startStatsLoop();

    /// Kill cores left behind by a previous run and clear stale network state. Runs once at
    /// service start, before anything is spawned, so every match is by definition an orphan.
    void crashRecovery();

private:
    struct RunningTunnel {
        ResolvedTunnelSpec spec;
        std::unique_ptr<ChildProcess> process;
        std::string adapter;
        std::wstring pipe;
        std::vector<std::string> endpointHosts;
        std::map<int, std::string> resolvedEndpoints;
        TunnelPhase phase = TunnelPhase::Starting;
        std::string lastError;
        std::vector<PeerRuntime> peers;
        int64_t since = 0;
        uint64_t lastRx = 0, lastTx = 0;
        int64_t stalledSince = 0;
        int64_t lastLoopCheck = 0;
        bool intentionalStop = false;

        bool isDefaultRoute() const;
    };

    bool startWireGuardFamily(const ResolvedTunnelSpec& spec, std::string* error);
    bool startOpenVpn(const ResolvedTunnelSpec& spec, std::string* error);
    void teardown(RunningTunnel& t);
    void pollStats();
    void rebuildKillSwitchLocked();
    void persistOwnershipLocked();

    /// Resolve every peer endpoint to an IP before the tunnel's routes exist — otherwise the
    /// lookup can be swallowed by the tunnel we are about to bring up.
    std::map<int, std::string> resolveEndpoints(const ResolvedTunnelSpec& spec,
                                                std::vector<std::string>& hostsOut) const;

    FileLog& log_;
    NetConfig net_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<RunningTunnel>> running_;
    std::map<std::string, std::unique_ptr<OpenVpnSession>> openvpn_;
    std::map<std::string, TunnelRuntimeState> failed_;
    std::map<std::string, std::pair<std::string, int64_t>> starting_;   // id → (name, since)

    std::thread statsThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> killSwitchEnabled_{true};
};

}  // namespace tunhub
