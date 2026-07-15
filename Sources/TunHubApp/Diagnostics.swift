import Foundation
import Network
import UserNotifications
import TunHubShared

// MARK: - Probe via a specific tunnel
// The socket binds to the utun interface address (requiredLocalEndpoint) —
// traffic is guaranteed to leave via the tunnel, without an NWInterface lookup.

enum TunnelProbe {

    /// TCP-connect probe. completion(success).
    static func tcpProbe(localAddress: String, host: String, port: UInt16,
                         timeout: TimeInterval = 5, completion: @escaping (Bool) -> Void) {
        let params = NWParameters.tcp
        if let local = IPv4Address(localAddress) {
            params.requiredLocalEndpoint = .hostPort(host: .ipv4(local), port: .any)
        } else if let local6 = IPv6Address(localAddress) {
            params.requiredLocalEndpoint = .hostPort(host: .ipv6(local6), port: .any)
        }
        guard let nwPort = NWEndpoint.Port(rawValue: port) else { completion(false); return }
        let conn = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: params)
        var done = false
        let finish: (Bool) -> Void = { ok in
            if !done { done = true; conn.cancel(); completion(ok) }
        }
        conn.stateUpdateHandler = { state in
            switch state {
            case .ready: finish(true)
            case .failed, .cancelled: finish(false)
            default: break
            }
        }
        conn.start(queue: .global())
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout) { finish(false) }
    }

    enum ExternalIPResult {
        case ip(String)
        case unreachable(short: String, detail: String)   // short label + hover help text
    }

    /// External IP via a specific tunnel. `curl --interface utunN` binds the outgoing
    /// socket to the tunnel interface (no root needed). On failure it resolves the
    /// IP-check service's address and reports whether that address is even covered by the
    /// tunnel's routes — so a split tunnel that simply doesn't route the service is called
    /// out precisely, instead of a vague "failed".
    static func externalIP(interface: String?, routes: [IPAddressRange],
                           completion: @escaping (ExternalIPResult) -> Void) {
        DispatchQueue.global().async {
            var args = ["--max-time", "8", "-s", "-4"]
            if let iface = interface, !iface.isEmpty { args += ["--interface", iface] }
            // (url, host) — host is used to resolve the service IP for the routing check.
            let services = [("https://api.ipify.org", "api.ipify.org"),
                            ("https://ifconfig.me/ip", "ifconfig.me"),
                            ("https://icanhazip.com", "icanhazip.com")]
            var lastCode: Int32 = -1
            for (url, _) in services {
                let p = Process()
                p.executableURL = URL(fileURLWithPath: "/usr/bin/curl")
                p.arguments = args + [url]
                let out = Pipe()
                p.standardOutput = out
                p.standardError = Pipe()
                do { try p.run() } catch { continue }
                let data = out.fileHandleForReading.readDataToEndOfFile()
                p.waitUntilExit()
                lastCode = p.terminationStatus
                let s = String(data: data, encoding: .utf8)?
                    .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
                if lastCode == 0, IPAddressRange.pton(s) != nil {
                    DispatchQueue.main.async { completion(.ip(s)) }
                    return
                }
            }

            // All services failed — figure out WHY. Resolve the primary service and check
            // whether its IP is covered by the tunnel's routes.
            let host = services[0].1
            let serviceIP = resolveFirstIPv4(host)
            let routedThrough = serviceIP.map { ip in routes.contains { $0.containsAddress(ip) } }
            let result = failureResult(code: lastCode, host: host, serviceIP: serviceIP, routedThrough: routedThrough)
            DispatchQueue.main.async { completion(result) }
        }
    }

    /// Build a short label + detailed hover help from the curl code and the routing check.
    private static func failureResult(code: Int32, host: String, serviceIP: String?,
                                      routedThrough: Bool?) -> ExternalIPResult {
        // Decisive case: the service's IP is definitively NOT in the tunnel's routes.
        if routedThrough == false, let ip = serviceIP {
            return .unreachable(
                short: String(localized: "not routed"),
                detail: String(localized: "The IP-check service \(host) (\(ip)) is not covered by this tunnel's routes, so it can't be reached through the tunnel. This is expected for a split tunnel."))
        }
        // The service IS routed through the tunnel but still didn't answer, or we couldn't
        // even resolve it — map the curl code to a concise reason.
        let routedNote = routedThrough == true
            ? " " + String(localized: "The service is routed through the tunnel but didn't respond (blocked or down).")
            : ""
        switch code {
        case 6:
            return .unreachable(short: String(localized: "DNS error"),
                detail: String(localized: "Couldn't resolve the IP-check service — DNS may be going outside the tunnel."))
        case 28:
            return .unreachable(short: String(localized: "timeout"),
                detail: String(localized: "Timed out reaching the IP-check service.") + routedNote)
        case 35, 60:
            return .unreachable(short: String(localized: "TLS error"),
                detail: String(localized: "TLS handshake with the IP-check service failed.") + routedNote)
        default:  // 7 (connect failed) and everything else
            return .unreachable(short: String(localized: "unavailable"),
                detail: String(localized: "Couldn't reach the IP-check service (blocked or not routed).") + routedNote + " (curl \(code))")
        }
    }

    /// Resolve a host to its first IPv4 literal (best-effort, short).
    private static func resolveFirstIPv4(_ host: String) -> String? {
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_STREAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &res) == 0, let first = res else { return nil }
        defer { freeaddrinfo(first) }
        var p: UnsafeMutablePointer<addrinfo>? = first
        while let cur = p {
            var buf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(cur.pointee.ai_addr, cur.pointee.ai_addrlen,
                           &buf, socklen_t(buf.count), nil, 0, NI_NUMERICHOST) == 0 {
                return String(cString: buf)
            }
            p = cur.pointee.ai_next
        }
        return nil
    }

    /// The first system resolver (for the DNS-leak report): `scutil --dns`.
    static func systemPrimaryDNS() -> [String] {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/scutil")
        p.arguments = ["--dns"]
        let pipe = Pipe()
        p.standardOutput = pipe
        guard (try? p.run()) != nil else { return [] }
        let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        p.waitUntilExit()
        var servers: [String] = []
        var inFirstResolver = false
        for line in out.split(separator: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("resolver #1") { inFirstResolver = true; continue }
            if t.hasPrefix("resolver #") && inFirstResolver { break }
            if inFirstResolver, t.hasPrefix("nameserver["),
               let ip = t.split(separator: ":").last?.trimmingCharacters(in: .whitespaces) {
                servers.append(ip)
            }
        }
        return servers
    }
}

// MARK: - Notifications

enum Notifier {
    static func setup() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }
    static func notify(title: String, body: String) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        let req = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        UNUserNotificationCenter.current().add(req)
    }
}

// MARK: - Health checks + failover (design §10)

@MainActor
final class HealthChecker {
    private var lastProbe: [UUID: Date] = [:]
    private var failures: [UUID: Int] = [:]
    private var actionCooldown: [UUID: Date] = [:]

    func tick(app: AppState) {
        for cfg in app.tunnels {
            guard let hc = cfg.options.healthCheck, !hc.host.isEmpty,
                  let state = app.runtime[cfg.id],
                  state.phase == .up || state.phase == .degraded,
                  let localAddr = cfg.interface.addresses.first?.addressString
            else { continue }

            let due = lastProbe[cfg.id].map { Date().timeIntervalSince($0) >= Double(hc.intervalSec) } ?? true
            guard due else { continue }
            lastProbe[cfg.id] = Date()

            let id = cfg.id
            TunnelProbe.tcpProbe(localAddress: localAddr, host: hc.host, port: hc.port) { ok in
                Task { @MainActor in
                    if ok {
                        self.failures[id] = 0
                    } else {
                        self.failures[id, default: 0] += 1
                        if self.failures[id, default: 0] >= hc.failureThreshold {
                            self.failures[id] = 0
                            await self.act(config: cfg, action: hc.action, app: app)
                        }
                    }
                }
            }
        }
        // failover on actual failure (without a health check)
        for cfg in app.tunnels {
            guard cfg.options.failoverGroup != nil,
                  app.runtime[cfg.id]?.phase == .failed else { continue }
            Task { await self.act(config: cfg, action: .failover, app: app) }
        }
    }

    private func act(config: TunnelConfig, action: HealthAction, app: AppState) async {
        if let cool = actionCooldown[config.id], Date().timeIntervalSince(cool) < 60 { return }
        actionCooldown[config.id] = Date()

        switch action {
        case .notify:
            Notifier.notify(title: "TunHub: “\(config.name)” degraded",
                            body: "Health check is failing")
        case .restart:
            Notifier.notify(title: "TunHub: restarting “\(config.name)”",
                            body: "Health check is failing — restarting the tunnel")
            await app.stop(config)
            try? await app.start(config, force: true)
        case .failover:
            guard let group = config.options.failoverGroup else { return }
            let members = app.tunnels
                .filter { $0.options.failoverGroup == group && $0.id != config.id }
                .sorted { $0.options.failoverPriority < $1.options.failoverPriority }
            guard let next = members.first(where: { app.runtime[$0.id]?.phase != .up }) else { return }
            Notifier.notify(title: "TunHub: failover",
                            body: "“\(config.name)” went down — bringing up “\(next.name)”")
            await app.stop(config)
            try? await app.start(next, force: true)
        }
    }
}
