import Foundation
import SystemConfiguration
import TunHubShared

// MARK: - RouteManager

/// Routes via /sbin/route with a journal for precise rollback (design §5.1).
final class RouteManager {
    static let shared = RouteManager()
    private var journal: [UUID: [[String]]] = [:]   // id → delete-args
    private let lock = NSLock()   // guards the journal during parallel starts
    private func setJournal(_ id: UUID, _ v: [[String]]?) { lock.lock(); journal[id] = v; lock.unlock() }
    private func getJournal(_ id: UUID) -> [[String]] { lock.lock(); defer { lock.unlock() }; return journal[id] ?? [] }

    struct Gateway { let gateway: String?; let iface: String? }

    /// Physical default gateway (before our /1 routes are installed).
    func physicalDefaultGateway(v6: Bool = false) -> Gateway {
        currentRoute(to: "default", v6: v6)
    }

    /// How the system reaches `dst` RIGHT NOW (default or a specific host/ip).
    /// Used to pin the endpoint: we record the real path (physical interface OR another VPN).
    func currentRoute(to dst: String, v6: Bool = false) -> Gateway {
        let r = run("/sbin/route", ["-n", "get", v6 ? "-inet6" : "-inet", dst])
        var gw: String?, ifc: String?
        for line in r.stdout.split(separator: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("gateway:") { gw = String(t.dropFirst(8)).trimmingCharacters(in: .whitespaces) }
            if t.hasPrefix("interface:") { ifc = String(t.dropFirst(10)).trimmingCharacters(in: .whitespaces) }
        }
        // gateway may be a link address — keep only an IP
        if let g = gw, IPAddressRange.pton(g) == nil { gw = nil }
        return Gateway(gateway: gw, iface: ifc)
    }

    func apply(spec: ResolvedTunnelSpec, utun: String, resolvedEndpoints: [Int: String]) throws {
        var deletes: [[String]] = []
        defer { setJournal(spec.id, deletes) }

        // 1. Pin endpoints via the CURRENT path to them (before the /1 routes!).
        // Ask the system how it actually reaches the endpoint (physical interface OR another
        // active VPN) and lock in exactly that path — otherwise the tunnel's outer packets
        // go into a black hole.
        for (_, ep) in resolvedEndpoints {
            guard let (host, _) = EndpointUtil.split(ep) else { continue }
            let v6 = host.contains(":")
            let fam = v6 ? "-inet6" : "-inet"
            let path = currentRoute(to: host, v6: v6)
            var addArgs = ["-q", "-n", "add", fam, "-host", host]
            if let g = path.gateway { addArgs += [g] }              // via gateway (physical or VPN gw)
            else if let i = path.iface { addArgs += ["-interface", i] }  // point-to-point interface
            else {
                flog.warn("route", "pin endpoint \(host): could not determine path — skipping")
                continue
            }
            let r = run("/sbin/route", addArgs)
            if r.ok {
                flog.info("route", "pin endpoint \(host) → gw=\(path.gateway ?? "-") iface=\(path.iface ?? "-")")
                deletes.append(["-q", "-n", "delete", fam, "-host", host])
            } else {
                flog.error("route", "pin endpoint \(host) FAILED: \(r.stderr.trimmingCharacters(in: .whitespacesAndNewlines))")
            }
        }

        // 2. Tunnel routes; default → the /1 pair (wg-quick trick).
        var expanded: [IPAddressRange] = []
        for r in spec.routes {
            if r.prefix == 0 {
                if r.isIPv6 {
                    expanded += [IPAddressRange(string: "::/1")!, IPAddressRange(string: "8000::/1")!]
                } else {
                    expanded += [IPAddressRange(string: "0.0.0.0/1")!, IPAddressRange(string: "128.0.0.0/1")!]
                }
            } else {
                expanded.append(r)
            }
        }
        // Fast add of many routes: one bash script (far fewer forks from Swift; route is
        // still invoked per prefix, but pipelined and without Process overhead). Speeds up
        // "default-all" start from ~14s to ~seconds.
        if expanded.count > 8 {
            var script = "set +e\n"
            for r in expanded {
                let fam = r.isIPv6 ? "-inet6" : "-inet"
                script += "/sbin/route -q -n add \(fam) \(r.canonical) -interface \(utun)\n"
                deletes.append(["-q", "-n", "delete", fam, r.canonical, "-interface", utun])
            }
            let t0 = Date()
            _ = run("/bin/bash", ["-c", script])
            flog.info("route", "added \(expanded.count) routes in \(Int(Date().timeIntervalSince(t0)*1000))ms (batch)")
        } else {
        for r in expanded {
            let fam = r.isIPv6 ? "-inet6" : "-inet"
            let res = run("/sbin/route", ["-q", "-n", "add", fam, r.canonical, "-interface", utun])
            if res.ok {
                deletes.append(["-q", "-n", "delete", fam, r.canonical, "-interface", utun])
            } else {
                // roll back what's already installed and error out
                for d in deletes.reversed() { run("/sbin/route", d) }
                deletes = []
                throw DaemonError("route add \(r.canonical) failed: \(res.stderr)")
            }
        }
        }

        // 3. LOOP CHECK: after installing routes the endpoint MUST go outside the tunnel.
        // If `route get endpoint` shows our utun, the core's outer packets loop (no data flows).
        for (_, ep) in resolvedEndpoints {
            guard let (host, _) = EndpointUtil.split(ep) else { continue }
            let v6 = host.contains(":")
            let path = currentRoute(to: host, v6: v6)
            if path.iface == utun {
                flog.error("route", "⚠️ LOOP: endpoint \(host) is routed INTO THE TUNNEL ITSELF (\(utun))! The core's outer packets loop — no data will flow. The pin didn't take.")
            } else {
                flog.info("route", "endpoint \(host) exits via \(path.iface ?? "?") gw=\(path.gateway ?? "-") (outside the tunnel — ok)")
            }
        }
    }

    func rollback(id: UUID) {
        for d in getJournal(id).reversed() { run("/sbin/route", d) }
        setJournal(id, nil)
    }

    /// Check whether the endpoint looped back into the tunnel and, if so, re-pin it via the
    /// physical gateway (like wg-quick monitor_daemon → set_endpoint_direct_route).
    /// Returns true if a loop was detected and fixed.
    @discardableResult
    func ensureEndpointNotLooped(id: UUID, endpoints: [String], tunnelUtun: String) -> Bool {
        var fixedAny = false
        for ep in endpoints {
            guard let (host, _) = EndpointUtil.split(ep) else { continue }
            let v6 = host.contains(":")
            let cur = currentRoute(to: host, v6: v6)
            guard cur.iface == tunnelUtun else { continue }  // not in the tunnel — ok
            // LOOP: re-pin via the physical default (bypassing all tunnels)
            let fam = v6 ? "-inet6" : "-inet"
            // remove the bad route and add it via the physical gateway
            run("/sbin/route", ["-q", "-n", "delete", fam, "-host", host])
            let phys = physicalDefaultGateway(v6: v6)
            var addArgs = ["-q", "-n", "add", fam, "-host", host]
            if let g = phys.gateway { addArgs += [g] }
            else if let i = phys.iface, i != tunnelUtun { addArgs += ["-interface", i] }
            else {
                flog.error("route", "endpoint loop \(host): no physical path to re-pin through")
                continue
            }
            let r = run("/sbin/route", addArgs)
            flog.warn("route", "🔁 LOOP fixed: endpoint \(host) re-pinned via gw=\(phys.gateway ?? "-") iface=\(phys.iface ?? "-") (\(r.ok ? "ok" : "fail"))")
            // add to the delete journal
            var j = getJournal(id)
            j.append(["-q", "-n", "delete", fam, "-host", host])
            setJournal(id, j)
            fixedAny = true
        }
        return fixedAny
    }
}

// MARK: - DNSManager

/// Split-DNS via SCDynamicStore, global via networksetup (like wg-quick). Design §5.2.
final class DNSManager {
    static let shared = DNSManager()

    private struct GlobalBackup: Codable {
        var tunnelID: UUID
        var services: [String: [String]]   // service name → previous DNS ([] = Empty)
    }
    private var appliedSplit: Set<UUID> = []
    private var globalBackup: GlobalBackup?
    private let lock = NSLock()

    private lazy var store: SCDynamicStore? =
        SCDynamicStoreCreate(nil, "TunHub" as CFString, nil, nil)

    func apply(spec: ResolvedTunnelSpec) throws {
        guard !spec.dnsServers.isEmpty else { return }
        lock.lock(); defer { lock.unlock() }
        switch spec.dnsMode {
        case .disabled:
            return
        case .split(let domains):
            try applySplit(id: spec.id, servers: spec.dnsServers,
                           domains: domains.isEmpty ? spec.dnsSearchDomains : domains)
        case .global:
            try applyGlobal(id: spec.id, servers: spec.dnsServers)
        }
    }

    private func applySplit(id: UUID, servers: [String], domains: [String]) throws {
        guard let store else { throw DaemonError("SCDynamicStore unavailable") }
        guard !domains.isEmpty else {
            throw DaemonError("split-DNS without match domains: set search domains in the tunnel settings")
        }
        let key = "State:/Network/Service/tunhub-\(id.uuidString)/DNS" as CFString
        let dict: [String: Any] = [
            "ServerAddresses": servers,
            "SupplementalMatchDomains": domains
        ]
        guard SCDynamicStoreSetValue(store, key, dict as CFDictionary) else {
            throw DaemonError("SCDynamicStoreSetValue failed")
        }
        appliedSplit.insert(id)
    }

    private func applyGlobal(id: UUID, servers: [String]) throws {
        guard globalBackup == nil else {
            throw DaemonError("global DNS already taken by another tunnel")
        }
        // Set DNS only on the PRIMARY network service (the default-route interface),
        // not on all services — fast and doesn't touch other/stale entries.
        let targets = primaryServices()
        flog.debug("dns", "global DNS \(servers) on services: \(targets)")
        var backup = GlobalBackup(tunnelID: id, services: [:])
        for svc in targets {
            let cur = run("/usr/sbin/networksetup", ["-getdnsservers", svc])
            let lines = cur.stdout.split(separator: "\n").map(String.init)
            let existing = lines.allSatisfy { IPAddressRange.pton($0) != nil } ? lines : []
            backup.services[svc] = existing
            run("/usr/sbin/networksetup", ["-setdnsservers", svc] + servers)
        }
        globalBackup = backup
        persistBackup()
    }

    /// The service(s) that own the current default-route interface. Fallback: the active service.
    private func primaryServices() -> [String] {
        let dev = RouteManager.shared.physicalDefaultGateway().iface   // e.g. en0
        // map: service → device via `-listnetworkserviceorder`
        let r = run("/usr/sbin/networksetup", ["-listnetworkserviceorder"])
        var svcForDev: [String: String] = [:]  // device → service
        var lastService: String?
        for raw in r.stdout.split(separator: "\n") {
            let line = String(raw)
            if let range = line.range(of: #"\(\d+\)\s"#, options: .regularExpression) {
                lastService = String(line[range.upperBound...]).trimmingCharacters(in: .whitespaces)
            } else if line.contains("Device:"), let dev = line.components(separatedBy: "Device: ").last?
                        .replacingOccurrences(of: ")", with: "").trimmingCharacters(in: .whitespaces),
                      let svc = lastService {
                svcForDev[dev] = svc
            }
        }
        if let dev, let svc = svcForDev[dev] { return [svc] }
        // fallback: the first service with an active IPv4
        for svc in listNetworkServices() {
            let info = run("/usr/sbin/networksetup", ["-getinfo", svc])
            if info.stdout.contains("IP address:"),
               !info.stdout.contains("IP address: none"),
               info.stdout.range(of: #"IP address:\s\d"#, options: .regularExpression) != nil {
                return [svc]
            }
        }
        return []
    }

    func rollback(id: UUID) {
        lock.lock(); defer { lock.unlock() }
        if appliedSplit.remove(id) != nil, let store {
            let key = "State:/Network/Service/tunhub-\(id.uuidString)/DNS" as CFString
            SCDynamicStoreRemoveValue(store, key)
        }
        if let b = globalBackup, b.tunnelID == id {
            restoreGlobal(b)
            globalBackup = nil
            persistBackup()
        }
    }

    private func restoreGlobal(_ b: GlobalBackup) {
        for (svc, dns) in b.services {
            if dns.isEmpty {
                run("/usr/sbin/networksetup", ["-setdnsservers", svc, "Empty"])
            } else {
                run("/usr/sbin/networksetup", ["-setdnsservers", svc] + dns)
            }
        }
    }

    private func listNetworkServices() -> [String] {
        let r = run("/usr/sbin/networksetup", ["-listallnetworkservices"])
        return r.stdout.split(separator: "\n").dropFirst()
            .map(String.init)
            .filter { !$0.hasPrefix("*") && !$0.isEmpty }
    }

    private func persistBackup() {
        if let b = globalBackup, let d = try? TunJSON.encoder.encode(b) {
            try? d.write(to: URL(fileURLWithPath: Paths.dnsBackupFile), options: .atomic)
        } else {
            try? FileManager.default.removeItem(atPath: Paths.dnsBackupFile)
        }
    }

    /// Recovery after a crash/reboot: orphaned split keys + the global backup.
    func crashRecovery() {
        if let store,
           let keys = SCDynamicStoreCopyKeyList(store, "State:/Network/Service/tunhub-.*" as CFString) as? [String] {
            for k in keys { SCDynamicStoreRemoveValue(store, k as CFString) }
        }
        if let d = try? Data(contentsOf: URL(fileURLWithPath: Paths.dnsBackupFile)),
           let b = try? TunJSON.decoder.decode(GlobalBackup.self, from: d) {
            dlog.warning("restoring global DNS after crash")
            restoreGlobal(b)
            try? FileManager.default.removeItem(atPath: Paths.dnsBackupFile)
        }
    }
}

// MARK: - FirewallManager (kill switch, pf anchor)

final class FirewallManager {
    static let shared = FirewallManager()

    private struct PFState: Codable { var active: Bool; var token: String? }
    private var state = PFState(active: false, token: nil)

    struct ActiveTunnelInfo {
        let utun: String
        let endpoints: [(ip: String, port: UInt16)]
    }

    /// Rebuild the kill switch from a snapshot of active tunnels with killSwitch=true.
    func rebuild(_ tunnels: [ActiveTunnelInfo]) {
        if tunnels.isEmpty { disable(); return }

        var rules = ""
        rules += "set skip on lo0\n"
        rules += "block drop out all\n"
        for t in tunnels {
            rules += "pass out on \(t.utun) all\n"
            for ep in t.endpoints {
                let dst = ep.ip.contains(":") ? "{ \(ep.ip) }" : ep.ip
                rules += "pass out proto udp from any to \(dst) port = \(ep.port)\n"
            }
        }
        rules += "pass out proto udp from any to any port { 67, 68, 546, 547 }\n"  // DHCP/DHCPv6
        rules += "pass out inet6 proto ipv6-icmp all\n"                            // NDP
        do {
            try rules.write(toFile: Paths.pfRulesFile, atomically: true, encoding: .utf8)
            let base = (try? String(contentsOfFile: "/etc/pf.conf", encoding: .utf8)) ?? ""
            let main = base + "\nanchor \"com.tunhub\"\nload anchor \"com.tunhub\" from \"\(Paths.pfRulesFile)\"\n"
            try main.write(toFile: Paths.pfMainFile, atomically: true, encoding: .utf8)
            let load = run("/sbin/pfctl", ["-f", Paths.pfMainFile])
            guard load.ok else { dlog.error("pfctl -f failed: \(load.stderr, privacy: .public)"); return }
            if !state.active {
                let en = run("/sbin/pfctl", ["-E"])
                // "Token : 12345" in stderr
                if let tok = en.stderr.split(separator: "\n")
                    .first(where: { $0.contains("Token") })?
                    .split(separator: ":").last?.trimmingCharacters(in: .whitespaces) {
                    state.token = tok
                }
                state.active = true
                persist()
            }
        } catch {
            dlog.error("kill switch apply failed: \(String(describing: error), privacy: .public)")
        }
    }

    func disable() {
        guard state.active else { return }
        run("/sbin/pfctl", ["-a", "com.tunhub", "-F", "all"])
        run("/sbin/pfctl", ["-f", "/etc/pf.conf"])
        if let tok = state.token { run("/sbin/pfctl", ["-X", tok]) }
        state = PFState(active: false, token: nil)
        persist()
    }

    private func persist() {
        if let d = try? TunJSON.encoder.encode(state) {
            try? d.write(to: URL(fileURLWithPath: Paths.pfStateFile), options: .atomic)
        }
    }

    func crashRecovery() {
        guard let d = try? Data(contentsOf: URL(fileURLWithPath: Paths.pfStateFile)),
              let s = try? TunJSON.decoder.decode(PFState.self, from: d), s.active else { return }
        dlog.warning("disabling stale kill switch after crash")
        state = s
        disable()
    }
}
