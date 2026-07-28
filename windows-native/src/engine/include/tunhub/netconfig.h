#pragma once
// Interface addressing, routing, DNS and the kill switch.

#include <map>
#include <string>
#include <vector>

#include "tunhub/log.h"
#include "tunhub/models.h"

namespace tunhub {

struct Gateway {
    std::string address;      // next hop, e.g. "192.168.1.1"
    unsigned long ifIndex = 0;
    std::string ifName;
    bool valid() const { return ifIndex != 0; }
};

class NetConfig {
public:
    explicit NetConfig(FileLog& log) : log_(log) {}

    /// The REAL uplink gateway.
    ///
    /// Deliberately not "whatever currently owns the default route": another VPN can install
    /// a lower-metric default via its own virtual gateway (often CGNAT 100.64/10), and pinning
    /// a full-tunnel endpoint through that black-holes the handshake. We pick the best route
    /// whose interface is physical.
    Gateway physicalGateway(bool v6 = false);

    /// Interface index for an adapter name (the core creates the adapter, Windows assigns it).
    unsigned long interfaceIndex(const std::string& adapterName);

    bool configureInterface(const std::string& adapter,
                            const std::vector<IpAddressRange>& addresses,
                            const std::optional<int>& mtu, std::string* error);

    /// Install the tunnel's routes. A 0.0.0.0/0 route is split into the /1 pair so the
    /// original default stays intact underneath and rollback can't strand the machine.
    /// Endpoints are pinned to the physical gateway FIRST, before any tunnel route exists.
    bool applyRoutes(const std::string& tunnelId, const std::string& adapter,
                     const std::vector<IpAddressRange>& routes,
                     const std::vector<std::string>& endpointHosts, std::string* error);

    void rollbackRoutes(const std::string& tunnelId);

    /// Re-pin an endpoint that ended up routed into the tunnel itself (the loop that makes a
    /// full tunnel go silent). Returns true when a loop was found and corrected.
    bool ensureEndpointNotLooped(const std::string& tunnelId,
                                 const std::vector<std::string>& endpointHosts,
                                 const std::string& adapter);

    bool applyDns(const std::string& tunnelId, const std::string& adapter,
                  const std::vector<std::string>& servers,
                  const std::vector<std::string>& searchDomains,
                  const DnsMode& mode, std::string* error);
    void rollbackDns(const std::string& tunnelId);

    struct ActiveTunnel {
        std::string adapter;
        std::vector<std::string> endpointHosts;
    };
    /// Rebuild the firewall rules for the currently protected tunnels (empty = disable).
    void rebuildKillSwitch(const std::vector<ActiveTunnel>& tunnels);
    void disableKillSwitch();

    /// Clear anything a previous run left behind (rules, NRPT entries, pinned host routes).
    void crashRecovery();

private:
    struct RouteEntry {
        std::string destination;    // canonical CIDR, or "host:<ip>" for a pin
        unsigned long ifIndex = 0;
        bool isPin = false;
    };

    bool addRoute(const IpAddressRange& r, unsigned long ifIndex, const std::string& gateway);
    bool deleteRoute(const IpAddressRange& r, unsigned long ifIndex);

    FileLog& log_;
    std::map<std::string, std::vector<RouteEntry>> journal_;   // tunnel id → routes to undo
    std::map<std::string, std::vector<std::string>> nrpt_;     // tunnel id → NRPT rule names
};

}  // namespace tunhub
