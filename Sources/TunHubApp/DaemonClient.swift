import Foundation
import ServiceManagement
import TunHubShared

/// Guarantees a single resume of the continuation (from reply, errorHandler, or timeout), and
/// cancels the timeout as soon as one of the other two wins.
///
/// The timeout used to be a bare `asyncAfter` that nobody cancelled: it kept the continuation
/// and the boxed fallback alive for the full timeout regardless of how fast the daemon
/// answered. At a few calls a second that meant a permanent backlog of pending closures —
/// steady CPU on the global queue and a memory footprint that only grew.
private final class OnceBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var done = false
    private var cont: CheckedContinuation<T, Never>?
    private var timeout: DispatchWorkItem?

    init(_ c: CheckedContinuation<T, Never>) { cont = c }

    func setTimeout(_ item: DispatchWorkItem) {
        lock.lock(); defer { lock.unlock() }
        if done { item.cancel() } else { timeout = item }
    }

    func resume(_ value: T) {
        lock.lock()
        guard !done else { lock.unlock(); return }
        done = true
        let c = cont
        let pending = timeout
        cont = nil
        timeout = nil
        lock.unlock()

        pending?.cancel()
        c?.resume(returning: value)
    }
}

/// XPC client for tunhubd + daemon install management (SMAppService).
final class DaemonClient {
    private var connection: NSXPCConnection?
    private let timeoutSec: Double = 20

    private func freshConnection() -> NSXPCConnection {
        if let c = connection { return c }
        let c = NSXPCConnection(machServiceName: kDaemonMachService, options: .privileged)
        c.remoteObjectInterface = NSXPCInterface(with: TunHubDaemonXPC.self)
        c.invalidationHandler = { [weak self] in self?.connection = nil }
        c.interruptionHandler = { [weak self] in self?.connection = nil }
        c.resume()
        connection = c
        return c
    }

    /// Universal safe call: reply | errorHandler | timeout — whichever resolves the continuation.
    private func call<T>(_ fallback: T, _ body: @escaping (TunHubDaemonXPC, OnceBox<T>) -> Void) async -> T {
        await withCheckedContinuation { (cont: CheckedContinuation<T, Never>) in
            let box = OnceBox(cont)
            let c = freshConnection()
            guard let proxy = c.remoteObjectProxyWithErrorHandler({ [weak self] err in
                applog.error("xpc", "daemon connection error: \(err.localizedDescription)")
                self?.connection = nil
                box.resume(fallback)
            }) as? TunHubDaemonXPC else {
                applog.error("xpc", "could not get the daemon proxy")
                box.resume(fallback)
                return
            }
            // Timeout safety net, so the UI never hangs. Cancelled by the box the moment the
            // reply (or an error) arrives.
            let timeout = DispatchWorkItem { box.resume(fallback) }
            box.setTimeout(timeout)
            DispatchQueue.global().asyncAfter(deadline: .now() + timeoutSec, execute: timeout)
            body(proxy, box)
        }
    }

    func startTunnel(_ spec: ResolvedTunnelSpec) async throws {
        let data = try TunJSON.encoder.encode(spec)
        applog.debug("xpc", "→ startTunnel “\(spec.name)” (\(data.count) bytes)")
        let err: String? = await call("no daemon reply (timeout)") { proxy, box in
            proxy.startTunnel(data) { box.resume($0) }
        }
        if let err { throw AppError(err) }
    }

    func stopTunnel(id: UUID) async throws {
        let err: String? = await call("no daemon reply (timeout)") { proxy, box in
            proxy.stopTunnel(id.uuidString) { box.resume($0) }
        }
        if let err { throw AppError(err) }
    }

    func stopAll() async {
        _ = await call("") { proxy, box in
            proxy.stopAll { box.resume("") }
        }
    }

    /// `nil` means the daemon did not reply — which is NOT the same as "no tunnels are
    /// running". Distinguishing the two is what lets the poll loop derive reachability from
    /// this one call instead of following every tick with a separate ping.
    func runtimeStates() async -> [TunnelRuntimeState]? {
        let data: Data? = await call(nil as Data?) { proxy, box in
            proxy.runtimeStates { box.resume($0) }
        }
        guard let data else { return nil }
        return (try? TunJSON.decoder.decode([TunnelRuntimeState].self, from: data)) ?? []
    }

    func setKillSwitchEnabled(_ enabled: Bool) async {
        _ = await call(nil as String?) { proxy, box in
            proxy.setKillSwitchEnabled(enabled) { box.resume($0) }
        }
    }

    func recentLog(maxLines: Int = 1000) async -> [LogLine] {
        let data: Data = await call(Data()) { proxy, box in
            proxy.recentLog(maxLines) { box.resume($0) }
        }
        return (try? TunJSON.decoder.decode([LogLine].self, from: data)) ?? []
    }

    /// Persist the capture mode on the daemon side (it applies it on its next start).
    func setLogMode(_ mode: LogCaptureMode) async -> String? {
        await call(nil as String?) { proxy, box in
            proxy.setLogMode(mode.rawValue) { box.resume($0) }
        }
    }

    func version() async -> String? {
        await call(nil as String?) { proxy, box in
            proxy.daemonVersion { box.resume($0) }
        }
    }

    /// Quick liveness check of the daemon (for the UI).
    func ping() async -> Bool {
        await version() != nil
    }
}

struct AppError: Error, LocalizedError {
    let message: String
    var errorDescription: String? { message }
    init(_ m: String) { self.message = m }
}

// MARK: - Daemon install/uninstall

enum DaemonManager {
    static let label = TunHub.daemonLabel
    static let plistName = "\(TunHub.daemonLabel).plist"

    static var service: SMAppService { SMAppService.daemon(plistName: plistName) }

    static var statusText: String {
        if classicPlistInstalled { return "installed (classic LaunchDaemon)" }
        switch service.status {
        case .notRegistered: return "not installed"
        case .enabled: return "installed and active"
        case .requiresApproval: return "awaiting approval in System Settings → Login Items"
        case .notFound: return "not found (is the app in /Applications?)"
        @unknown default: return "unknown"
        }
    }

    static var isEnabled: Bool { service.status == .enabled }

    static func install() throws {
        try service.register()
    }

    static func uninstall() throws {
        try service.unregister()
    }

    static func openLoginItemsSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }

    /// Whether the classic LaunchDaemon is installed (bypassing SMAppService).
    static var classicPlistInstalled: Bool {
        FileManager.default.fileExists(atPath: TunHub.DaemonPath.plist)
    }

    /// Restart/install the daemon with a system password prompt (a single dialog).
    /// Installs the classic LaunchDaemon from the bundle and kickstarts it.
    @discardableResult
    static func privilegedRestart() -> (ok: Bool, message: String) {
        let appPath = Bundle.main.bundlePath
        let srcPlist = "\(appPath)/Contents/Library/LaunchDaemons/\(TunHub.daemonLabel).system.plist"
        let dstPlist = TunHub.DaemonPath.plist
        // One privileged command: copy the plist, (re)load it, kickstart.
        let shell = """
        set -e
        cp '\(srcPlist)' '\(dstPlist)'
        chown root:wheel '\(dstPlist)'
        chmod 644 '\(dstPlist)'
        launchctl bootout system/\(label) 2>/dev/null || true
        launchctl bootstrap system '\(dstPlist)' 2>/dev/null || true
        launchctl enable system/\(label) 2>/dev/null || true
        launchctl kickstart -k system/\(label)
        """
        let osa = "do shell script \"\(shell.replacingOccurrences(of: "\"", with: "\\\""))\" with administrator privileges"
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        p.arguments = ["-e", osa]
        let err = Pipe()
        p.standardError = err
        do { try p.run() } catch { return (false, "failed to launch osascript: \(error.localizedDescription)") }
        p.waitUntilExit()
        if p.terminationStatus == 0 { return (true, "daemon restarted") }
        let e = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        if e.contains("-128") { return (false, "cancelled by the user") }
        return (false, e.isEmpty ? "code \(p.terminationStatus)" : e)
    }
}
