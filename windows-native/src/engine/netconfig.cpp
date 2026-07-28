#include "tunhub/netconfig.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <climits>   // ULONG_MAX

#include "tunhub/proc.h"
#include "tunhub/str.h"

#pragma comment(lib, "iphlpapi.lib")

namespace tunhub {
namespace {

/// Firewall rule prefix, so we can find and remove exactly our own rules.
constexpr const char* kRulePrefix = "TunHub-KS-";

bool isPhysicalIfType(DWORD type) {
    // Anything tunnel-like is disqualified as an "uplink": another VPN's adapter must never
    // be chosen as the path for our own outer packets.
    return type != IF_TYPE_TUNNEL && type != IF_TYPE_PROP_VIRTUAL && type != IF_TYPE_SOFTWARE_LOOPBACK;
}

std::string sockaddrToString(const SOCKADDR_INET& addr) {
    char buf[INET6_ADDRSTRLEN]{};
    if (addr.si_family == AF_INET)
        inet_ntop(AF_INET, &addr.Ipv4.sin_addr, buf, sizeof(buf));
    else if (addr.si_family == AF_INET6)
        inet_ntop(AF_INET6, &addr.Ipv6.sin6_addr, buf, sizeof(buf));
    return buf;
}

std::string netshExe() { return "netsh.exe"; }

}  // namespace

// ── gateway discovery ────────────────────────────────────────────────────────

Gateway NetConfig::physicalGateway(bool v6) {
    Gateway best;
    unsigned long bestMetric = ULONG_MAX;

    PMIB_IPFORWARD_TABLE2 table = nullptr;
    if (GetIpForwardTable2(v6 ? AF_INET6 : AF_INET, &table) != NO_ERROR || !table) return best;

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& row = table->Table[i];
        if (row.DestinationPrefix.PrefixLength != 0) continue;      // default routes only

        MIB_IF_ROW2 ifRow{};
        ifRow.InterfaceLuid = row.InterfaceLuid;
        if (GetIfEntry2(&ifRow) != NO_ERROR) continue;
        if (!isPhysicalIfType(ifRow.Type)) continue;                 // skip VPN adapters
        if (ifRow.OperStatus != IfOperStatusUp) continue;

        const unsigned long metric = row.Metric + ifRow.Metric;
        if (metric >= bestMetric) continue;

        bestMetric = metric;
        best.address = sockaddrToString(row.NextHop);
        best.ifIndex = row.InterfaceIndex;
        best.ifName = str::narrow(ifRow.Alias);
    }
    FreeMibTable(table);
    return best;
}

unsigned long NetConfig::interfaceIndex(const std::string& adapterName) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return 0;
    unsigned long index = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto alias = str::narrow(table->Table[i].Alias);
        if (str::iequals(alias, adapterName)) { index = table->Table[i].InterfaceIndex; break; }
    }
    FreeMibTable(table);
    return index;
}

// ── addressing ───────────────────────────────────────────────────────────────

bool NetConfig::configureInterface(const std::string& adapter,
                                   const std::vector<IpAddressRange>& addresses,
                                   const std::optional<int>& mtu, std::string* error) {
    const unsigned long index = interfaceIndex(adapter);
    if (index == 0) {
        if (error) *error = "adapter " + adapter + " not found";
        return false;
    }

    for (const auto& a : addresses) {
        const auto family = a.isV6 ? "ipv6" : "ipv4";
        // `netsh` is used rather than the Set*IpAddressEntry APIs: it handles the
        // add-or-replace semantics and the IPv6 cases uniformly, and its errors are readable.
        auto r = runCommand(netshExe(), {"interface", family, "set", "address",
                                         "name=" + std::to_string(index),
                                         "source=static",
                                         "address=" + a.canonical()});
        if (!r.ok()) {
            // set fails when the address already exists; adding is the idempotent path.
            r = runCommand(netshExe(), {"interface", family, "add", "address",
                                        std::to_string(index), a.canonical()});
        }
        if (!r.ok()) {
            if (error) *error = "failed to set address " + a.canonical() + ": " + str::trim(r.output);
            return false;
        }
    }

    if (mtu) {
        runCommand(netshExe(), {"interface", "ipv4", "set", "subinterface",
                                std::to_string(index), "mtu=" + std::to_string(*mtu),
                                "store=active"});
    }
    return true;
}

// ── routing ──────────────────────────────────────────────────────────────────

bool NetConfig::addRoute(const IpAddressRange& r, unsigned long ifIndex,
                         const std::string& gateway) {
    const auto family = r.isV6 ? "ipv6" : "ipv4";
    std::vector<std::string> args{"interface", family, "add", "route",
                                  r.canonical(), std::to_string(ifIndex)};
    if (!gateway.empty()) args.push_back(gateway);
    args.push_back("store=active");        // never persist: a crash must not outlive us
    auto res = runCommand(netshExe(), args);
    return res.ok();
}

bool NetConfig::deleteRoute(const IpAddressRange& r, unsigned long ifIndex) {
    const auto family = r.isV6 ? "ipv6" : "ipv4";
    auto res = runCommand(netshExe(), {"interface", family, "delete", "route",
                                       r.canonical(), std::to_string(ifIndex),
                                       "store=active"});
    return res.ok();
}

bool NetConfig::applyRoutes(const std::string& tunnelId, const std::string& adapter,
                            const std::vector<IpAddressRange>& routes,
                            const std::vector<std::string>& endpointHosts, std::string* error) {
    const unsigned long index = interfaceIndex(adapter);
    if (index == 0) {
        if (error) *error = "adapter " + adapter + " not found";
        return false;
    }

    std::vector<RouteEntry> journal;

    // 1. Pin the endpoints to the physical uplink BEFORE any tunnel route exists, otherwise a
    //    default route we are about to install would swallow the core's own outer packets.
    for (const auto& host : endpointHosts) {
        auto parsed = IpAddressRange::parse(host);
        if (!parsed) continue;
        const auto gw = physicalGateway(parsed->isV6);
        if (!gw.valid()) {
            log_.warn("route", "pin endpoint " + host + ": no physical gateway found — skipping");
            continue;
        }
        if (addRoute(*parsed, gw.ifIndex, gw.address)) {
            log_.info("route", "pin endpoint " + host + " → gw=" + gw.address +
                                   " iface=" + gw.ifName);
            journal.push_back({parsed->canonical(), gw.ifIndex, true});
        } else {
            log_.warn("route", "pin endpoint " + host + " failed (it may already be pinned)");
        }
    }

    // 2. Tunnel routes. A default route becomes the /1 pair: more specific than the existing
    //    default, so it wins, but the original stays in place for rollback.
    std::vector<IpAddressRange> expanded;
    for (const auto& r : routes) {
        if (r.prefix == 0) {
            const char* halves[2] = {"0.0.0.0/1", "128.0.0.0/1"};
            const char* halves6[2] = {"::/1", "8000::/1"};
            for (int i = 0; i < 2; ++i)
                if (auto half = IpAddressRange::parse(r.isV6 ? halves6[i] : halves[i]))
                    expanded.push_back(*half);
        } else {
            expanded.push_back(r);
        }
    }

    for (const auto& r : expanded) {
        if (addRoute(r, index, {})) {
            journal.push_back({r.canonical(), index, false});
        } else {
            log_.warn("route", "route add " + r.canonical() + " failed");
        }
    }
    journal_[tunnelId] = std::move(journal);
    log_.info("route", "installed " + std::to_string(expanded.size()) + " route(s) on " + adapter);

    // 3. Verify the endpoints did not end up inside the tunnel.
    ensureEndpointNotLooped(tunnelId, endpointHosts, adapter);
    return true;
}

void NetConfig::rollbackRoutes(const std::string& tunnelId) {
    auto it = journal_.find(tunnelId);
    if (it == journal_.end()) return;
    // Reverse order so pins (added first) are removed last.
    for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry) {
        if (auto r = IpAddressRange::parse(entry->destination)) deleteRoute(*r, entry->ifIndex);
    }
    journal_.erase(it);
}

bool NetConfig::ensureEndpointNotLooped(const std::string& tunnelId,
                                        const std::vector<std::string>& endpointHosts,
                                        const std::string& adapter) {
    const unsigned long tunnelIndex = interfaceIndex(adapter);
    if (tunnelIndex == 0) return false;
    bool fixedAny = false;

    for (const auto& host : endpointHosts) {
        auto parsed = IpAddressRange::parse(host);
        if (!parsed) continue;

        // Ask the stack which interface it would actually use for this destination.
        SOCKADDR_INET dst{};
        if (parsed->isV6) {
            dst.si_family = AF_INET6;
            inet_pton(AF_INET6, parsed->address.c_str(), &dst.Ipv6.sin6_addr);
        } else {
            dst.si_family = AF_INET;
            inet_pton(AF_INET, parsed->address.c_str(), &dst.Ipv4.sin_addr);
        }
        MIB_IPFORWARD_ROW2 best{};
        SOCKADDR_INET bestSource{};
        if (GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &best, &bestSource) != NO_ERROR) continue;
        if (best.InterfaceIndex != tunnelIndex) continue;    // not looped — fine

        const auto gw = physicalGateway(parsed->isV6);
        if (!gw.valid()) {
            log_.error("route", "endpoint " + host +
                                    " is routed into the tunnel and no physical gateway is available");
            continue;
        }
        deleteRoute(*parsed, tunnelIndex);
        if (addRoute(*parsed, gw.ifIndex, gw.address)) {
            log_.warn("route", "LOOP fixed: endpoint " + host + " re-pinned via gw=" +
                                   gw.address + " iface=" + gw.ifName);
            journal_[tunnelId].push_back({parsed->canonical(), gw.ifIndex, true});
            fixedAny = true;
        }
    }
    return fixedAny;
}

// ── DNS ──────────────────────────────────────────────────────────────────────

bool NetConfig::applyDns(const std::string& tunnelId, const std::string& adapter,
                         const std::vector<std::string>& servers,
                         const std::vector<std::string>& searchDomains,
                         const DnsMode& mode, std::string* error) {
    (void)error;
    if (mode.kind == DnsModeKind::Disabled || servers.empty()) return true;

    const unsigned long index = interfaceIndex(adapter);
    if (index == 0) return false;

    if (mode.kind == DnsModeKind::Global) {
        // Set the resolvers on the tunnel adapter itself; Windows prefers it because the
        // tunnel holds the (more specific) default route.
        bool first = true;
        for (const auto& s : servers) {
            const auto family = s.find(':') != std::string::npos ? "ipv6" : "ipv4";
            if (first) {
                runCommand(netshExe(), {"interface", family, "set", "dnsservers",
                                        "name=" + std::to_string(index), "static", s,
                                        "primary", "no"});
                first = false;
            } else {
                runCommand(netshExe(), {"interface", family, "add", "dnsservers",
                                        "name=" + std::to_string(index), s, "index=2"});
            }
        }
        log_.info("dns", "global DNS " + str::join(servers, ", ") + " on " + adapter);
        return true;
    }

    // Split DNS: NRPT rules route only the listed suffixes to the tunnel's resolvers, leaving
    // everything else on the system resolver. This is what lets two split tunnels coexist.
    std::vector<std::string> ruleNames;
    for (const auto& domain : (searchDomains.empty() ? mode.matchDomains : searchDomains)) {
        const auto suffix = str::startsWith(domain, ".") ? domain : "." + domain;
        const auto comment = std::string("TunHub-") + tunnelId;
        auto r = runCommand("powershell.exe",
                            {"-NoProfile", "-NonInteractive", "-Command",
                             "Add-DnsClientNrptRule -Namespace '" + suffix + "' -NameServers '" +
                                 str::join(servers, ",") + "' -Comment '" + comment + "'"});
        if (r.ok()) ruleNames.push_back(suffix);
        else log_.warn("dns", "NRPT rule for " + suffix + " failed: " + str::trim(r.output));
    }
    if (!ruleNames.empty()) {
        nrpt_[tunnelId] = ruleNames;
        log_.info("dns", "split DNS for " + str::join(ruleNames, ", ") + " → " +
                             str::join(servers, ", "));
    }
    return true;
}

void NetConfig::rollbackDns(const std::string& tunnelId) {
    if (auto it = nrpt_.find(tunnelId); it != nrpt_.end()) {
        runCommand("powershell.exe",
                   {"-NoProfile", "-NonInteractive", "-Command",
                    "Get-DnsClientNrptRule | Where-Object { $_.Comment -eq 'TunHub-" + tunnelId +
                        "' } | Remove-DnsClientNrptRule -Force"});
        nrpt_.erase(it);
    }
    // Adapter-level resolvers disappear with the adapter when the core exits, so there is
    // nothing to undo for the global case.
}

// ── kill switch ──────────────────────────────────────────────────────────────

void NetConfig::rebuildKillSwitch(const std::vector<ActiveTunnel>& tunnels) {
    disableKillSwitch();
    if (tunnels.empty()) return;

    // Block all outbound traffic, then re-allow: the tunnel adapters themselves, the endpoints
    // we must reach to keep the tunnels alive, LAN, and loopback. Without the endpoint
    // exceptions the rules would cut off the very handshake that sustains the tunnel.
    runCommand(netshExe(), {"advfirewall", "firewall", "add", "rule",
                            std::string("name=") + kRulePrefix + "BlockAll",
                            "dir=out", "action=block", "enable=yes", "profile=any"});

    for (const auto& t : tunnels) {
        const unsigned long index = interfaceIndex(t.adapter);
        if (index != 0) {
            runCommand(netshExe(), {"advfirewall", "firewall", "add", "rule",
                                    std::string("name=") + kRulePrefix + "Allow-" + t.adapter,
                                    "dir=out", "action=allow", "enable=yes", "profile=any",
                                    "interfacetype=any"});
        }
        for (const auto& host : t.endpointHosts) {
            runCommand(netshExe(), {"advfirewall", "firewall", "add", "rule",
                                    std::string("name=") + kRulePrefix + "Endpoint-" + host,
                                    "dir=out", "action=allow", "enable=yes", "profile=any",
                                    "remoteip=" + host});
        }
    }
    // Local networks and loopback stay reachable so the machine remains manageable.
    for (const auto& lan : {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "127.0.0.0/8",
                            "224.0.0.0/4"}) {
        runCommand(netshExe(), {"advfirewall", "firewall", "add", "rule",
                                std::string("name=") + kRulePrefix + "LAN-" + lan,
                                "dir=out", "action=allow", "enable=yes", "profile=any",
                                std::string("remoteip=") + lan});
    }
    log_.info("firewall", "kill switch active for " + std::to_string(tunnels.size()) + " tunnel(s)");
}

void NetConfig::disableKillSwitch() {
    // netsh removes every rule sharing a name; ours all carry the same prefix, so delete by
    // enumerating the known shapes rather than wildcarding (netsh has no wildcard delete).
    runCommand("powershell.exe",
               {"-NoProfile", "-NonInteractive", "-Command",
                std::string("Get-NetFirewallRule -DisplayName '") + kRulePrefix +
                    "*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule"});
}

void NetConfig::crashRecovery() {
    disableKillSwitch();
    runCommand("powershell.exe",
               {"-NoProfile", "-NonInteractive", "-Command",
                "Get-DnsClientNrptRule | Where-Object { $_.Comment -like 'TunHub-*' } | "
                "Remove-DnsClientNrptRule -Force"});
    log_.debug("recover", "cleared stale firewall rules and NRPT entries");
}

}  // namespace tunhub
