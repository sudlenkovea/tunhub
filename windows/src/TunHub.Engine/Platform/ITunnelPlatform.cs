using System.Diagnostics;
using TunHub.Core;

namespace TunHub.Engine.Platform;

/// <summary>How to launch a core process for one tunnel (platform-specific args/env).</summary>
public sealed class CoreLaunch
{
    public required string ExePath { get; init; }
    public required List<string> Args { get; init; }
    public Dictionary<string, string> Environment { get; init; } = new();
    /// <summary>File the core writes its created interface name into (macOS WG_TUN_NAME_FILE style).</summary>
    public string? NameFile { get; init; }
    /// <summary>If the interface name is known up-front (Windows adapter name), it's set here.</summary>
    public string? KnownInterfaceName { get; init; }
}

public sealed record ActiveTunnel(string Interface, IReadOnlyList<(string Ip, ushort Port)> Endpoints);

/// <summary>
/// Everything OS-specific: launching the core, talking UAPI, and applying/removing
/// routes, DNS and the kill switch. Implemented per platform (macOS / Windows).
/// </summary>
public interface ITunnelPlatform
{
    string Name { get; }

    /// <summary>Locate a bundled core binary (wireguard-go / amneziawg-go) next to the helper.</summary>
    string? LocateCore(string coreName);

    CoreLaunch BuildCoreLaunch(ResolvedTunnelSpec spec, string coreExe, Guid id);

    /// <summary>Resolve the interface name the core created (reads NameFile or returns the known one).</summary>
    string? WaitForInterface(CoreLaunch launch, Process process, TimeSpan timeout);

    /// <summary>Open a connected stream to the core's UAPI endpoint (unix socket / named pipe).</summary>
    Stream ConnectUapi(string interfaceName, TunnelKind kind, TimeSpan timeout);

    void ConfigureInterface(string iface, ResolvedTunnelSpec spec);

    void ApplyRoutes(ResolvedTunnelSpec spec, string iface, IReadOnlyDictionary<int, string> endpoints);
    void RollbackRoutes(Guid id, string iface);

    void ApplyDns(ResolvedTunnelSpec spec, string iface);
    void RollbackDns(Guid id, string iface);

    void RebuildKillSwitch(IReadOnlyList<ActiveTunnel> active, bool enabled);

    /// <summary>Physical default-route interface (for pinning endpoints / DNS binding).</summary>
    string? PhysicalDefaultInterface();
}

/// <summary>
/// Secret storage held by the *app* (user scope): DPAPI CurrentUser on Windows, the
/// user login Keychain on macOS. The privileged helper never persists secrets — the app
/// resolves them and sends a full spec over the local IPC socket. One blob per tunnel.
/// </summary>
public interface ISecretStore
{
    void Save(Guid tunnelId, TunnelSecrets secrets);
    TunnelSecrets? Load(Guid tunnelId);
    void Delete(Guid tunnelId);
}

public sealed class TunnelSecrets
{
    public string PrivateKey { get; set; } = "";
    public Dictionary<string, string> Psks { get; set; } = new();  // peerId → PSK
}
