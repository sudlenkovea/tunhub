import Foundation
import SwiftUI
import TunHubShared

enum DaemonVersionStatus: Equatable {
    case unknown
    case checking
    case ok(String)                        // versions matched
    case mismatch(installed: String, expected: String)
    case unreachable                       // daemon not responding

    var isProblem: Bool {
        switch self {
        case .mismatch, .unreachable: return true
        default: return false
        }
    }
}

/// App-side log (UI / XPC client). File lives in ~/Library/Logs/TunHub/.
let applog: FileLog = {
    let dir = FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
        .appendingPathComponent(TunHub.AppPath.logsDir, isDirectory: true)
    return FileLog(path: dir.appendingPathComponent("app.log").path)
}()

/// A request to collect OpenVPN credentials / OTP before connecting.
struct OVPNCredentialRequest: Identifiable {
    let id: UUID                 // tunnel id
    let config: TunnelConfig
    let needsUsername: Bool
    let staticChallenge: OpenVPNStaticChallenge?
}

/// A failed connection surfaced to the user with retry/cancel choices.
struct ConnectionFailure: Identifiable {
    let id: UUID                 // tunnel id
    let config: TunnelConfig
    let message: String
    /// Whether the failure looks like bad credentials (→ retry re-prompts login/password).
    var isAuth: Bool { message.uppercased().contains("AUTH") || message.lowercased().contains("verification") }
}

struct StatSample: Identifiable {
    let id = UUID()
    let t: Date
    let rx: UInt64
    let tx: UInt64
    let rxRate: Double
    let txRate: Double
}

@MainActor
final class AppState: ObservableObject {
    @Published var tunnels: [TunnelConfig] = []
    @Published var runtime: [UUID: TunnelRuntimeState] = [:]
    @Published var history: [UUID: [StatSample]] = [:]      // ~2h at a 2s poll interval
    @Published var externalIPs: [UUID: String] = [:]         // short status shown inline
    @Published var externalIPDetails: [UUID: String] = [:]   // full explanation (hover help)
    @Published var daemonReachable = false
    @Published var daemonInstalled = DaemonManager.isEnabled
    @Published var lastError: String?
    @Published var pendingStop: Set<UUID> = []     // optimistic "stopping" phase
    // Daemon version
    @Published var daemonVersion: String?          // what the daemon reports
    @Published var daemonVersionStatus: DaemonVersionStatus = .unknown
    @Published var showDaemonUpdateSheet = false
    @Published var daemonBusy = false              // install/restart in progress
    // Import
    @Published var importCandidates: [ImportCandidate] = []
    @Published var importErrors: [String] = []
    @Published var showImportSheet = false
    // Conflicts on start
    @Published var blockedFindings: [ConflictFinding] = []
    @Published var showConflictSheet = false
    // OpenVPN credential / OTP prompt
    @Published var credentialRequest: OVPNCredentialRequest?
    // A connection that failed and awaits a user decision (retry / cancel)
    @Published var connectionFailure: ConnectionFailure?
    private var reportedFailures: Set<UUID> = []

    let store = TunnelStore()
    let daemon = DaemonClient()
    let ledger = TrafficLedger()
    let health = HealthChecker()
    private var pollTask: Task<Void, Never>?

    init() {
        applog.info("app", "═══ TunHub UI started, log: \(applog.filePath) ═══")
        tunnels = store.loadAll()
        applog.info("app", "loaded tunnels: \(tunnels.count), daemon installed: \(DaemonManager.isEnabled)")
        Notifier.setup()
        startPolling()
        Task {
            await checkDaemonVersion()
            await healDaemonIfNeeded()
            await poll()          // pick up tunnels the daemon is already holding
            await autoConnect()
        }
    }

    private var versionCheckInFlight = false

    /// Check the installed daemon's version against the expected one (this build).
    /// explicit=true — triggered by the user (shows "Checking…" and may re-present the modal).
    @discardableResult
    func checkDaemonVersion(explicit: Bool = true) async -> DaemonVersionStatus {
        if versionCheckInFlight { return daemonVersionStatus }
        versionCheckInFlight = true
        defer { versionCheckInFlight = false }

        if explicit { daemonVersionStatus = .checking }
        let expected = kDaemonFullVersion
        let reported = await daemon.version()
        daemonVersion = reported
        let status: DaemonVersionStatus
        if let reported {
            status = (reported == expected) ? .ok(reported)
                                            : .mismatch(installed: reported, expected: expected)
        } else {
            status = (DaemonManager.isEnabled || DaemonManager.classicPlistInstalled)
                ? .unreachable : .unknown
        }
        // Don't touch @Published if nothing changed (otherwise the UI flickers).
        if daemonVersionStatus != status {
            daemonVersionStatus = status
            applog.info("daemon", "version: installed=\(reported ?? "no reply"), expected=\(expected) → \(status)")
        }
        // Raise the modal only if there's a problem and it isn't already shown. The sheet
        // lives in the main window, so proactively open the window — a menu-bar app has no
        // window on launch, and we want the "update system component" prompt to appear
        // without waiting for the user to open it.
        if status.isProblem, !showDaemonUpdateSheet {
            showDaemonUpdateSheet = true
            WindowManager.shared.showMain()
        }
        return status
    }

    /// If the daemon isn't responding, try to bring it up. For a classic daemon (in
    /// /Library/LaunchDaemons) that's privilegedRestart (one password prompt); for
    /// SMAppService it re-registers.
    func healDaemonIfNeeded() async {
        // A classic daemon is usually alive; check with a ping.
        if await daemon.ping() { applog.info("daemon", "daemon is responding"); daemonInstalled = true; return }
        if DaemonManager.classicPlistInstalled {
            applog.warn("daemon", "classic daemon not responding — will offer a restart (needs a password)")
            // Don't prompt for a password automatically on launch; the user clicks "Restart".
            return
        }
        guard DaemonManager.isEnabled else { return }
        applog.warn("daemon", "daemon installed (SMAppService) but not responding — re-registering")
        do {
            try DaemonManager.uninstall()
            try? await Task.sleep(nanoseconds: 800_000_000)
            try DaemonManager.install()
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            let ok = await daemon.ping()
            applog.info("daemon", ok ? "daemon came up after re-registration"
                        : "daemon still not responding — may need approval in System Settings → Login Items")
            if !ok, DaemonManager.service.status == .requiresApproval {
                DaemonManager.openLoginItemsSettings()
            }
        } catch {
            applog.error("daemon", "re-registration failed: \(error.localizedDescription)")
        }
        daemonInstalled = DaemonManager.isEnabled
    }

    /// Reinstall/restart the daemon with a password prompt, then RE-CHECK the version.
    /// The modal stays open and shows the final status (ok / still a problem).
    func restartDaemon() async {
        applog.info("daemon", "daemon reinstall requested (privilegedRestart)")
        daemonBusy = true
        defer { daemonBusy = false }
        // Stop active tunnels BEFORE updating the core/daemon (clean route/DNS rollback).
        if anyUp {
            applog.info("daemon", "stopping active tunnels before the daemon update")
            await stopAll()
        }
        let result = await Task.detached { DaemonManager.privilegedRestart() }.value
        if !result.ok {
            lastError = "daemon restart: \(result.message)"
            applog.error("daemon", "restart failed: \(result.message)")
            // Status stays problematic — the modal keeps insisting.
            await checkDaemonVersion()
            return
        }
        lastError = nil
        applog.info("daemon", "daemon restarted, re-checking version…")
        // Give the daemon time to come up and re-check (a few attempts).
        for _ in 0..<5 {
            try? await Task.sleep(nanoseconds: 700_000_000)
            let st = await checkDaemonVersion()
            if case .ok = st { break }
        }
        await poll()
        if case .ok = daemonVersionStatus {
            // Don't auto-close — show the green "all good"; the user closes it.
            applog.info("daemon", "version matched after reinstall")
        }
    }

    // MARK: Polling

    func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.poll()
                try? await Task.sleep(nanoseconds: 500_000_000)   // 0.5s — responsive speed graph
            }
        }
    }

    func poll() async {
        let states = await daemon.runtimeStates()
        // The daemon counts as reachable if it replied over XPC (classic LaunchDaemon or SMAppService).
        let alive = await daemon.ping()
        daemonReachable = alive
        daemonInstalled = DaemonManager.isEnabled || alive
        // Version is checked only on launch (and on demand) — do NOT touch it here so the modal doesn't flicker.
        var newRuntime: [UUID: TunnelRuntimeState] = [:]
        let now = Date()
        for s in states {
            newRuntime[s.id] = s
            // rates
            let prev = history[s.id]?.last
            var rxRate = 0.0, txRate = 0.0
            if let prev, s.rxTotal >= prev.rx, s.txTotal >= prev.tx {
                let dt = now.timeIntervalSince(prev.t)
                if dt > 0.5 {
                    rxRate = Double(s.rxTotal - prev.rx) / dt
                    txRate = Double(s.txTotal - prev.tx) / dt
                }
            }
            var h = history[s.id] ?? []
            h.append(StatSample(t: now, rx: s.rxTotal, tx: s.txTotal, rxRate: rxRate, txRate: txRate))
            if h.count > 3600 { h.removeFirst(h.count - 3600) }
            history[s.id] = h
            ledger.update(id: s.id, rx: s.rxTotal, tx: s.txTotal)
        }
        // Tunnels that disappeared or stopped → clear graph history and counters so the
        // graph doesn't "freeze" on the last values.
        for id in Set(history.keys) where newRuntime[id] == nil {
            history[id] = nil
            ledger.tunnelReset(id: id)
            externalIPs[id] = nil
        }
        // Log phase changes + auto-check external IP on transition to up.
        for (id, s) in newRuntime where lastPhases[id] != s.phase {
            let from = lastPhases[id]?.rawValue ?? "—"
            let extra = s.errorMessage.map { ": \($0)" } ?? ""
            applog.info("phase", "“\(s.name)” \(from) → \(s.phase.rawValue)\(extra)")
            if s.phase == .up, lastPhases[id] != .up, let cfg = tunnels.first(where: { $0.id == id }) {
                fetchExternalIP(cfg)   // via the utun interface, works even without a default route
            }
            // A failed connection → surface a retry/cancel prompt (once per failure episode).
            if s.phase == .failed, !reportedFailures.contains(id),
               let cfg = tunnels.first(where: { $0.id == id }) {
                reportedFailures.insert(id)
                connectionFailure = ConnectionFailure(
                    id: id, config: cfg,
                    message: s.errorMessage ?? String(localized: "Connection failed."))
                WindowManager.shared.showMain()
            }
            if s.phase != .failed { reportedFailures.remove(id) }
            lastPhases[id] = s.phase
        }
        for id in lastPhases.keys where newRuntime[id] == nil { lastPhases[id] = nil }
        runtime = newRuntime
        // Clear the optimistic "stopping" once the daemon confirms (tunnel gone or already .stopped).
        for id in pendingStop where newRuntime[id] == nil || newRuntime[id]?.phase == .stopped {
            pendingStop.remove(id)
        }
        // Auto-connect once, as soon as the daemon actually answers — covers the case where
        // the helper comes up only after launch (e.g. after a password-gated reinstall).
        if daemonReachable && !didAutoConnect { await autoConnect() }
        health.tick(app: self)
    }
    private var lastPhases: [UUID: TunnelPhase] = [:]

    /// Display phase, accounting for the optimistic stop.
    func displayPhase(_ config: TunnelConfig) -> TunnelPhase? {
        if pendingStop.contains(config.id) { return .stopping }
        return runtime[config.id]?.phase
    }

    // MARK: Lifecycle

    func isRunning(_ config: TunnelConfig) -> Bool {
        guard let s = runtime[config.id] else { return false }
        return s.phase == .up || s.phase == .degraded || s.phase == .starting
    }

    func toggle(_ config: TunnelConfig) {
        Task {
            if isRunning(config) { await stop(config) }
            else { try? await start(config) }
        }
    }

    /// force=true skips the interactive conflict prompt (used by failover/restart).
    func start(_ config: TunnelConfig, force: Bool = false) async throws {
        applog.info("start", "start requested “\(config.name)” (force=\(force))")
        let activeConfigs = tunnels.filter { isRunning($0) }
        let findings = ConflictChecker.check(candidate: config, against: activeConfigs)
        if ConflictChecker.hasErrors(findings) {
            let codes = findings.filter { $0.severity == .error }.map(\.code).joined(separator: ", ")
            applog.warn("start", "“\(config.name)” blocked by conflicts: \(codes)")
            blockedFindings = findings
            showConflictSheet = true
            throw AppError("conflicts block starting “\(config.name)”")
        }
        if !findings.isEmpty && !force {
            blockedFindings = findings   // show, but don't block
        }
        // OpenVPN may need username/password and/or a one-time OTP before connecting —
        // ask the user via a sheet; the actual start resumes in submitOVPNCredentials().
        if config.kind == .openvpn, let profile = config.openvpn, needsCredentialPrompt(config, profile) {
            applog.info("start", "“\(config.name)”: prompting for OpenVPN credentials/OTP")
            credentialRequest = OVPNCredentialRequest(
                id: config.id, config: config,
                needsUsername: profile.needsUsername || profile.authMode != .cert,
                staticChallenge: profile.staticChallenge)
            WindowManager.shared.showMain()   // ensure the sheet has a window to appear in
            return
        }
        try await performStart(config, otp: nil)
    }

    /// The actual start: resolve → send to the daemon. Kept separate so the OpenVPN
    /// credential sheet can resume it once the user has entered creds/OTP.
    func performStart(_ config: TunnelConfig, otp: String?,
                      ovpnUser: String? = nil, ovpnPass: String? = nil) async throws {
        let spec: ResolvedTunnelSpec
        do {
            spec = try resolveSpec(config, otp: otp, ovpnUser: ovpnUser, ovpnPass: ovpnPass)
        } catch {
            applog.error("start", "“\(config.name)”: failed to build spec: \(error.localizedDescription)")
            lastError = "“\(config.name)”: \(error.localizedDescription)"
            throw error
        }
        do {
            try await daemon.startTunnel(spec)
            applog.info("start", "daemon accepted start of “\(config.name)”")
            markConnected(config)
        } catch {
            applog.error("start", "daemon rejected “\(config.name)”: \(error.localizedDescription)")
            lastError = "“\(config.name)”: \(error.localizedDescription)"
            throw error
        }
        await poll()
    }

    // MARK: OpenVPN credentials (savable ahead of time, not only at connect)

    /// Current saved OpenVPN credentials for a tunnel: username + whether a password is stored.
    func ovpnCredentials(_ config: TunnelConfig) -> (username: String, hasPassword: Bool) {
        let s = KeychainService.loadSecrets(tunnelID: config.id)
        return (s?.openvpn["username"] ?? "", !(s?.openvpn["password"] ?? "").isEmpty)
    }

    /// Save username (and password if non-empty) to the Keychain for an OpenVPN tunnel.
    func saveOVPNCredentials(_ config: TunnelConfig, username: String, password: String) {
        var s = KeychainService.loadSecrets(tunnelID: config.id) ?? .init(privateKey: "")
        s.openvpn["username"] = username
        if password.isEmpty { s.openvpn["password"] = nil } else { s.openvpn["password"] = password }
        KeychainService.saveSecrets(tunnelID: config.id, s)
        applog.info("ovpn", "saved credentials for “\(config.name)” (password: \(password.isEmpty ? "no" : "yes"))")
    }

    /// Remove the stored password (keeps the username).
    func forgetOVPNPassword(_ config: TunnelConfig) {
        guard var s = KeychainService.loadSecrets(tunnelID: config.id) else { return }
        s.openvpn["password"] = nil
        KeychainService.saveSecrets(tunnelID: config.id, s)
    }

    /// Retry a failed connection. For an auth failure, forget the (wrong) saved password so
    /// the start re-prompts for credentials.
    func retryConnection(_ f: ConnectionFailure) {
        connectionFailure = nil
        reportedFailures.remove(f.id)
        Task {
            await stop(f.config)   // clear the daemon's failed entry
            if f.isAuth, f.config.kind == .openvpn { forgetOVPNPassword(f.config) }
            try? await start(f.config)
        }
    }

    /// Dismiss a failed connection and tidy up the daemon-side failed entry.
    func dismissConnectionFailure(_ f: ConnectionFailure) {
        connectionFailure = nil
        Task { await stop(f.config) }
    }

    private func needsCredentialPrompt(_ config: TunnelConfig, _ profile: OpenVPNProfile) -> Bool {
        if profile.staticChallenge != nil { return true }   // always ask for a fresh OTP
        guard profile.needsUsername || profile.authMode != .cert else { return false }
        let secrets = KeychainService.loadSecrets(tunnelID: config.id)
        let hasUser = !(secrets?.openvpn["username"]?.isEmpty ?? true)
        let hasPass = !(secrets?.openvpn["password"]?.isEmpty ?? true)
        return !(hasUser && hasPass)
    }

    /// Called by the OpenVPN credential sheet. Persists creds (username always; password if
    /// asked), then resumes the start with the entered OTP.
    func submitOVPNCredentials(username: String, password: String, savePassword: Bool, otp: String?) {
        guard let req = credentialRequest else { return }
        credentialRequest = nil
        if req.needsUsername {
            var s = KeychainService.loadSecrets(tunnelID: req.id) ?? .init(privateKey: "")
            s.openvpn["username"] = username
            if savePassword { s.openvpn["password"] = password } else { s.openvpn["password"] = nil }
            KeychainService.saveSecrets(tunnelID: req.id, s)
        }
        Task {
            try? await performStart(req.config, otp: otp,
                                    ovpnUser: req.needsUsername ? username : nil,
                                    ovpnPass: (req.needsUsername && !savePassword) ? password : nil)
        }
    }

    func stop(_ config: TunnelConfig) async {
        applog.info("stop", "stop requested “\(config.name)”")
        pendingStop.insert(config.id)          // instant "stopping" indication
        try? await daemon.stopTunnel(id: config.id)
        externalIPs[config.id] = nil
        await poll()
    }

    func stopAll() async {
        applog.info("stop", "stop requested for ALL tunnels")
        await daemon.stopAll()
        externalIPs.removeAll()
        await poll()
    }

    private func markConnected(_ config: TunnelConfig) {
        if let i = tunnels.firstIndex(where: { $0.id == config.id }) {
            tunnels[i].meta.lastConnectedAt = Date()
            try? store.save(tunnels[i])
        }
    }

    private var didAutoConnect = false

    /// Connect every tunnel flagged "Connect on app launch". Idempotent (skips already-running
    /// tunnels) and runs at most once per app session.
    func autoConnect() async {
        guard daemonReachable, !didAutoConnect else { return }
        didAutoConnect = true
        let toStart = tunnels.filter { $0.options.autoConnectOnLaunch && !isRunning($0) }
        guard !toStart.isEmpty else { return }
        applog.info("autoconnect", "auto-connecting \(toStart.count) tunnel(s): \(toStart.map(\.name).joined(separator: ", "))")
        for t in toStart {
            // force: skip warning findings during unattended startup (hard ERROR conflicts still block).
            try? await start(t, force: true)
        }
    }

    // MARK: Secrets → spec

    func resolveSpec(_ config: TunnelConfig, otp: String? = nil,
                     ovpnUser: String? = nil, ovpnPass: String? = nil) throws -> ResolvedTunnelSpec {
        // One Keychain access for the whole tunnel (a single password prompt).
        // If the combined item is missing, migrate from the old (per-item) scheme.
        guard let secrets = KeychainService.loadSecrets(tunnelID: config.id)
                ?? KeychainService.migrateLegacySecrets(config: config) else {
            throw AppError("secrets for “\(config.name)” not found in Keychain")
        }

        // OpenVPN: inline the secret blocks back into the config text and attach credentials.
        if config.kind == .openvpn, let profile = config.openvpn {
            var text = profile.configText
            for (tag, material) in secrets.openvpn where tag != "username" && tag != "password" {
                text = text.replacingOccurrences(of: "##SECRET:\(tag)##", with: material)
            }
            let resolved = ResolvedOpenVPN(
                configText: text,
                username: ovpnUser ?? secrets.openvpn["username"],
                password: ovpnPass ?? secrets.openvpn["password"],
                otp: otp,
                staticChallenge: profile.staticChallenge,
                remotes: profile.remotes,
                dns: profile.dns,
                redirectGateway: profile.redirectGateway)
            return ResolvedTunnelSpec(
                id: config.id, name: config.name, kind: .openvpn, privateKey: "",
                addresses: [], listenPort: nil, mtu: nil,
                dnsServers: profile.dns, dnsSearchDomains: profile.searchDomains,
                dnsMode: profile.redirectGateway ? .global : .disabled,
                routes: [], awg: nil, killSwitch: config.options.killSwitch, peers: [],
                openvpn: resolved)
        }

        let pk = secrets.privateKey
        let peers = config.peers.map { p in
            ResolvedPeer(publicKey: p.publicKey,
                         presharedKey: secrets.psks[p.id.uuidString],
                         endpoint: p.endpoint,
                         allowedIPs: p.allowedIPs,
                         keepalive: p.persistentKeepalive)
        }
        return ResolvedTunnelSpec(
            id: config.id, name: config.name, kind: config.kind, privateKey: pk,
            addresses: config.interface.addresses,
            listenPort: config.interface.listenPort,
            mtu: config.interface.mtu,
            dnsServers: config.interface.dns,
            dnsSearchDomains: config.interface.dnsSearchDomains,
            dnsMode: config.effectiveDNSMode,
            routes: config.effectiveRoutes(),
            awg: config.awg,
            killSwitch: config.options.killSwitch,
            peers: peers)
    }

    // MARK: CRUD

    func save(_ config: TunnelConfig) {
        do {
            try store.save(config)
            if let i = tunnels.firstIndex(where: { $0.id == config.id }) {
                tunnels[i] = config
            } else {
                tunnels.append(config)
            }
        } catch {
            lastError = "save: \(error.localizedDescription)"
        }
    }

    func delete(_ config: TunnelConfig) {
        Task {
            if isRunning(config) { await stop(config) }
            store.delete(config)
            tunnels.removeAll { $0.id == config.id }
        }
    }

    func duplicate(_ config: TunnelConfig) {
        var copy = config
        copy.id = UUID()
        copy.name = ImportService.uniqueName(config.name, existing: tunnels)
        copy.meta.createdAt = Date()
        let old = KeychainService.loadSecrets(tunnelID: config.id)
        var newSecrets = KeychainService.TunnelSecrets(privateKey: old?.privateKey ?? "")
        copy.interface.privateKeyRef = KeychainService.interfaceRef(copy.id)
        for i in copy.peers.indices {
            let oldPeer = config.peers[i]
            let newID = UUID()
            copy.peers[i].id = newID
            if let psk = old?.psks[oldPeer.id.uuidString] {
                newSecrets.psks[newID.uuidString] = psk
                copy.peers[i].presharedKeyRef = KeychainService.pskRef(copy.id, peerID: newID)
            }
        }
        KeychainService.saveSecrets(tunnelID: copy.id, newSecrets)
        save(copy)
    }

    // MARK: Import / export

    func importFiles(_ urls: [URL]) {
        let (ok, errors) = ImportService.candidates(fromFiles: urls, existing: tunnels)
        importCandidates = ok
        importErrors = errors
        showImportSheet = !ok.isEmpty || !errors.isEmpty
    }

    func importText(_ text: String) {
        do {
            let c = try ImportService.candidate(name: "Imported", text: text, existing: tunnels)
            importCandidates = [c]
            importErrors = []
            showImportSheet = true
        } catch {
            importErrors = [error.localizedDescription]
            importCandidates = []
            showImportSheet = true
        }
    }

    func commitImport() {
        do {
            let saved = try ImportService.commit(importCandidates, store: store)
            tunnels.append(contentsOf: saved)
            tunnels.sort { ($0.meta.sortOrder, $0.name) < ($1.meta.sortOrder, $1.name) }
        } catch {
            lastError = error.localizedDescription
        }
        importCandidates = []
        showImportSheet = false
    }

    // MARK: Diagnostics

    func fetchExternalIP(_ config: TunnelConfig) {
        let id = config.id
        guard let iface = runtime[id]?.utunName, isRunning(config) else {
            externalIPs[id] = String(localized: "tunnel not running")
            return
        }
        externalIPs[id] = String(localized: "checking…")
        externalIPDetails[id] = nil
        applog.info("diag", "external IP “\(config.name)” via \(iface)…")
        TunnelProbe.externalIP(interface: iface, routes: config.effectiveRoutes()) { result in
            Task { @MainActor in
                switch result {
                case .ip(let ip):
                    self.externalIPs[id] = ip
                    self.externalIPDetails[id] = String(localized: "Public IP as seen through this tunnel.")
                    applog.info("diag", "external IP “\(config.name)”: \(ip)")
                case .unreachable(let short, let detail):
                    self.externalIPs[id] = short
                    self.externalIPDetails[id] = detail
                    applog.warn("diag", "external IP “\(config.name)”: \(short) — \(detail)")
                }
            }
        }
    }

    func checkAllConflicts() -> [ConflictFinding] {
        ConflictChecker.checkAll(tunnels)
    }

    // MARK: Derived

    func currentRate(_ id: UUID) -> (rx: Double, tx: Double) {
        guard let s = history[id]?.last else { return (0, 0) }
        return (s.rxRate, s.txRate)
    }

    var anyUp: Bool { runtime.values.contains { $0.phase == .up } }
    var anyDegraded: Bool { runtime.values.contains { $0.phase == .degraded || $0.phase == .failed } }

    func persistOnQuit() { ledger.persist() }
}
