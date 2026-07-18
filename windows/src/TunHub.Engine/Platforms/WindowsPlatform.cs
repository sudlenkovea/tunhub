using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Runtime.Versioning;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.Platforms;

/// <summary>
/// Windows implementation: Wintun (via the Go core) + netsh routes/DNS + named-pipe UAPI.
/// Networking uses netsh/route shell-outs to stay compilable and inspectable; the kill
/// switch (WFP) and endpoint pinning are marked TODO for on-device iteration.
/// </summary>
[SupportedOSPlatform("windows")]
public sealed class WindowsPlatform : ITunnelPlatform
{
    private readonly FileLog _log;
    private readonly Dictionary<Guid, List<string[]>> _routeJournal = new();

    public WindowsPlatform(FileLog log) => _log = log;

    public string Name => "Windows";

    public string? LocateCore(string coreName)
    {
        var dir = AppContext.BaseDirectory;
        var exe = coreName.EndsWith(".exe") ? coreName : coreName + ".exe";
        var candidates = new[] { Path.Combine(dir, exe), Path.Combine(dir, "cores", exe) };
        return candidates.FirstOrDefault(File.Exists);
    }

    public CoreLaunch BuildCoreLaunch(ResolvedTunnelSpec spec, string coreExe, Guid id)
    {
        // Wintun adapter name the core will create; also the UAPI pipe name.
        var ifaceName = "TunHub-" + id.ToString("N")[..8];
        return new CoreLaunch
        {
            ExePath = coreExe,
            Args = new List<string> { ifaceName },
            KnownInterfaceName = ifaceName,
            Environment =
            {
                ["LOG_LEVEL"] = "verbose",
                ["TUNHUB_OWNER"] = id.ToString()
            }
        };
    }

    public string? WaitForInterface(CoreLaunch launch, Process process, TimeSpan timeout)
    {
        // The adapter name is passed to the core up-front and used verbatim; File.Exists on a
        // named pipe is unreliable, so don't poll it here — just confirm the core didn't die
        // immediately. ConnectUapi then waits for the actual UAPI pipe to appear.
        Thread.Sleep(300);
        return process.HasExited ? null : launch.KnownInterfaceName;
    }

    // amneziawg-go exposes UAPI on \\.\pipe\ProtectedPrefix\Administrators\AmneziaWG\<iface>,
    // plain wireguard-go on …\WireGuard\<iface> — the prefix depends on the core.
    private static string UapiPipeName(string iface, TunnelKind kind) =>
        $@"ProtectedPrefix\Administrators\{(kind == TunnelKind.AmneziaWg ? "AmneziaWG" : "WireGuard")}\{iface}";

    public Stream ConnectUapi(string interfaceName, TunnelKind kind, TimeSpan timeout)
    {
        var client = new NamedPipeClientStream(".", UapiPipeName(interfaceName, kind),
            PipeDirection.InOut, PipeOptions.Asynchronous);
        client.Connect((int)timeout.TotalMilliseconds);
        return client;
    }

    public void ConfigureInterface(string iface, ResolvedTunnelSpec spec)
    {
        foreach (var a in spec.Addresses)
        {
            if (a.IsIPv6)
                Shell.Run("netsh", "interface", "ipv6", "set", "address", $"interface={iface}",
                    $"address={a.AddressString}/{a.Prefix}");
            else
                Shell.Run("netsh", "interface", "ipv4", "set", "address", $"name={iface}",
                    "static", a.AddressString, MaskFromPrefix(a.Prefix));
        }
        if (spec.Mtu is { } mtu)
            Shell.Run("netsh", "interface", "ipv4", "set", "subinterface", iface, $"mtu={mtu}", "store=active");
    }

    public void ApplyRoutes(ResolvedTunnelSpec spec, string iface, IReadOnlyDictionary<int, string> endpoints)
    {
        var deletes = new List<string[]>();

        // Pin endpoints via the current physical gateway (avoid the tunnel loop).
        var gw = PhysicalDefaultGateway();
        foreach (var ep in endpoints.Values)
        {
            if (EndpointUtil.Split(ep) is not { } s || s.Host.Contains(':')) continue;
            if (gw is null) break;
            if (Shell.Run("route", "add", s.Host, "mask", "255.255.255.255", gw).Ok)
                deletes.Add(new[] { "delete", s.Host });
        }

        var expanded = new List<IpAddressRange>();
        foreach (var r in spec.Routes)
        {
            if (r.Prefix == 0 && !r.IsIPv6)
                expanded.AddRange(new[] { IpAddressRange.Parse("0.0.0.0/1")!.Value, IpAddressRange.Parse("128.0.0.0/1")!.Value });
            else expanded.Add(r);
        }
        foreach (var r in expanded)
        {
            var proto = r.IsIPv6 ? "ipv6" : "ipv4";
            var res = Shell.Run("netsh", "interface", proto, "add", "route", r.Canonical, $"interface={iface}");
            if (res.Ok) deletes.Add(new[] { "netsh", "interface", proto, "delete", "route", r.Canonical, $"interface={iface}" });
        }

        lock (_routeJournal) _routeJournal[spec.Id] = deletes;
    }

    public void RollbackRoutes(Guid id, string iface)
    {
        List<string[]>? journal;
        lock (_routeJournal) { _routeJournal.TryGetValue(id, out journal); _routeJournal.Remove(id); }
        if (journal is null) return;
        for (var i = journal.Count - 1; i >= 0; i--)
        {
            var cmd = journal[i];
            if (cmd[0] == "delete") Shell.Run("route", cmd);
            else Shell.Run(cmd[0], cmd.Skip(1).ToArray());
        }
    }

    public void ApplyDns(ResolvedTunnelSpec spec, string iface)
    {
        if (spec.DnsServers.Count == 0 || spec.DnsMode.Kind == DnsModeKind.Disabled) return;
        var v4 = spec.DnsServers.Where(d => !d.Contains(':')).ToList();

        if (spec.DnsMode.Kind == DnsModeKind.Split && spec.DnsMode.MatchDomains.Count > 0)
        {
            // Split-DNS: resolve only the match domains through the tunnel via an NRPT rule,
            // so the two tunnels don't fight over the system resolver (mirrors macOS split DNS).
            var servers = string.Join("','", v4);
            foreach (var domain in spec.DnsMode.MatchDomains)
            {
                var ns = NormalizeNrptNamespace(domain);
                PowerShell($"Add-DnsClientNrptRule -Namespace '{ns}' -NameServers @('{servers}') -Comment 'TunHub:{spec.Id}'");
            }
            return;
        }

        // Global: set the tunnel adapter's DNS servers directly.
        var first = true;
        foreach (var dns in v4)
        {
            if (first) { Shell.Run("netsh", "interface", "ipv4", "set", "dnsservers", $"name={iface}", "static", dns, "primary"); first = false; }
            else Shell.Run("netsh", "interface", "ipv4", "add", "dnsservers", $"name={iface}", dns, "index=2");
        }
    }

    public void RollbackDns(Guid id, string iface)
    {
        Shell.Run("netsh", "interface", "ipv4", "set", "dnsservers", $"name={iface}", "dhcp");
        // Remove any NRPT rules this tunnel added (matched by the comment tag).
        PowerShell($"Get-DnsClientNrptRule | Where-Object {{ $_.Comment -eq 'TunHub:{id}' }} | Remove-DnsClientNrptRule -Force -ErrorAction SilentlyContinue");
    }

    private static string NormalizeNrptNamespace(string domain)
    {
        var d = domain.Trim();
        return d.StartsWith('.') ? d : "." + d;   // NRPT wants a leading dot for suffix matches
    }

    private static void PowerShell(string script) =>
        Shell.Run("powershell", "-NoProfile", "-NonInteractive", "-Command", script);

    // MARK: - Kill switch (Windows Firewall)

    // Model: while any kill-switch tunnel is up, the default outbound action is "block", and we
    // allow only (a) the VPN core processes (so handshakes/tunnel traffic reach the physical NIC),
    // (b) the pinned server endpoints, (c) loopback and DHCP. If the tunnel drops, the core stops
    // forwarding and every other app is blocked from the internet — a fail-closed kill switch.
    private const string FwGroup = "TunHub-KillSwitch";
    private bool _killApplied;

    public void RebuildKillSwitch(IReadOnlyList<ActiveTunnel> active, bool enabled)
    {
        try
        {
            // Always clear our previous rules first (idempotent rebuild).
            Shell.Run("netsh", "advfirewall", "firewall", "delete", "rule", $"group={FwGroup}");

            if (!enabled || active.Count == 0)
            {
                if (_killApplied)
                {
                    Shell.Run("netsh", "advfirewall", "set", "allprofiles", "firewallpolicy", "blockinbound,allowoutbound");
                    _killApplied = false;
                    _log.Info("firewall", "kill switch disabled (outbound restored)");
                }
                return;
            }

            // Allow the VPN cores to reach the internet directly.
            foreach (var core in new[] { TunHubInfo.Core.WireGuard, TunHubInfo.Core.AmneziaWg, TunHubInfo.Core.OpenVpn })
            {
                var exe = LocateCore(core);
                if (exe is null) continue;
                Shell.Run("netsh", "advfirewall", "firewall", "add", "rule", $"name={FwGroup}-core",
                    $"group={FwGroup}", "dir=out", "action=allow", $"program={exe}", "enable=yes");
            }
            // Allow the pinned server endpoints.
            foreach (var ep in active.SelectMany(a => a.Endpoints))
            {
                if (ep.Ip.Contains(':')) continue; // IPv4 endpoints only for now
                Shell.Run("netsh", "advfirewall", "firewall", "add", "rule", $"name={FwGroup}-ep",
                    $"group={FwGroup}", "dir=out", "action=allow", $"remoteip={ep.Ip}", "enable=yes");
            }
            // Loopback + DHCP so the machine keeps basic connectivity.
            Shell.Run("netsh", "advfirewall", "firewall", "add", "rule", $"name={FwGroup}-loopback",
                $"group={FwGroup}", "dir=out", "action=allow", "remoteip=127.0.0.0/8,::1", "enable=yes");
            Shell.Run("netsh", "advfirewall", "firewall", "add", "rule", $"name={FwGroup}-dhcp",
                $"group={FwGroup}", "dir=out", "action=allow", "protocol=UDP", "localport=68", "remoteport=67", "enable=yes");

            Shell.Run("netsh", "advfirewall", "set", "allprofiles", "firewallpolicy", "blockinbound,blockoutbound");
            _killApplied = true;
            _log.Info("firewall", $"kill switch active ({active.Count} tunnel(s), fail-closed)");
        }
        catch (Exception ex) { _log.Error("firewall", ex.Message); }
    }

    public string? PhysicalDefaultInterface() => null; // not needed for netsh route pinning

    private string? PhysicalDefaultGateway()
    {
        // Parse `route print 0.0.0.0` for the active default gateway.
        var r = Shell.Run("route", "print", "0.0.0.0");
        foreach (var raw in r.Stdout.Split('\n'))
        {
            var parts = raw.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            if (parts.Length >= 3 && parts[0] == "0.0.0.0" && parts[1] == "0.0.0.0"
                && IPAddress.TryParse(parts[2], out _))
                return parts[2];
        }
        return null;
    }

    private static string MaskFromPrefix(int prefix)
    {
        uint mask = prefix == 0 ? 0 : 0xFFFFFFFF << (32 - prefix);
        return $"{(mask >> 24) & 0xFF}.{(mask >> 16) & 0xFF}.{(mask >> 8) & 0xFF}.{mask & 0xFF}";
    }
}
