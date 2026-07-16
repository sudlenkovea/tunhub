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
        var name = launch.KnownInterfaceName;
        if (name is null) return null;
        // Wait for the core's UAPI pipe to appear.
        var deadline = DateTime.UtcNow + timeout;
        var pipe = UapiPipeName(name);
        while (DateTime.UtcNow < deadline)
        {
            if (File.Exists($@"\\.\pipe\{pipe}")) return name;
            if (process.HasExited) return null;
            Thread.Sleep(100);
        }
        return name; // best effort; connection attempt will surface a clear error if wrong
    }

    private static string UapiPipeName(string iface) =>
        $@"ProtectedPrefix\Administrators\WireGuard\{iface}";

    public Stream ConnectUapi(string interfaceName, TunnelKind kind, TimeSpan timeout)
    {
        var client = new NamedPipeClientStream(".", UapiPipeName(interfaceName),
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
        // MVP: set the tunnel adapter's DNS. Split-DNS via NRPT — TODO.
        var first = true;
        foreach (var dns in spec.DnsServers.Where(d => !d.Contains(':')))
        {
            if (first)
            {
                Shell.Run("netsh", "interface", "ipv4", "set", "dnsservers", $"name={iface}", "static", dns, "primary");
                first = false;
            }
            else
            {
                Shell.Run("netsh", "interface", "ipv4", "add", "dnsservers", $"name={iface}", dns, "index=2");
            }
        }
    }

    public void RollbackDns(Guid id, string iface) =>
        Shell.Run("netsh", "interface", "ipv4", "set", "dnsservers", $"name={iface}", "dhcp");

    public void RebuildKillSwitch(IReadOnlyList<ActiveTunnel> active, bool enabled)
    {
        // TODO: implement with the Windows Filtering Platform (WFP), like WireGuard-for-Windows.
        _log.Warn("firewall", "kill switch not yet implemented on Windows (WFP TODO)");
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
