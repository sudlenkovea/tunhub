namespace TunHub.Core;

/// <summary>
/// Single source of truth for app identity and core binary names, shared by the
/// app, engine and privileged helper. Platform-specific paths live in the helper.
/// </summary>
public static class TunHubInfo
{
    public const string AppName = "TunHub";
    public const string AppId = "com.tunhub.app";

    /// <summary>Daemon / Windows-service identifier.</summary>
    public const string ServiceLabel = "com.tunhub.daemon";

    /// <summary>IPC endpoint name (named pipe on Windows, unix socket on macOS).</summary>
    public const string IpcEndpoint = "tunhub.daemon.ipc";

    /// <summary>Wire protocol version between UI and helper — both parts must match.</summary>
    public const string ProtocolVersion = "0.8.0";

    /// <summary>Core binaries bundled next to the helper (a single AWG core covers 1.5 &amp; 2.0).</summary>
    public static class Core
    {
        public const string WireGuard = "wireguard-go.exe";
        public const string AmneziaWg = "amneziawg-go.exe";
        public const string OpenVpn   = "openvpn.exe";
    }
}
