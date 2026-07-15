import Foundation
import ServiceManagement
import TunHubShared

/// Guarantees a single resume of the continuation (from reply, errorHandler, or timeout).
private final class OnceBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var done = false
    private var cont: CheckedContinuation<T, Never>?
    init(_ c: CheckedContinuation<T, Never>) { cont = c }
    func resume(_ value: T) {
        lock.lock(); defer { lock.unlock() }
        guard !done else { return }
        done = true
        cont?.resume(returning: value)
        cont = nil
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
            // timeout safety net, so the UI never hangs
            DispatchQueue.global().asyncAfter(deadline: .now() + timeoutSec) {
                box.resume(fallback)
            }
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

    func runtimeStates() async -> [TunnelRuntimeState] {
        let data: Data = await call(Data()) { proxy, box in
            proxy.runtimeStates { box.resume($0) }
        }
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
