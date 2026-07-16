using TunHub.Core;

namespace TunHub.Engine.Ipc;

/// <summary>IPC method names (UI ↔ privileged helper).</summary>
public static class IpcMethod
{
    public const string StartTunnel = "startTunnel";
    public const string StopTunnel = "stopTunnel";
    public const string StopAll = "stopAll";
    public const string RuntimeStates = "runtimeStates";
    public const string SetKillSwitch = "setKillSwitch";
    public const string Version = "version";
    public const string RecentLog = "recentLog";
}

/// <summary>Envelope: one JSON line per request/response over the socket.</summary>
public sealed class IpcRequest
{
    public string Method { get; set; } = "";
    /// <summary>Method-specific payload as raw JSON (already serialized).</summary>
    public string? Payload { get; set; }
}

public sealed class IpcResponse
{
    public bool Ok { get; set; }
    public string? Error { get; set; }
    public string? Payload { get; set; }

    public static IpcResponse Success(string? payload = null) => new() { Ok = true, Payload = payload };
    public static IpcResponse Fail(string error) => new() { Ok = false, Error = error };
}

// Payload DTOs

public sealed class SetKillSwitchPayload { public bool Enabled { get; set; } }
public sealed class StopTunnelPayload { public Guid Id { get; set; } }
public sealed class RecentLogPayload { public int MaxLines { get; set; } = 500; }

public sealed class LogLine
{
    public DateTimeOffset Time { get; set; }
    public string Level { get; set; } = "";
    public string Category { get; set; } = "";
    public string Message { get; set; } = "";
}
