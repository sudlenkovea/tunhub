import Foundation

public let kDaemonMachService = TunHub.machService
public let kDaemonProtocolVersion = "0.8.2"

/// Team ID for the XPC code-signing requirement. Empty = check disabled (dev/ad-hoc).
public let kTeamID = TunHub.teamID

/// Daemon XPC interface. DTOs travel as JSON `Data` (TunJSON) to avoid NSSecureCoding.
@objc public protocol TunHubDaemonXPC {
    /// spec: ResolvedTunnelSpec (JSON). reply(nil) = success, otherwise an error string.
    func startTunnel(_ spec: Data, reply: @escaping (String?) -> Void)
    func stopTunnel(_ id: String, reply: @escaping (String?) -> Void)
    func stopAll(_ reply: @escaping () -> Void)
    /// reply: [TunnelRuntimeState] (JSON)
    func runtimeStates(_ reply: @escaping (Data) -> Void)
    func setKillSwitchEnabled(_ enabled: Bool, reply: @escaping (String?) -> Void)
    func daemonVersion(_ reply: @escaping (String) -> Void)
    /// Last N lines of the daemon log (JSON [LogLine]).
    func recentLog(_ maxLines: Int, reply: @escaping (Data) -> Void)
    /// Persist the log capture mode (LogCaptureMode raw value) for the daemon side.
    /// Applied on the daemon's next start; if it is idle it restarts itself right away.
    func setLogMode(_ mode: String, reply: @escaping (String?) -> Void)
}

public func codesignRequirementString(teamID: String) -> String {
    "anchor apple generic and certificate leaf[subject.OU] = \"\(teamID)\""
}
