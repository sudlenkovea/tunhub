import Foundation
import TunHubShared

final class RunningOVPN {
    let spec: ResolvedTunnelSpec
    let process: Process
    let mgmt: OpenVPNManagement
    var utun: String?
    var phase: TunnelPhase = .starting
    var rx: UInt64 = 0
    var tx: UInt64 = 0
    var since = Date()
    var lastError: String?
    var intentionalStop = false
    var dnsApplied = false

    init(spec: ResolvedTunnelSpec, process: Process, mgmt: OpenVPNManagement) {
        self.spec = spec; self.process = process; self.mgmt = mgmt
    }
}

/// OpenVPN lifecycle: spawns the `openvpn` core with a management socket and drives the
/// connect handshake (state, auth incl. static-challenge OTP, bytecount) over it. Routes
/// are managed by openvpn itself; we apply pushed DNS and roll it back on stop. Parallel
/// to `TunnelSupervisor` (which handles the WireGuard family).
final class OpenVPNSupervisor {
    static let shared = OpenVPNSupervisor()
    private let queue = DispatchQueue(label: "com.tunhub.ovpn")
    private var running: [UUID: RunningOVPN] = [:]
    private var starting: [UUID: (name: String, since: Date)] = [:]
    private var stopping: [UUID: String] = [:]
    private var failed: [UUID: TunnelRuntimeState] = [:]

    // MARK: Start

    func start(spec: ResolvedTunnelSpec) throws {
        guard let ov = spec.openvpn else { throw DaemonError("OpenVPN payload missing") }
        try queue.sync {
            if running[spec.id] != nil { throw DaemonError("tunnel already running") }
            if starting[spec.id] != nil { throw DaemonError("tunnel already starting") }
            starting[spec.id] = (spec.name, Date())
            failed.removeValue(forKey: spec.id)
        }
        flog.info("ovpn", "accepted request “\(spec.name)” → background")
        DispatchQueue.global().async { [weak self] in
            do { try self?.startLocked(spec: spec, ov: ov) }
            catch {
                self?.queue.sync {
                    self?.starting.removeValue(forKey: spec.id)
                    self?.failed[spec.id] = TunnelRuntimeState(
                        id: spec.id, name: spec.name, phase: .failed, utunName: nil,
                        errorMessage: error.localizedDescription, peers: [], since: nil)
                }
                flog.error("ovpn", "✕ FAIL “\(spec.name)”: \(error.localizedDescription)")
            }
        }
    }

    private func startLocked(spec: ResolvedTunnelSpec, ov: ResolvedOpenVPN) throws {
        flog.info("ovpn", "▶ START “\(spec.name)” [openvpn] remotes=\(ov.remotes.map { "\($0.host):\($0.port)/\($0.proto)" })")

        guard let core = TunnelSupervisor.locateCore(TunHub.Core.openvpn) else {
            throw DaemonError("core binary not found: \(TunHub.Core.openvpn)")
        }

        // Write the fully-resolved .ovpn to a root-only temp file.
        let cfgPath = Paths.runDir + "/\(spec.id.uuidString).ovpn"
        let mgmtSock = Paths.runDir + "/\(spec.id.uuidString).mgmt.sock"
        try? FileManager.default.removeItem(atPath: mgmtSock)
        try ov.configText.write(toFile: cfgPath, atomically: true, encoding: .utf8)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: cfgPath)

        let p = Process()
        p.executableURL = core
        p.arguments = [
            "--config", cfgPath,
            "--management", mgmtSock, "unix",
            "--management-hold",
            "--management-query-passwords",
            "--auth-nocache",
            "--auth-retry", "none",     // bad credentials → clean exit (no retry loop that sends
                                        // the same wrong creds and makes the status flap)
            "--connect-retry-max", "3", // don't retry forever on network failures either
            // script-security 1: allow openvpn's OWN built-in ifconfig/route calls (needed to
            // configure the utun) but forbid any user scripts. We also strip script directives
            // in the parser, so no config-supplied script can run.
            "--script-security", "1",
            "--verb", "4",
            "--mute", "0"               // don't hide errors (the profile's own --mute is stripped)
        ]
        let pipe = Pipe()
        p.standardError = pipe
        p.standardOutput = pipe
        let id = spec.id
        pipe.fileHandleForReading.readabilityHandler = { [weak self] fh in
            let data = fh.availableData
            guard !data.isEmpty, let s = String(data: data, encoding: .utf8) else { return }
            for line in s.split(separator: "\n") where !line.isEmpty {
                flog.debug("core:\(spec.name)", String(line))
                self?.scanLog(id: id, line: String(line))
            }
        }
        let mgmt = OpenVPNManagement(path: mgmtSock)
        let rt = RunningOVPN(spec: spec, process: p, mgmt: mgmt)

        p.terminationHandler = { [weak self] proc in
            pipe.fileHandleForReading.readabilityHandler = nil
            self?.queue.async { self?.handleTermination(id: id, status: proc.terminationStatus) }
        }
        try p.run()
        flog.debug("ovpn", "openvpn pid=\(p.processIdentifier)")

        queue.sync { starting.removeValue(forKey: id); running[id] = rt }

        // Connect to the management socket and drive the session.
        try mgmt.connect(timeout: 8)
        wireManagement(rt)
        mgmt.send("state on")
        mgmt.send("bytecount 5")
        mgmt.send("hold release")
    }

    private func wireManagement(_ rt: RunningOVPN) {
        let ov = rt.spec.openvpn!
        rt.mgmt.onPasswordNeed = { [weak self, weak rt] payload in
            guard let rt else { return }
            // payload like: Need 'Auth' username/password  OR  Need 'Auth' SC:... / CRV1:...
            if payload.contains("Verification Failed") {
                self?.queue.async { rt.lastError = "authentication failed"; rt.phase = .failed }
                flog.error("ovpn", "“\(rt.spec.name)”: auth verification failed")
                return
            }
            if payload.contains("Need 'Auth'") {
                if payload.contains("CRV1:") {
                    // Dynamic challenge — needs the app callback (OpenVPN M5). Not yet supported.
                    flog.error("ovpn", "“\(rt.spec.name)”: dynamic challenge (CRV1) not yet supported")
                    self?.queue.async { rt.lastError = "OTP challenge not supported yet"; rt.phase = .failed }
                    return
                }
                let user = ov.username ?? ""
                let pass = ov.password ?? ""
                rt.mgmt.sendCredentials(username: user, password: pass, otp: ov.otp)
                flog.info("ovpn", "“\(rt.spec.name)”: sent credentials\(ov.otp != nil ? " + OTP" : "")")
            }
        }
        rt.mgmt.onByteCount = { [weak self, weak rt] rx, tx in
            guard let rt else { return }
            self?.queue.async { rt.rx = rx; rt.tx = tx }
        }
        rt.mgmt.onState = { [weak self, weak rt] payload in
            guard let rt else { return }
            // <time>,<STATE>,<desc>,<localip>,<remoteip>,...
            let f = payload.split(separator: ",", omittingEmptySubsequences: false).map(String.init)
            let state = f.count > 1 ? f[1] : ""
            self?.queue.async { self?.applyState(rt, state: state) }
        }
        rt.mgmt.onFatal = { [weak self, weak rt] msg in
            guard let rt else { return }
            self?.queue.async { rt.lastError = msg; rt.phase = .failed }
            flog.error("ovpn", "“\(rt.spec.name)” FATAL: \(msg)")
        }
    }

    private func applyState(_ rt: RunningOVPN, state: String) {
        let prev = rt.phase
        switch state {
        case "CONNECTED":
            rt.phase = .up
            rt.since = Date()
            applyDNSIfNeeded(rt)
        case "RECONNECTING", "WAIT", "AUTH", "GET_CONFIG", "ASSIGN_IP", "ADD_ROUTES", "TCP_CONNECT", "RESOLVE":
            rt.phase = (state == "RECONNECTING") ? .degraded : .starting
        case "EXITING":
            rt.phase = rt.intentionalStop ? .stopping : .failed
        default:
            break
        }
        if rt.phase != prev {
            flog.info("ovpn", "“\(rt.spec.name)” \(prev.rawValue) → \(rt.phase.rawValue) (state=\(state))")
        }
    }

    /// Apply pushed/profile DNS. OpenVPN doesn't set DNS on macOS itself.
    private func applyDNSIfNeeded(_ rt: RunningOVPN) {
        guard !rt.dnsApplied else { return }
        let ov = rt.spec.openvpn!
        let dns = ov.dns
        guard !dns.isEmpty else { return }
        // Full-tunnel (redirect-gateway) → own the global resolver; otherwise leave system DNS.
        guard ov.redirectGateway else { return }
        let dnsSpec = ResolvedTunnelSpec(
            id: rt.spec.id, name: rt.spec.name, kind: .openvpn, privateKey: "",
            addresses: [], listenPort: nil, mtu: nil,
            dnsServers: dns, dnsSearchDomains: [], dnsMode: .global,
            routes: [], awg: nil, killSwitch: false, peers: [])
        do { try DNSManager.shared.apply(spec: dnsSpec); rt.dnsApplied = true
             flog.info("ovpn", "“\(rt.spec.name)”: applied global DNS \(dns)") }
        catch { flog.warn("ovpn", "“\(rt.spec.name)”: DNS apply failed: \(error.localizedDescription)") }
    }

    /// Parse openvpn's log for the utun device name and pushed DNS.
    private func scanLog(id: UUID, line: String) {
        // Capture a meaningful error line so the UI can show WHY a connect failed.
        let lower = line.lowercased()
        if line.contains("AUTH_FAILED") || lower.contains("verification failed") {
            queue.async { self.running[id]?.lastError = "AUTH_FAILED: authentication failed — check username/password" }
        }
        if lower.contains("error:") || lower.contains("options error") || line.contains("FATAL")
            || lower.contains("cannot ") || lower.contains("exiting due to") {
            let msg = line.replacingOccurrences(of: #"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d "#,
                                                with: "", options: .regularExpression)
            queue.async { self.running[id]?.lastError = msg }
        }
        if let range = line.range(of: #"utun\d+"#, options: .regularExpression) {
            let name = String(line[range])
            queue.async { self.running[id]?.utun = name }
        }
        if line.contains("PUSH_REPLY") {
            // extract every `dhcp-option DNS <ip>`
            var dns: [String] = []
            let tokens = line.components(separatedBy: ",")
            for t in tokens where t.contains("dhcp-option DNS") {
                let parts = t.split(separator: " ")
                if let ip = parts.last, IPAddressRange.pton(String(ip)) != nil { dns.append(String(ip)) }
            }
            if !dns.isEmpty {
                queue.async {
                    guard let rt = self.running[id], var ov = rt.spec.openvpn else { return }
                    // Merge pushed DNS into the effective set (spec is a value type; keep a copy).
                    ov.dns = Array(Set(ov.dns + dns))
                    // Note: spec is let; store pushed DNS on a side channel via re-apply.
                    if rt.phase == .up { self.reapplyDNS(rt, dns: ov.dns, redirect: ov.redirectGateway) }
                }
            }
        }
    }

    private func reapplyDNS(_ rt: RunningOVPN, dns: [String], redirect: Bool) {
        guard redirect, !rt.dnsApplied, !dns.isEmpty else { return }
        let dnsSpec = ResolvedTunnelSpec(
            id: rt.spec.id, name: rt.spec.name, kind: .openvpn, privateKey: "",
            addresses: [], listenPort: nil, mtu: nil,
            dnsServers: dns, dnsSearchDomains: [], dnsMode: .global,
            routes: [], awg: nil, killSwitch: false, peers: [])
        do { try DNSManager.shared.apply(spec: dnsSpec); rt.dnsApplied = true }
        catch { }
    }

    // MARK: Stop

    func stop(id: UUID) {
        let rt: RunningOVPN? = queue.sync {
            guard let rt = running[id] else { failed.removeValue(forKey: id); return nil }
            rt.intentionalStop = true
            rt.phase = .stopping
            running.removeValue(forKey: id)
            stopping[id] = rt.spec.name
            return rt
        }
        guard let rt else { return }
        flog.info("ovpn", "■ STOP “\(rt.spec.name)”")
        DispatchQueue.global().async { [weak self] in
            self?.teardown(rt)
            self?.queue.sync { _ = self?.stopping.removeValue(forKey: id) }
            flog.info("ovpn", "✔ STOPPED “\(rt.spec.name)”")
        }
    }

    func stopAll() {
        let all: [RunningOVPN] = queue.sync {
            let v = Array(running.values); running.removeAll(); return v
        }
        for rt in all { rt.intentionalStop = true; teardown(rt) }
    }

    private func teardown(_ rt: RunningOVPN) {
        DNSManager.shared.rollback(id: rt.spec.id)
        rt.mgmt.send("signal SIGTERM")
        usleep(300_000)
        rt.mgmt.close()
        if rt.process.isRunning {
            rt.process.terminate()
            let deadline = Date().addingTimeInterval(3)
            while rt.process.isRunning && Date() < deadline { usleep(100_000) }
            if rt.process.isRunning { kill(rt.process.processIdentifier, SIGKILL) }
        }
        try? FileManager.default.removeItem(atPath: Paths.runDir + "/\(rt.spec.id.uuidString).ovpn")
    }

    private func handleTermination(id: UUID, status: Int32) {
        guard let rt = running[id], !rt.intentionalStop else { return }
        flog.error("ovpn", "☠︎ openvpn “\(rt.spec.name)” exited (status \(status))")
        DNSManager.shared.rollback(id: rt.spec.id)
        running.removeValue(forKey: id)
        failed[id] = TunnelRuntimeState(id: id, name: rt.spec.name, phase: .failed,
                                        utunName: nil, errorMessage: rt.lastError ?? "openvpn exited",
                                        peers: [], since: nil)
    }

    // MARK: State

    func states() -> [TunnelRuntimeState] {
        queue.sync {
            var out: [TunnelRuntimeState] = running.values.map { rt in
                var peer = PeerRuntime()
                peer.publicKey = rt.spec.openvpn?.remotes.first.map { "\($0.host):\($0.port)" } ?? "openvpn"
                peer.rxBytes = rt.rx; peer.txBytes = rt.tx
                if rt.phase == .up { peer.lastHandshake = rt.since }
                return TunnelRuntimeState(id: rt.spec.id, name: rt.spec.name, phase: rt.phase,
                                          utunName: rt.utun, errorMessage: rt.lastError,
                                          peers: [peer], since: rt.since)
            }
            for (id, info) in starting where running[id] == nil {
                out.append(TunnelRuntimeState(id: id, name: info.name, phase: .starting,
                                              utunName: nil, errorMessage: nil, peers: [], since: info.since))
            }
            for (id, name) in stopping where running[id] == nil {
                out.append(TunnelRuntimeState(id: id, name: name, phase: .stopping,
                                              utunName: nil, errorMessage: nil, peers: [], since: nil))
            }
            out.append(contentsOf: failed.values)
            return out
        }
    }
}
