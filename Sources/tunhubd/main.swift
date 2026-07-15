import Foundation
import TunHubShared

// MARK: - XPC service

final class DaemonService: NSObject, TunHubDaemonXPC {
    func startTunnel(_ spec: Data, reply: @escaping (String?) -> Void) {
        do {
            let s = try TunJSON.decoder.decode(ResolvedTunnelSpec.self, from: spec)
            try TunnelSupervisor.shared.start(spec: s)
            reply(nil)
        } catch {
            reply(error.localizedDescription)
        }
    }

    func stopTunnel(_ id: String, reply: @escaping (String?) -> Void) {
        guard let uuid = UUID(uuidString: id) else { reply("bad uuid"); return }
        do {
            try TunnelSupervisor.shared.stop(id: uuid)
            reply(nil)
        } catch {
            reply(error.localizedDescription)
        }
    }

    func stopAll(_ reply: @escaping () -> Void) {
        TunnelSupervisor.shared.stopAll()
        reply()
    }

    func runtimeStates(_ reply: @escaping (Data) -> Void) {
        let states = TunnelSupervisor.shared.states()
        reply((try? TunJSON.encoder.encode(states)) ?? Data())
    }

    func setKillSwitchEnabled(_ enabled: Bool, reply: @escaping (String?) -> Void) {
        TunnelSupervisor.shared.setKillSwitchEnabled(enabled)
        reply(nil)
    }

    func daemonVersion(_ reply: @escaping (String) -> Void) {
        reply(kDaemonFullVersion)
    }

    func recentLog(_ maxLines: Int, reply: @escaping (Data) -> Void) {
        let lines = flog.tail(maxLines: min(max(maxLines, 1), 5000))
        reply((try? TunJSON.encoder.encode(lines)) ?? Data())
    }
}

final class ListenerDelegate: NSObject, NSXPCListenerDelegate {
    func listener(_ listener: NSXPCListener,
                  shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        if !kTeamID.isEmpty {
            connection.setCodeSigningRequirement(codesignRequirementString(teamID: kTeamID))
        } else {
            dlog.warning("XPC codesign check DISABLED (dev build, kTeamID empty)")
        }
        connection.exportedInterface = NSXPCInterface(with: TunHubDaemonXPC.self)
        connection.exportedObject = DaemonService()
        connection.resume()
        return true
    }
}

// MARK: - main

guard geteuid() == 0 else {
    FileHandle.standardError.write("tunhubd must run as root (via SMAppService LaunchDaemon)\n".data(using: .utf8)!)
    exit(1)
}

Paths.ensure()
dlog.info("tunhubd \(kDaemonProtocolVersion, privacy: .public) starting")
flog.info("daemon", "═══ tunhubd \(kDaemonProtocolVersion) started (pid=\(getpid()), uid=\(geteuid())) ═══")
flog.info("daemon", "log: \(flog.filePath)")

// Recovery after a crash/reboot
flog.debug("daemon", "crash recovery: cleaning up orphaned processes/DNS/pf…")
TunnelSupervisor.shared.crashRecovery()

// Graceful shutdown: tear down routes/DNS/pf on SIGTERM
signal(SIGTERM, SIG_IGN)
let sigterm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
sigterm.setEventHandler {
    dlog.info("SIGTERM: stopping all tunnels")
    flog.info("daemon", "SIGTERM — stopping all tunnels and exiting")
    TunnelSupervisor.shared.stopAll()
    exit(0)
}
sigterm.resume()

TunnelSupervisor.shared.startStatsLoop()

let delegate = ListenerDelegate()
let listener = NSXPCListener(machServiceName: kDaemonMachService)
listener.delegate = delegate
listener.resume()

RunLoop.main.run()
