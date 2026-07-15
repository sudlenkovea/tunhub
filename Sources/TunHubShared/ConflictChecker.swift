import Foundation

public enum FindingSeverity: String, Codable, Comparable {
    case info, warning, error
    public static func < (a: Self, b: Self) -> Bool {
        let order: [Self] = [.info, .warning, .error]
        return order.firstIndex(of: a)! < order.firstIndex(of: b)!
    }
}

public struct ConflictFinding: Identifiable, Codable {
    public var id = UUID()
    public var severity: FindingSeverity
    public var code: String
    public var message: String
    public var tunnelNames: [String]
    public var fixHint: String?
    public init(_ severity: FindingSeverity, _ code: String, _ message: String,
                tunnels: [String], fixHint: String? = nil) {
        self.severity = severity; self.code = code; self.message = message
        self.tunnelNames = tunnels; self.fixHint = fixHint
    }
}

/// Checks route/DNS/address/port overlaps between tunnels. Design §5.3.
public enum ConflictChecker {

    /// Candidate against the set of active tunnels.
    public static func check(candidate: TunnelConfig, against active: [TunnelConfig]) -> [ConflictFinding] {
        var out: [ConflictFinding] = []
        out.append(contentsOf: selfCheck(candidate))
        for other in active where other.id != candidate.id {
            out.append(contentsOf: pairCheck(candidate, other))
        }
        return out.sorted { $0.severity > $1.severity }
    }

    /// All pairs in the set (for "Check all").
    public static func checkAll(_ tunnels: [TunnelConfig]) -> [ConflictFinding] {
        var out: [ConflictFinding] = []
        for t in tunnels { out.append(contentsOf: selfCheck(t)) }
        for i in 0..<tunnels.count {
            for j in (i + 1)..<tunnels.count {
                out.append(contentsOf: pairCheck(tunnels[i], tunnels[j]))
            }
        }
        return out.sorted { $0.severity > $1.severity }
    }

    // MARK: - Single-tunnel checks

    static func selfCheck(_ t: TunnelConfig) -> [ConflictFinding] {
        var out: [ConflictFinding] = []
        // DNSUnreachable: DNS server not covered by AllowedIPs
        let routes = t.effectiveRoutes()
        for dns in t.interface.dns {
            let covered = routes.contains { $0.containsAddress(dns) }
            if !covered {
                out.append(.init(.warning, "DNSUnreachable",
                    "DNS \(dns) of “\(t.name)” is not covered by its AllowedIPs — resolution will bypass the tunnel",
                    tunnels: [t.name],
                    fixHint: "Add \(dns)/32 to AllowedIPs or change the DNS"))
            }
        }
        // EndpointInsideTunnel (the tunnel's own routes)
        for p in t.peers {
            guard let ep = p.endpoint, let (host, _) = EndpointUtil.split(ep),
                  EndpointUtil.isIPLiteral(host) else { continue }
            if routes.contains(where: { $0.containsAddress(host) }) {
                out.append(.init(.info, "EndpointPinned",
                    "Endpoint \(host) falls inside “\(t.name)” routes — TunHub will pin it via the physical gateway automatically",
                    tunnels: [t.name]))
            }
        }
        if let a = t.awg {
            for e in a.validate() {
                out.append(.init(.error, "AWGParamInvalid", "“\(t.name)”: \(e)", tunnels: [t.name]))
            }
        }
        return out
    }

    // MARK: - Pair checks

    static func pairCheck(_ a: TunnelConfig, _ b: TunnelConfig) -> [ConflictFinding] {
        var out: [ConflictFinding] = []
        let ra = a.effectiveRoutes(), rb = b.effectiveRoutes()

        // 1. Default route clash (prefix 0 or the /1 pair)
        if a.hasDefaultRoute && b.hasDefaultRoute {
            out.append(.init(.error, "DefaultRouteClash",
                "“\(a.name)” and “\(b.name)” both claim all traffic (default route). They cannot run at the same time.",
                tunnels: [a.name, b.name],
                fixHint: "Keep the default route on one; move the other to specific subnets or split DNS"))
        }

        // 2/3. Routes: exact duplicate / shadowing
        for x in ra {
            for y in rb {
                guard x.isIPv6 == y.isIPv6 else { continue }
                if x.prefix <= 1 || y.prefix <= 1 { continue } // default handled above
                if x.canonical == y.canonical {
                    out.append(.init(.error, "ExactDuplicate",
                        "Identical route \(x.canonical) in “\(a.name)” and “\(b.name)”",
                        tunnels: [a.name, b.name]))
                } else if x.contains(y) {
                    out.append(.init(.warning, "SubnetShadowing",
                        "\(y.canonical) (“\(b.name)”) is nested in \(x.canonical) (“\(a.name)”) — traffic goes to the more specific “\(b.name)”",
                        tunnels: [a.name, b.name]))
                } else if y.contains(x) {
                    out.append(.init(.warning, "SubnetShadowing",
                        "\(x.canonical) (“\(a.name)”) is nested in \(y.canonical) (“\(b.name)”) — traffic goes to the more specific “\(a.name)”",
                        tunnels: [a.name, b.name]))
                }
            }
        }

        // 4. Interface address overlap
        for x in a.interface.addresses {
            for y in b.interface.addresses where x.overlaps(y) {
                out.append(.init(.error, "AddressOverlap",
                    "Interface addresses overlap: \(x.canonical) (“\(a.name)”) and \(y.canonical) (“\(b.name)”)",
                    tunnels: [a.name, b.name]))
            }
        }

        // 5. ListenPort clash
        if let pa = a.interface.listenPort, let pb = b.interface.listenPort, pa == pb {
            out.append(.init(.error, "ListenPortClash",
                "Same ListenPort \(pa) on “\(a.name)” and “\(b.name)”",
                tunnels: [a.name, b.name]))
        }

        // 6. DNS: global clash (by effective mode — split tunnels don't take global DNS)
        let aGlobal = a.effectiveDNSMode == .global && !a.interface.dns.isEmpty
        let bGlobal = b.effectiveDNSMode == .global && !b.interface.dns.isEmpty
        if aGlobal && bGlobal {
            out.append(.init(.error, "GlobalDNSClash",
                "“\(a.name)” and “\(b.name)” both want to be the system's global DNS",
                tunnels: [a.name, b.name],
                fixHint: "Switch one tunnel to split DNS (by domain) in its settings"))
        }

        // 7. Split domain overlap
        if case .split(let da) = a.options.dnsMode, case .split(let db) = b.options.dnsMode {
            for x in da {
                for y in db where domainOverlap(x, y) {
                    out.append(.init(.warning, "SplitDomainOverlap",
                        "DNS domains overlap: \(x) (“\(a.name)”) and \(y) (“\(b.name)”)",
                        tunnels: [a.name, b.name]))
                }
            }
        }

        // 8. Endpoint inside another tunnel
        for (src, dstRoutes, dstName) in [(a, rb, b.name), (b, ra, a.name)] {
            for p in src.peers {
                guard let ep = p.endpoint, let (host, _) = EndpointUtil.split(ep),
                      EndpointUtil.isIPLiteral(host) else { continue }
                if dstRoutes.contains(where: { $0.containsAddress(host) }) {
                    out.append(.init(.error, "EndpointInsideTunnel",
                        "Endpoint \(host) of “\(src.name)” falls inside “\(dstName)” routes — possible loop / black hole",
                        tunnels: [src.name, dstName],
                        fixHint: "TunHub pins the endpoint via the physical gateway on start; verify this is expected"))
                }
            }
        }

        return out
    }

    static func domainOverlap(_ a: String, _ b: String) -> Bool {
        let x = a.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "."))
        let y = b.lowercased().trimmingCharacters(in: CharacterSet(charactersIn: "."))
        return x == y || x.hasSuffix("." + y) || y.hasSuffix("." + x)
    }

    public static func hasErrors(_ findings: [ConflictFinding]) -> Bool {
        findings.contains { $0.severity == .error }
    }
}
