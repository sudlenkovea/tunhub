import Foundation
import TunHubShared

final class RunningTunnel {
    let spec: ResolvedTunnelSpec
    let process: Process
    let utun: String
    let socketPath: String
    let resolvedEndpoints: [Int: String]
    var phase: TunnelPhase = .starting
    var peers: [PeerRuntime] = []
    var since = Date()
    var lastError: String?
    var restartCount = 0
    var intentionalStop = false
    // Traffic-based health tracking (WireGuard handshakes lazily, so handshake age
    // alone is a poor health signal — an idle tunnel is healthy, not degraded).
    var lastTx: UInt64 = 0
    var lastRx: UInt64 = 0
    var stalledSince: Date?   // set while we're sending but getting no replies

    /// A tunnel that routes all traffic (default route via prefix 0 or the /1 pair).
    var isDefaultRoute: Bool { spec.routes.contains { $0.prefix <= 1 } }

    init(spec: ResolvedTunnelSpec, process: Process, utun: String,
         socketPath: String, resolvedEndpoints: [Int: String]) {
        self.spec = spec; self.process = process; self.utun = utun
        self.socketPath = socketPath; self.resolvedEndpoints = resolvedEndpoints
    }
}

/// Core-process lifecycle + route/dns/pf orchestration. See design §3.1.
final class TunnelSupervisor {
    static let shared = TunnelSupervisor()
    private let queue = DispatchQueue(label: "com.tunhub.supervisor")
    // Heavy work (routes/DNS/spawn) runs on a separate concurrent queue so that
    // starting one tunnel doesn't block start/stop of others or the stats poll.
    private let workQueue = DispatchQueue(label: "com.tunhub.supervisor.work", attributes: .concurrent)
    private var running: [UUID: RunningTunnel] = [:]
    private var failed: [UUID: TunnelRuntimeState] = [:]   // for reporting in the UI
    private var starting: [UUID: (name: String, since: Date)] = [:]  // accepted, still coming up
    private var statsTimer: DispatchSourceTimer?
    var killSwitchGloballyEnabled = true

    // MARK: Core binary lookup

    /// Absolute path to the daemon's own directory (argv[0] may be relative under launchd).
    static func executableDir() -> URL {
        var size: UInt32 = 0
        _NSGetExecutablePath(nil, &size)
        var buf = [CChar](repeating: 0, count: Int(size))
        if _NSGetExecutablePath(&buf, &size) == 0 {
            let path = String(cString: buf)
            return URL(fileURLWithPath: path).resolvingSymlinksInPath().deletingLastPathComponent()
        }
        // fallback
        return URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
    }

    static func locateCore(_ name: String) -> URL? {
        let dir = executableDir()
        let candidates = [
            dir.appendingPathComponent(name),                                   // next to the daemon (Contents/MacOS)
            dir.deletingLastPathComponent().appendingPathComponent("MacOS/\(name)"),
            URL(fileURLWithPath: "/usr/local/bin/\(name)")                       // dev fallback
        ]
        for c in candidates where FileManager.default.isExecutableFile(atPath: c.path) {
            return c
        }
        return nil
    }

    // MARK: Start

    /// Non-blocking request intake: quick validation on the serial queue, heavy work
    /// runs in the background. The XPC call returns in milliseconds.
    func start(spec: ResolvedTunnelSpec) throws {
        try queue.sync {
            if running[spec.id] != nil { throw DaemonError("tunnel already running") }
            if starting[spec.id] != nil { throw DaemonError("tunnel already starting") }
            starting[spec.id] = (spec.name, Date())
            failed.removeValue(forKey: spec.id)
            flog.info("start", "accepted request “\(spec.name)” → background")
        }
        workQueue.async { [weak self] in
            guard let self else { return }
            do {
                try self.startLocked(spec: spec)
            } catch {
                self.queue.sync {
                    self.starting.removeValue(forKey: spec.id)
                    self.failed[spec.id] = TunnelRuntimeState(
                        id: spec.id, name: spec.name, phase: .failed, utunName: nil,
                        errorMessage: error.localizedDescription, peers: [], since: nil)
                }
                self.queue.sync { self.rebuildKillSwitch() }
                flog.error("start", "✕ FAIL “\(spec.name)”: \(error.localizedDescription)")
            }
        }
    }

    /// Runs in the background (workQueue). Dictionary mutations go through queue.sync.
    private func startLocked(spec: ResolvedTunnelSpec) throws {
        let t0 = Date()
        flog.info("start", "▶︎ START “\(spec.name)” [\(spec.kind.rawValue)] id=\(spec.id.uuidString.prefix(8)) peers=\(spec.peers.count) routes=\(spec.routes.count) dns=\(spec.dnsServers) killSwitch=\(spec.killSwitch)")

        // Core selection: WireGuard or AmneziaWG (a single v0.2.x core handles 1.5 & 2.0).
        let coreName = spec.kind.coreBinary
        guard let core = Self.locateCore(coreName) else {
            flog.error("start", "core binary not found: \(coreName). Looked next to \(Self.executableDir().path)")
            throw DaemonError("core binary not found: \(coreName)")
        }
        flog.info("start", "1/8 core: \(core.lastPathComponent) (\(spec.kind.label))")

        // Resolve endpoints BEFORE spawning the process.
        flog.debug("start", "2/8 resolving endpoints…")
        let endpoints: [Int: String]
        do {
            endpoints = try ConfigRenderer.resolveEndpoints(spec: spec)
        } catch {
            flog.error("start", "endpoint resolution failed: \(error.localizedDescription)")
            throw error
        }
        for (i, ep) in endpoints.sorted(by: { $0.key < $1.key }) {
            flog.debug("start", "   peer#\(i) endpoint → \(ep)")
        }

        let nameFile = Paths.runDir + "/\(spec.id.uuidString).name"
        try? FileManager.default.removeItem(atPath: nameFile)

        let p = Process()
        p.executableURL = core
        p.arguments = ["utun"]
        // LOG_LEVEL=verbose → the core logs handshake/transport in detail; we capture stderr into our log.
        let coreLogLevel = ProcessInfo.processInfo.environment["TUNHUB_CORE_LOG"] ?? "verbose"
        p.environment = [
            "WG_PROCESS_FOREGROUND": "1",
            "WG_TUN_NAME_FILE": nameFile,
            "LOG_LEVEL": coreLogLevel,
            // Ownership stamp: lets us positively identify this process as ours later
            // (macOS won't let us rename the utun interface itself).
            TunHub.ownerEnvKey: spec.id.uuidString
        ]
        // Capture the core's stderr/stdout → flog (category core:<name>).
        let corePipe = Pipe()
        p.standardError = corePipe
        p.standardOutput = corePipe
        let tunName = spec.name
        corePipe.fileHandleForReading.readabilityHandler = { fh in
            let data = fh.availableData
            guard !data.isEmpty, let s = String(data: data, encoding: .utf8) else { return }
            for line in s.split(separator: "\n") where !line.isEmpty {
                flog.debug("core:\(tunName)", String(line))
            }
        }
        let id = spec.id
        p.terminationHandler = { [weak self] proc in
            corePipe.fileHandleForReading.readabilityHandler = nil
            self?.queue.async { self?.handleTermination(id: id, status: proc.terminationStatus) }
        }
        flog.debug("start", "3/8 spawning \(coreName) utun (LOG_LEVEL=\(coreLogLevel))…")
        do { try p.run() } catch {
            flog.error("start", "failed to launch core: \(error)")
            throw error
        }
        flog.debug("start", "   core pid=\(p.processIdentifier)")

        guard let utun = waitForNameFile(nameFile, timeout: 8) else {
            flog.error("start", "4/8 core did not create utun (8s timeout), killing pid=\(p.processIdentifier)")
            p.terminate()
            throw DaemonError("core did not create a utun interface (timeout)")
        }
        flog.info("start", "4/8 interface: \(utun)")
        let sockDir = spec.kind == .amneziawg ? "/var/run/amneziawg" : "/var/run/wireguard"
        let sock = "\(sockDir)/\(utun).sock"
        guard waitForPath(sock, timeout: 8) else {
            flog.error("start", "5/8 UAPI socket \(sock) did not appear (timeout)")
            p.terminate()
            throw DaemonError("UAPI socket did not appear (timeout)")
        }
        flog.debug("start", "5/8 UAPI socket: \(sock)")

        let rt = RunningTunnel(spec: spec, process: p, utun: utun,
                               socketPath: sock, resolvedEndpoints: endpoints)
        // Register as running (on the serial queue).
        queue.sync {
            starting.removeValue(forKey: spec.id)
            running[spec.id] = rt
        }

        do {
            let uapi = try ConfigRenderer.uapiSet(spec: spec, resolvedEndpoints: endpoints)
            flog.debug("start", "6/8 UAPI set=1 (\(uapi.split(separator: "\n").count) lines, awg=\(spec.awg != nil))")
            // Full UAPI string with secrets redacted (for comparison with the official client).
            for line in uapi.split(separator: "\n") {
                var l = String(line)
                if l.hasPrefix("private_key=") { l = "private_key=<hidden>" }
                if l.hasPrefix("preshared_key=") { l = "preshared_key=<hidden>" }
                if l.hasPrefix("public_key=") { l = "public_key=" + l.dropFirst(11).prefix(12) + "…" }
                if l.hasPrefix("allowed_ip=") { continue }  // there are many, skip them
                flog.debug("uapi", l)
            }
            flog.debug("uapi", "(allowed_ip lines: \(uapi.split(separator: "\n").filter { $0.hasPrefix("allowed_ip=") }.count))")
            try UAPIClient.set(socketPath: sock, config: uapi)
            flog.debug("start", "   UAPI applied")

            flog.debug("start", "7/8 ifconfig: addresses \(spec.addresses.map(\.canonical)) mtu=\(spec.mtu.map(String.init) ?? "-")")
            try configureInterface(utun: utun, spec: spec)

            flog.debug("start", "8/8 routes (\(spec.routes.count)) + DNS (\(spec.dnsMode))…")
            try RouteManager.shared.apply(spec: spec, utun: utun, resolvedEndpoints: endpoints)
            try DNSManager.shared.apply(spec: spec)
            queue.sync {
                rt.phase = .up
                rt.since = Date()
                rebuildKillSwitch()
                persistState()
            }
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            flog.info("start", "✔︎ UP “\(spec.name)” on \(utun) in \(ms)ms")
            dlog.info("tunnel \(spec.name, privacy: .public) up on \(utun, privacy: .public)")
        } catch {
            flog.error("start", "✕ FAIL “\(spec.name)”: \(error.localizedDescription) — rolling back")
            teardown(rt, killProcess: true)
            queue.sync { _ = running.removeValue(forKey: spec.id) }
            throw error
        }
    }

    private func configureInterface(utun: String, spec: ResolvedTunnelSpec) throws {
        for a in spec.addresses {
            let res: CommandResult
            if a.isIPv6 {
                res = run("/sbin/ifconfig", [utun, "inet6", "\(a.addressString)/\(a.prefix)", "alias"])
            } else {
                res = run("/sbin/ifconfig", [utun, "inet", "\(a.addressString)/\(a.prefix)", a.addressString, "alias"])
            }
            guard res.ok else { throw DaemonError("ifconfig address failed: \(res.stderr)") }
        }
        if let mtu = spec.mtu {
            run("/sbin/ifconfig", [utun, "mtu", String(mtu)])
        }
        let up = run("/sbin/ifconfig", [utun, "up"])
        guard up.ok else { throw DaemonError("ifconfig up failed: \(up.stderr)") }
    }

    // MARK: Stop

    // id → name, to show the .stopping phase while the rollback runs in the background
    private var stopping: [UUID: String] = [:]

    /// Non-blocking stop: XPC returns instantly, the heavy rollback (routes/DNS) runs in the background.
    func stop(id: UUID) throws {
        let rt: RunningTunnel? = queue.sync {
            guard let rt = running[id] else {
                flog.debug("stop", "stop id=\(id.uuidString.prefix(8)): tunnel not running, clearing failed")
                failed.removeValue(forKey: id)
                return nil
            }
            flog.info("stop", "■ STOP “\(rt.spec.name)” on \(rt.utun) (pid=\(rt.process.processIdentifier)) → rollback in background")
            rt.intentionalStop = true
            rt.phase = .stopping
            running.removeValue(forKey: id)
            stopping[id] = rt.spec.name
            return rt
        }
        guard let rt else { return }
        workQueue.async { [weak self] in
            guard let self else { return }
            let t0 = Date()
            self.teardown(rt, killProcess: true)
            self.queue.sync {
                self.stopping.removeValue(forKey: id)
                self.rebuildKillSwitch()
                self.persistState()
            }
            flog.info("stop", "✔︎ STOPPED “\(rt.spec.name)” (rollback \(Int(Date().timeIntervalSince(t0)*1000))ms)")
        }
    }

    func stopAll() {
        queue.sync {
            flog.info("stop", "■ STOP ALL (\(running.count) active)")
            for rt in running.values {
                rt.intentionalStop = true
                teardown(rt, killProcess: true)
            }
            running.removeAll()
            FirewallManager.shared.disable()
            persistState()
            flog.info("stop", "✔︎ all tunnels stopped")
        }
    }

    private func teardown(_ rt: RunningTunnel, killProcess: Bool) {
        flog.debug("stop", "teardown “\(rt.spec.name)”: rolling back DNS…")
        DNSManager.shared.rollback(id: rt.spec.id)
        flog.debug("stop", "teardown “\(rt.spec.name)”: rolling back routes…")
        RouteManager.shared.rollback(id: rt.spec.id)
        if killProcess && rt.process.isRunning {
            flog.debug("stop", "teardown “\(rt.spec.name)”: SIGTERM pid=\(rt.process.processIdentifier)")
            rt.process.terminate()
            let deadline = Date().addingTimeInterval(3)
            while rt.process.isRunning && Date() < deadline { usleep(100_000) }
            if rt.process.isRunning {
                flog.warn("stop", "teardown “\(rt.spec.name)”: did not exit within 3s, SIGKILL")
                kill(rt.process.processIdentifier, SIGKILL)
            }
        }
    }

    // MARK: Crash of a core process

    private func handleTermination(id: UUID, status: Int32) {
        guard let rt = running[id], !rt.intentionalStop else { return }
        flog.error("crash", "☠︎ core “\(rt.spec.name)” (\(rt.utun)) crashed, exit=\(status)")
        dlog.error("core process for \(rt.spec.name, privacy: .public) died (status \(status))")
        teardown(rt, killProcess: false)
        running.removeValue(forKey: id)
        rebuildKillSwitch()

        if rt.restartCount < 3 {
            let attempt = rt.restartCount + 1
            let delay = pow(2.0, Double(attempt))
            flog.warn("crash", "auto-restarting “\(rt.spec.name)” in \(Int(delay))s (attempt \(attempt)/3)")
            queue.asyncAfter(deadline: .now() + delay) { [weak self] in
                guard let self else { return }
                do {
                    try self.startLocked(spec: rt.spec)
                    self.running[id]?.restartCount = attempt
                } catch {
                    self.markFailed(rt, "restart failed: \(error.localizedDescription)")
                }
            }
        } else {
            markFailed(rt, "tunnel core crashed (after 3 restarts)")
        }
        persistState()
    }

    private func markFailed(_ rt: RunningTunnel, _ message: String) {
        flog.error("crash", "✕ FAILED “\(rt.spec.name)”: \(message)")
        failed[rt.spec.id] = TunnelRuntimeState(
            id: rt.spec.id, name: rt.spec.name, phase: .failed,
            utunName: nil, errorMessage: message, peers: [], since: nil)
    }

    // MARK: Kill switch

    func rebuildKillSwitch() {
        guard killSwitchGloballyEnabled else { FirewallManager.shared.disable(); return }
        let infos: [FirewallManager.ActiveTunnelInfo] = running.values
            .filter { $0.spec.killSwitch }
            .map { rt in
                let eps = rt.resolvedEndpoints.values.compactMap { ep -> (String, UInt16)? in
                    guard let (h, p) = EndpointUtil.split(ep) else { return nil }
                    return (h, p)
                }
                return .init(utun: rt.utun, endpoints: eps)
            }
        FirewallManager.shared.rebuild(infos)
    }

    func setKillSwitchEnabled(_ enabled: Bool) {
        queue.sync {
            killSwitchGloballyEnabled = enabled
            rebuildKillSwitch()
        }
    }

    // MARK: Stats

    func startStatsLoop() {
        // Poll every 0.5s so the speed graph reacts almost immediately to traffic.
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.5, repeating: 0.5)
        t.setEventHandler { [weak self] in self?.pollStats() }
        t.resume()
        statsTimer = t
    }

    private var lastDiag: [UUID: Date] = [:]
    private var lastLoopCheck: [UUID: Date] = [:]

    private func pollStats() {
        for rt in running.values where rt.phase == .up || rt.phase == .degraded {
            // Endpoint-loop monitor (like wg-quick monitor_daemon): only relevant for
            // default-route tunnels, where the tunnel's own routes could capture the
            // endpoint and loop it. Split tunnels can't loop (endpoint isn't in routes),
            // so we skip the (costly, chatty) route lookups for them. Interval 15s.
            if rt.isDefaultRoute {
                let loopDue = lastLoopCheck[rt.spec.id].map { Date().timeIntervalSince($0) > 15 } ?? true
                if loopDue {
                    lastLoopCheck[rt.spec.id] = Date()
                    let eps = Array(rt.resolvedEndpoints.values)
                    if RouteManager.shared.ensureEndpointNotLooped(id: rt.spec.id, endpoints: eps, tunnelUtun: rt.utun) {
                        flog.warn("stats", "“\(rt.spec.name)”: endpoint loop detected and fixed (re-pin)")
                    }
                }
            }
            if let peers = try? UAPIClient.get(socketPath: rt.socketPath) {
                rt.peers = peers
                let lastHS = peers.compactMap(\.lastHandshake).max()
                let fresh = lastHS.map { Date().timeIntervalSince($0) < 185 } ?? false
                let rx = peers.reduce(0) { $0 + $1.rxBytes }
                let tx = peers.reduce(0) { $0 + $1.txBytes }
                let sending = tx > rt.lastTx
                let receiving = rx > rt.lastRx

                // Health model (traffic-aware): a tunnel is UP when it's fresh, receiving,
                // or simply idle. It is DEGRADED only when it keeps SENDING but gets NO
                // replies for a sustained window — the real "black hole" signal. This stops
                // healthy idle/split tunnels from being flagged just for a lazy handshake.
                let newPhase: TunnelPhase
                if fresh || receiving {
                    rt.stalledSince = nil
                    newPhase = .up
                } else if sending && !receiving {
                    if rt.stalledSince == nil { rt.stalledSince = Date() }
                    newPhase = (Date().timeIntervalSince(rt.stalledSince!) > 12) ? .degraded : .up
                } else {
                    // idle: not sending, not receiving, handshake stale → still healthy
                    rt.stalledSince = nil
                    newPhase = .up
                }

                if newPhase != rt.phase {
                    let age = lastHS.map { Int(Date().timeIntervalSince($0)) }
                    flog.warn("stats", "“\(rt.spec.name)” \(rt.phase.rawValue) → \(newPhase.rawValue) "
                        + "(last handshake: \(age.map { "\($0)s ago" } ?? "never"), "
                        + "tx=\(ByteFormat.human(tx)) rx=\(ByteFormat.human(rx)))")
                    if newPhase == .degraded {
                        flog.error("diag", "“\(rt.spec.name)”: sending (tx=\(ByteFormat.human(tx))) but NO replies (rx=\(ByteFormat.human(rx))) for >12s → server unreachable via this path OR AWG params mismatch (Jc/S1/S2/H1–H4). endpoint=\(rt.resolvedEndpoints.values.joined(separator: ","))")
                        if rt.isDefaultRoute {
                            flog.error("diag", "“\(rt.spec.name)” — DEFAULT-ROUTE tunnel is black-holing ALL system traffic. Stop it or fix the handshake.")
                        }
                    }
                }
                // Periodic trace every ~20s for live diagnostics.
                let due = lastDiag[rt.spec.id].map { Date().timeIntervalSince($0) > 20 } ?? true
                if due {
                    lastDiag[rt.spec.id] = Date()
                    let age = lastHS.map { Int(Date().timeIntervalSince($0)) }
                    flog.debug("stats", "“\(rt.spec.name)” \(newPhase.rawValue) hs=\(age.map{"\($0)s"} ?? "never") tx=\(ByteFormat.human(tx)) rx=\(ByteFormat.human(rx))")
                }
                rt.lastTx = tx
                rt.lastRx = rx
                rt.phase = newPhase
            } else {
                flog.warn("stats", "“\(rt.spec.name)”: UAPI did not respond (core hung?)")
            }
        }
    }

    // MARK: State for UI / recovery

    func states() -> [TunnelRuntimeState] {
        queue.sync {
            var out = running.values.map { rt in
                TunnelRuntimeState(id: rt.spec.id, name: rt.spec.name, phase: rt.phase,
                                   utunName: rt.utun, errorMessage: rt.lastError,
                                   peers: rt.peers, since: rt.since)
            }
            // Tunnels accepted and still coming up.
            for (id, info) in starting where running[id] == nil {
                out.append(TunnelRuntimeState(id: id, name: info.name, phase: .starting,
                                              utunName: nil, errorMessage: nil, peers: [], since: info.since))
            }
            // Tunnels being stopped (route/DNS rollback in progress).
            for (id, name) in stopping where running[id] == nil {
                out.append(TunnelRuntimeState(id: id, name: name, phase: .stopping,
                                              utunName: nil, errorMessage: nil, peers: [], since: nil))
            }
            out.append(contentsOf: failed.values)
            return out
        }
    }

    /// One record per interface we own. Lets us later prove a running process is
    /// genuinely ours before touching it — macOS can't rename utun interfaces, so
    /// ownership is tracked here instead of on the interface name.
    private struct OwnedInterface: Codable {
        var tunnelID: UUID
        var name: String        // tunnel name (for logs)
        var utun: String        // utunN assigned by the kernel
        var pid: Int32
        var core: String        // expected process basename (wireguard-go / amneziawg-go)
    }
    private struct PersistedState: Codable {
        var owned: [OwnedInterface]
    }

    private func persistState() {
        let owned = running.values.map {
            OwnedInterface(tunnelID: $0.spec.id, name: $0.spec.name, utun: $0.utun,
                           pid: $0.process.processIdentifier, core: $0.spec.kind.coreBinary)
        }
        let s = PersistedState(owned: owned)
        if let d = try? TunJSON.encoder.encode(s) {
            try? d.write(to: URL(fileURLWithPath: Paths.ownership), options: .atomic)
        }
    }

    /// True only if `pid` is alive AND its executable basename matches the core we
    /// spawned — so we never SIGTERM a reused PID or another app's WG/AWG process.
    private func processIsOurs(pid: Int32, core: String) -> Bool {
        guard pid > 0, kill(pid, 0) == 0 else { return false }
        let comm = run("/bin/ps", ["-p", String(pid), "-o", "comm="]).stdout
            .trimmingCharacters(in: .whitespacesAndNewlines)
        return (comm as NSString).lastPathComponent == core
    }

    /// After a daemon crash: reclaim ONLY our own core processes (verified by the
    /// ownership registry), kill the orphans, then roll back DNS/pf.
    func crashRecovery() {
        if let d = try? Data(contentsOf: URL(fileURLWithPath: Paths.ownership)),
           let s = try? TunJSON.decoder.decode(PersistedState.self, from: d) {
            for iface in s.owned {
                if processIsOurs(pid: iface.pid, core: iface.core) {
                    dlog.warning("killing orphaned core pid \(iface.pid) (\(iface.utun), ours)")
                    kill(iface.pid, SIGTERM)
                } else {
                    flog.debug("recover", "pid \(iface.pid) for \(iface.utun) is not ours (dead or reused) — skipping")
                }
            }
            try? FileManager.default.removeItem(atPath: Paths.ownership)
        }
        DNSManager.shared.crashRecovery()
        FirewallManager.shared.crashRecovery()
    }

    // MARK: helpers

    private func waitForNameFile(_ path: String, timeout: TimeInterval) -> String? {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if let s = try? String(contentsOfFile: path, encoding: .utf8) {
                let name = s.trimmingCharacters(in: .whitespacesAndNewlines)
                if !name.isEmpty { return name }
            }
            usleep(100_000)
        }
        return nil
    }

    private func waitForPath(_ path: String, timeout: TimeInterval) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if FileManager.default.fileExists(atPath: path) { return true }
            usleep(100_000)
        }
        return false
    }
}
