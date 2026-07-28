#include "tunhub/conflicts.h"

#include <algorithm>

#include "tunhub/str.h"

namespace tunhub::conflicts {
namespace {

/// Does any route cover this bare address?
bool routesCover(const std::vector<IpAddressRange>& routes, const std::string& address) {
    auto host = IpAddressRange::parse(address);
    if (!host) return false;
    return std::any_of(routes.begin(), routes.end(),
                       [&](const IpAddressRange& r) { return r.contains(*host); });
}

/// "example.com" overlaps "sub.example.com" and vice versa.
bool domainOverlap(const std::string& a, const std::string& b) {
    const auto x = str::lower(a), y = str::lower(b);
    if (x == y) return true;
    return str::endsWith(x, "." + y) || str::endsWith(y, "." + x);
}

std::string q(const std::string& s) { return "“" + s + "”"; }   // “name”

void add(std::vector<ConflictFinding>& out, FindingSeverity sev, std::string code,
         std::string message, std::vector<std::string> tunnels, std::string fixHint = {}) {
    out.push_back({sev, std::move(code), std::move(message), std::move(tunnels), std::move(fixHint)});
}

// ── single-tunnel checks ─────────────────────────────────────────────────────

std::vector<ConflictFinding> selfCheck(const TunnelConfig& t) {
    std::vector<ConflictFinding> out;
    const auto routes = t.effectiveRoutes();

    // A resolver outside the tunnel's own routes would be queried over the plain uplink.
    for (const auto& dns : t.iface.dns) {
        if (!routesCover(routes, dns))
            add(out, FindingSeverity::Warning, "DNSUnreachable",
                "DNS " + dns + " of " + q(t.name) +
                    " is not covered by its AllowedIPs — resolution will bypass the tunnel",
                {t.name}, "Add " + dns + "/32 to AllowedIPs or change the DNS");
    }

    // An endpoint inside our own routes is fine — we pin it — but worth surfacing.
    for (const auto& p : t.peers) {
        if (!p.endpoint) continue;
        auto ep = parseEndpoint(*p.endpoint);
        if (!ep || !isIpLiteral(ep->host)) continue;
        if (routesCover(routes, ep->host))
            add(out, FindingSeverity::Info, "EndpointPinned",
                "Endpoint " + ep->host + " falls inside " + q(t.name) +
                    " routes — TunHub will pin it via the physical gateway automatically",
                {t.name});
    }

    if (t.awg)
        for (const auto& e : t.awg->validate())
            add(out, FindingSeverity::Error, "AWGParamInvalid", q(t.name) + ": " + e, {t.name});

    return out;
}

// ── pairwise checks ──────────────────────────────────────────────────────────

std::vector<ConflictFinding> pairCheck(const TunnelConfig& a, const TunnelConfig& b) {
    std::vector<ConflictFinding> out;

    // 1. Both claiming the default route.
    if (a.hasDefaultRoute() && b.hasDefaultRoute())
        add(out, FindingSeverity::Error, "DefaultRouteClash",
            q(a.name) + " and " + q(b.name) +
                " both claim all traffic (default route). They cannot run at the same time.",
            {a.name, b.name},
            "Keep the default route on one; move the other to specific subnets or split DNS");

    // 2/3. Route duplication and shadowing.
    for (const auto& x : a.effectiveRoutes()) {
        for (const auto& y : b.effectiveRoutes()) {
            if (x.prefix <= 1 || y.prefix <= 1) continue;   // default handled above
            if (x.canonical() == y.canonical())
                add(out, FindingSeverity::Error, "ExactDuplicate",
                    "Identical route " + x.canonical() + " in " + q(a.name) + " and " + q(b.name),
                    {a.name, b.name});
            else if (x.contains(y))
                add(out, FindingSeverity::Warning, "SubnetShadowing",
                    y.canonical() + " (" + q(b.name) + ") is nested in " + x.canonical() + " (" +
                        q(a.name) + ") — traffic goes to the more specific " + q(b.name),
                    {a.name, b.name});
            else if (y.contains(x))
                add(out, FindingSeverity::Warning, "SubnetShadowing",
                    x.canonical() + " (" + q(a.name) + ") is nested in " + y.canonical() + " (" +
                        q(b.name) + ") — traffic goes to the more specific " + q(a.name),
                    {a.name, b.name});
        }
    }

    // 4. Interface address overlap.
    for (const auto& x : a.iface.addresses)
        for (const auto& y : b.iface.addresses)
            if (x.overlaps(y))
                add(out, FindingSeverity::Error, "AddressOverlap",
                    "Interface addresses overlap: " + x.canonical() + " (" + q(a.name) + ") and " +
                        y.canonical() + " (" + q(b.name) + ")",
                    {a.name, b.name});

    // 5. ListenPort clash.
    if (a.iface.listenPort && b.iface.listenPort && *a.iface.listenPort == *b.iface.listenPort)
        add(out, FindingSeverity::Error, "ListenPortClash",
            "Same ListenPort " + std::to_string(*a.iface.listenPort) + " on " + q(a.name) +
                " and " + q(b.name),
            {a.name, b.name});

    // 6. Both wanting to own the system resolver (split tunnels don't take global DNS).
    const bool aGlobal = a.effectiveDnsMode().kind == DnsModeKind::Global && !a.iface.dns.empty();
    const bool bGlobal = b.effectiveDnsMode().kind == DnsModeKind::Global && !b.iface.dns.empty();
    if (aGlobal && bGlobal)
        add(out, FindingSeverity::Error, "GlobalDNSClash",
            q(a.name) + " and " + q(b.name) + " both want to be the system's global DNS",
            {a.name, b.name}, "Switch one tunnel to split DNS (by domain) in its settings");

    // 7. Split-DNS domain overlap.
    for (const auto& x : a.options.dnsMode.matchDomains)
        for (const auto& y : b.options.dnsMode.matchDomains)
            if (domainOverlap(x, y))
                add(out, FindingSeverity::Warning, "SplitDomainOverlap",
                    "DNS domains overlap: " + x + " (" + q(a.name) + ") and " + y + " (" +
                        q(b.name) + ")",
                    {a.name, b.name});

    // 8. One tunnel's endpoint routed into the other — the classic loop / black hole.
    auto endpointInside = [&out](const TunnelConfig& src, const TunnelConfig& dst) {
        const auto dstRoutes = dst.effectiveRoutes();
        for (const auto& p : src.peers) {
            if (!p.endpoint) continue;
            auto ep = parseEndpoint(*p.endpoint);
            if (!ep || !isIpLiteral(ep->host)) continue;
            if (routesCover(dstRoutes, ep->host))
                add(out, FindingSeverity::Error, "EndpointInsideTunnel",
                    "Endpoint " + ep->host + " of " + q(src.name) + " falls inside " +
                        q(dst.name) + " routes — possible loop / black hole",
                    {src.name, dst.name},
                    "TunHub pins the endpoint via the physical gateway on start; verify this is expected");
        }
    };
    endpointInside(a, b);
    endpointInside(b, a);

    return out;
}

void sortBySeverity(std::vector<ConflictFinding>& f) {
    std::stable_sort(f.begin(), f.end(), [](const ConflictFinding& x, const ConflictFinding& y) {
        return static_cast<int>(x.severity) > static_cast<int>(y.severity);
    });
}

}  // namespace

std::vector<ConflictFinding> check(const TunnelConfig& candidate,
                                   const std::vector<TunnelConfig>& active) {
    auto out = selfCheck(candidate);
    for (const auto& other : active) {
        if (other.id == candidate.id) continue;
        auto pair = pairCheck(candidate, other);
        out.insert(out.end(), pair.begin(), pair.end());
    }
    sortBySeverity(out);
    return out;
}

std::vector<ConflictFinding> checkAll(const std::vector<TunnelConfig>& tunnels) {
    std::vector<ConflictFinding> out;
    for (const auto& t : tunnels) {
        auto self = selfCheck(t);
        out.insert(out.end(), self.begin(), self.end());
    }
    for (size_t i = 0; i < tunnels.size(); ++i)
        for (size_t j = i + 1; j < tunnels.size(); ++j) {
            auto pair = pairCheck(tunnels[i], tunnels[j]);
            out.insert(out.end(), pair.begin(), pair.end());
        }
    sortBySeverity(out);
    return out;
}

bool hasErrors(const std::vector<ConflictFinding>& findings) {
    return std::any_of(findings.begin(), findings.end(),
                       [](const ConflictFinding& f) { return f.severity == FindingSeverity::Error; });
}

std::string severityLabel(FindingSeverity s) {
    switch (s) {
        case FindingSeverity::Error:   return "Error";
        case FindingSeverity::Warning: return "Warning";
        default:                       return "Info";
    }
}

}  // namespace tunhub::conflicts
