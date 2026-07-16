namespace TunHub.Core;

public enum TunnelKind
{
    WireGuard,
    AmneziaWg,
    OpenVpn
}

public static class TunnelKindExtensions
{
    public static string Label(this TunnelKind kind) => kind switch
    {
        TunnelKind.WireGuard => "WireGuard",
        TunnelKind.AmneziaWg => "AmneziaWG",
        TunnelKind.OpenVpn   => "OpenVPN",
        _ => kind.ToString()
    };

    /// <summary>Core binary for this kind. A single AWG v0.2.x core covers 1.5 &amp; 2.0.</summary>
    public static string CoreBinary(this TunnelKind kind) => kind switch
    {
        TunnelKind.WireGuard => TunHubInfo.Core.WireGuard,
        TunnelKind.AmneziaWg => TunHubInfo.Core.AmneziaWg,
        TunnelKind.OpenVpn   => TunHubInfo.Core.OpenVpn,
        _ => TunHubInfo.Core.WireGuard
    };

    /// <summary>WireGuard-family tunnels share the userspace-core + UAPI machinery; OpenVPN does not.</summary>
    public static bool IsWireGuardFamily(this TunnelKind kind) =>
        kind is TunnelKind.WireGuard or TunnelKind.AmneziaWg;
}

/// <summary>Reference to a secret held in the OS secret store (never on disk).</summary>
public sealed class SecretRef
{
    public string Account { get; set; } = "";
    public SecretRef() { }
    public SecretRef(string account) => Account = account;
}

// MARK: - AmneziaWG obfuscation

public sealed class AwgParams
{
    public int? Jc { get; set; }
    public int? Jmin { get; set; }
    public int? Jmax { get; set; }
    public int? S1 { get; set; }
    public int? S2 { get; set; }
    public int? S3 { get; set; }   // cookie-reply junk (AmneziaWG 2.x) — the official client sends it
    public int? S4 { get; set; }   // transport junk — critical: without it the server rejects our transport
    public uint? H1 { get; set; }
    public uint? H2 { get; set; }
    public uint? H3 { get; set; }
    public uint? H4 { get; set; }
    public string? I1 { get; set; }
    public string? I2 { get; set; }
    public string? I3 { get; set; }
    public string? I4 { get; set; }
    public string? I5 { get; set; }
    public int? ITime { get; set; }

    public bool IsEmpty =>
        Jc is null && Jmin is null && Jmax is null && S1 is null && S2 is null &&
        S3 is null && S4 is null &&
        H1 is null && H2 is null && H3 is null && H4 is null &&
        I1 is null && I2 is null && I3 is null && I4 is null && I5 is null && ITime is null;

    public List<string> Validate()
    {
        var e = new List<string>();
        if (Jc is { } jc && (jc < 0 || jc > 128)) e.Add("Jc must be 0…128");
        if (Jmin is { } a && Jmax is { } b && a > b) e.Add("Jmin > Jmax");
        if (Jmin is { } jmin && (jmin < 0 || jmin > 1280)) e.Add("Jmin must be 0…1280");
        if (Jmax is { } jmax && (jmax < 0 || jmax > 1280)) e.Add("Jmax must be 0…1280");
        if (S1 is { } s1 && (s1 < 0 || s1 > 1132)) e.Add("S1 out of range (0…1132)");
        if (S2 is { } s2 && (s2 < 0 || s2 > 1188)) e.Add("S2 out of range (0…1188)");
        var hs = new[] { H1, H2, H3, H4 }.Where(h => h is not null).Select(h => h!.Value).ToList();
        if (hs.Count > 0)
        {
            if (hs.Count != 4) e.Add("H1–H4 must all be set together");
            if (hs.Distinct().Count() != hs.Count) e.Add("H1–H4 must be pairwise distinct");
        }
        return e;
    }

    /// <summary>"Amnezia default" preset (safe: junk without changing headers).</summary>
    public static AwgParams AmneziaDefault()
    {
        var rng = Random.Shared;
        return new AwgParams { Jc = rng.Next(3, 11), Jmin = 50, Jmax = 1000, S1 = 0, S2 = 0 };
    }

    public static AwgParams FullObfuscation()
    {
        var rng = Random.Shared;
        var p = new AwgParams
        {
            Jc = rng.Next(4, 13),
            Jmin = 40,
            Jmax = 70,
            S1 = rng.Next(15, 151),
            S2 = rng.Next(15, 151)
        };
        var hs = new HashSet<uint>();
        while (hs.Count < 4) hs.Add((uint)rng.Next(5, int.MaxValue));
        var arr = hs.ToArray();
        p.H1 = arr[0]; p.H2 = arr[1]; p.H3 = arr[2]; p.H4 = arr[3];
        return p;
    }
}

// MARK: - Config

public sealed class InterfaceConfig
{
    public SecretRef? PrivateKeyRef { get; set; }
    public string PublicKey { get; set; } = "";          // derived, cached for the UI
    public List<IpAddressRange> Addresses { get; set; } = new();
    public ushort? ListenPort { get; set; }
    public List<string> Dns { get; set; } = new();       // resolver IP addresses
    public List<string> DnsSearchDomains { get; set; } = new();
    public int? Mtu { get; set; }
    // Scripts are parsed and stored, but NEVER executed (security).
    public List<string> PreUp { get; set; } = new();
    public List<string> PostUp { get; set; } = new();
    public List<string> PreDown { get; set; } = new();
    public List<string> PostDown { get; set; } = new();
}

public sealed class PeerConfig
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string PublicKey { get; set; } = "";
    public SecretRef? PresharedKeyRef { get; set; }
    public string? Endpoint { get; set; }                // "host:port"
    public List<IpAddressRange> AllowedIPs { get; set; } = new();
    public ushort? PersistentKeepalive { get; set; }
}

public enum DnsModeKind { Global, Split, Disabled }

public sealed class DnsMode
{
    public DnsModeKind Kind { get; set; } = DnsModeKind.Global;
    public List<string> MatchDomains { get; set; } = new();

    public static DnsMode Global() => new() { Kind = DnsModeKind.Global };
    public static DnsMode Split(IEnumerable<string> domains) =>
        new() { Kind = DnsModeKind.Split, MatchDomains = domains.ToList() };
    public static DnsMode Disabled() => new() { Kind = DnsModeKind.Disabled };
}

public enum RouteModeKind { FromAllowedIPs, Custom }

public sealed class RouteMode
{
    public RouteModeKind Kind { get; set; } = RouteModeKind.FromAllowedIPs;
    public List<IpAddressRange> Custom { get; set; } = new();
}

public enum HealthAction { Notify, Restart, Failover }

public sealed class HealthCheckConfig
{
    public string Host { get; set; } = "";               // probe target (inside the tunnel)
    public ushort Port { get; set; } = 443;
    public int IntervalSec { get; set; } = 30;
    public int FailureThreshold { get; set; } = 3;
    public HealthAction Action { get; set; } = HealthAction.Notify;
}

public sealed class TunnelOptions
{
    public DnsMode DnsMode { get; set; } = DnsMode.Global();
    public RouteMode RouteMode { get; set; } = new();
    public bool AutoConnectOnLaunch { get; set; }
    public bool KillSwitch { get; set; }
    public HealthCheckConfig? HealthCheck { get; set; }
    public string? FailoverGroup { get; set; }
    public int FailoverPriority { get; set; }
}

public sealed class TunnelMeta
{
    public DateTimeOffset CreatedAt { get; set; } = DateTimeOffset.Now;
    public DateTimeOffset? LastConnectedAt { get; set; }
    public string? Group { get; set; }
    public string Notes { get; set; } = "";
    public int SortOrder { get; set; }
}

public sealed class TunnelConfig
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public string Name { get; set; } = "";
    public TunnelKind Kind { get; set; } = TunnelKind.WireGuard;
    public InterfaceConfig Interface { get; set; } = new();
    public List<PeerConfig> Peers { get; set; } = new();
    public AwgParams? Awg { get; set; }
    public OpenVpnProfile? OpenVpn { get; set; }
    public TunnelOptions Options { get; set; } = new();
    public TunnelMeta Meta { get; set; } = new();
    public int SchemaVersion { get; set; } = 1;

    /// <summary>The routes that will actually be applied.</summary>
    public List<IpAddressRange> EffectiveRoutes()
    {
        if (Options.RouteMode.Kind == RouteModeKind.Custom)
            return Options.RouteMode.Custom.ToList();

        var seen = new HashSet<string>();
        var outRoutes = new List<IpAddressRange>();
        foreach (var p in Peers)
            foreach (var r in p.AllowedIPs)
                if (seen.Add(r.Canonical))
                    outRoutes.Add(r);
        return outRoutes;
    }

    public bool HasDefaultRoute => EffectiveRoutes().Any(r => r.Prefix <= 1);

    /// <summary>
    /// Effective DNS mode. A split tunnel (no default route) does NOT capture the system
    /// DNS globally — otherwise two such tunnels would fight over the system resolver.
    /// </summary>
    public DnsMode EffectiveDnsMode()
    {
        if (Options.DnsMode.Kind == DnsModeKind.Global)
            return HasDefaultRoute ? DnsMode.Global() : DnsMode.Disabled();
        return Options.DnsMode;
    }
}

// MARK: - Resolved spec (app → helper, carries secrets, lives only in memory)

public sealed class ResolvedPeer
{
    public string PublicKey { get; set; } = "";
    public string? PresharedKey { get; set; }
    public string? Endpoint { get; set; }
    public List<IpAddressRange> AllowedIPs { get; set; } = new();
    public ushort? Keepalive { get; set; }
}

public sealed class ResolvedTunnelSpec
{
    public Guid Id { get; set; }
    public string Name { get; set; } = "";
    public TunnelKind Kind { get; set; }
    public string PrivateKey { get; set; } = "";          // base64
    public List<IpAddressRange> Addresses { get; set; } = new();
    public ushort? ListenPort { get; set; }
    public int? Mtu { get; set; }
    public List<string> DnsServers { get; set; } = new();
    public List<string> DnsSearchDomains { get; set; } = new();
    public DnsMode DnsMode { get; set; } = DnsMode.Global();
    public List<IpAddressRange> Routes { get; set; } = new();
    public AwgParams? Awg { get; set; }
    public bool KillSwitch { get; set; }
    public List<ResolvedPeer> Peers { get; set; } = new();
    /// <summary>Present only for OpenVPN tunnels (config text + credentials + OTP).</summary>
    public ResolvedOpenVpn? OpenVpn { get; set; }
}

// MARK: - Runtime state (helper → app)

public enum TunnelPhase { Stopped, Starting, Up, Degraded, Failed, Stopping }

public sealed class PeerRuntime
{
    public string PublicKey { get; set; } = "";
    public string? Endpoint { get; set; }
    public DateTimeOffset? LastHandshake { get; set; }
    public ulong RxBytes { get; set; }
    public ulong TxBytes { get; set; }
}

public sealed class TunnelRuntimeState
{
    public Guid Id { get; set; }
    public string Name { get; set; } = "";
    public TunnelPhase Phase { get; set; }
    public string? UtunName { get; set; }
    public string? ErrorMessage { get; set; }
    public List<PeerRuntime> Peers { get; set; } = new();
    public DateTimeOffset? Since { get; set; }
    /// <summary>Effective routes actually in force (e.g. server-pushed routes for OpenVPN).</summary>
    public List<string>? Routes { get; set; }

    public ulong RxTotal => Peers.Aggregate(0UL, (a, p) => a + p.RxBytes);
    public ulong TxTotal => Peers.Aggregate(0UL, (a, p) => a + p.TxBytes);

    public DateTimeOffset? LastHandshake
    {
        get
        {
            DateTimeOffset? max = null;
            foreach (var p in Peers)
                if (p.LastHandshake is { } h && (max is null || h > max.Value)) max = h;
            return max;
        }
    }

    public bool HandshakeFresh =>
        LastHandshake is { } h && (DateTimeOffset.Now - h).TotalSeconds < 185;
}

// MARK: - Formatting helpers

public static class ByteFormat
{
    private static readonly string[] Units = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };

    public static string Human(ulong v)
    {
        double value = v;
        var i = 0;
        while (value >= 1024 && i < Units.Length - 1) { value /= 1024; i++; }
        return i == 0 ? $"{v} {Units[i]}" : $"{value:0.0} {Units[i]}";
    }

    public static string Rate(double bytesPerSec) => Human((ulong)Math.Max(0, bytesPerSec)) + "/s";
}
