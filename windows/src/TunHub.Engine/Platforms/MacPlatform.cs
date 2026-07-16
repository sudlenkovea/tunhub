using System.Diagnostics;
using System.Net.Sockets;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.Platforms;

/// <summary>macOS implementation: utun + /sbin/route + pf + networksetup (mirrors the Swift daemon).</summary>
public sealed class MacPlatform : ITunnelPlatform
{
    private readonly FileLog _log;
    private readonly Dictionary<Guid, List<string[]>> _routeJournal = new();
    private readonly Dictionary<Guid, Dictionary<string, string[]>> _dnsBackup = new();

    public MacPlatform(FileLog log) => _log = log;

    public string Name => "macOS";

    public string? LocateCore(string coreName)
    {
        var dir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(dir, coreName),
            Path.Combine(dir, "MacOS", coreName),
            $"/usr/local/bin/{coreName}"
        };
        return candidates.FirstOrDefault(File.Exists);
    }

    public CoreLaunch BuildCoreLaunch(ResolvedTunnelSpec spec, string coreExe, Guid id)
    {
        var nameFile = Path.Combine(PlatformPaths.RunDir, $"{id}.name");
        if (File.Exists(nameFile)) File.Delete(nameFile);
        return new CoreLaunch
        {
            ExePath = coreExe,
            Args = new List<string> { "utun" },
            NameFile = nameFile,
            Environment =
            {
                ["WG_PROCESS_FOREGROUND"] = "1",
                ["WG_TUN_NAME_FILE"] = nameFile,
                ["LOG_LEVEL"] = "verbose",
                ["TUNHUB_OWNER"] = id.ToString()
            }
        };
    }

    public string? WaitForInterface(CoreLaunch launch, Process process, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (launch.NameFile is { } nf && File.Exists(nf))
            {
                var name = File.ReadAllText(nf).Trim();
                if (name.Length > 0) return name;
            }
            Thread.Sleep(100);
        }
        return null;
    }

    public Stream ConnectUapi(string interfaceName, TunnelKind kind, TimeSpan timeout)
    {
        var dir = kind == TunnelKind.AmneziaWg ? "/var/run/amneziawg" : "/var/run/wireguard";
        var sock = $"{dir}/{interfaceName}.sock";
        var deadline = DateTime.UtcNow + timeout;
        while (!File.Exists(sock) && DateTime.UtcNow < deadline) Thread.Sleep(50);

        var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        socket.Connect(new UnixDomainSocketEndPoint(sock));
        return new NetworkStream(socket, ownsSocket: true);
    }

    public void ConfigureInterface(string iface, ResolvedTunnelSpec spec)
    {
        foreach (var a in spec.Addresses)
        {
            var res = a.IsIPv6
                ? Shell.Run("/sbin/ifconfig", iface, "inet6", $"{a.AddressString}/{a.Prefix}", "alias")
                : Shell.Run("/sbin/ifconfig", iface, "inet", $"{a.AddressString}/{a.Prefix}", a.AddressString, "alias");
            if (!res.Ok) throw new Exception($"ifconfig address failed: {res.Stderr}");
        }
        if (spec.Mtu is { } mtu) Shell.Run("/sbin/ifconfig", iface, "mtu", mtu.ToString());
        var up = Shell.Run("/sbin/ifconfig", iface, "up");
        if (!up.Ok) throw new Exception($"ifconfig up failed: {up.Stderr}");
    }

    public void ApplyRoutes(ResolvedTunnelSpec spec, string iface, IReadOnlyDictionary<int, string> endpoints)
    {
        var deletes = new List<string[]>();

        // Pin endpoints via the current path to them (before the /1 routes).
        foreach (var ep in endpoints.Values)
        {
            if (EndpointUtil.Split(ep) is not { } s) continue;
            var host = s.Host;
            var v6 = host.Contains(':');
            var fam = v6 ? "-inet6" : "-inet";
            var gw = CurrentRoute(host, v6);
            var addArgs = new List<string> { "-q", "-n", "add", fam, "-host", host };
            if (gw.Gateway is { } g) addArgs.Add(g);
            else if (gw.Iface is { } i) { addArgs.Add("-interface"); addArgs.Add(i); }
            else continue;
            if (Shell.Run("/sbin/route", addArgs.ToArray()).Ok)
                deletes.Add(new[] { "-q", "-n", "delete", fam, "-host", host });
        }

        // Expand routes; default → /1 pair.
        var expanded = new List<IpAddressRange>();
        foreach (var r in spec.Routes)
        {
            if (r.Prefix == 0)
                expanded.AddRange(r.IsIPv6
                    ? new[] { IpAddressRange.Parse("::/1")!.Value, IpAddressRange.Parse("8000::/1")!.Value }
                    : new[] { IpAddressRange.Parse("0.0.0.0/1")!.Value, IpAddressRange.Parse("128.0.0.0/1")!.Value });
            else expanded.Add(r);
        }
        foreach (var r in expanded)
        {
            var fam = r.IsIPv6 ? "-inet6" : "-inet";
            var res = Shell.Run("/sbin/route", "-q", "-n", "add", fam, r.Canonical, "-interface", iface);
            if (res.Ok) deletes.Add(new[] { "-q", "-n", "delete", fam, r.Canonical, "-interface", iface });
        }

        lock (_routeJournal) _routeJournal[spec.Id] = deletes;
    }

    public void RollbackRoutes(Guid id, string iface)
    {
        List<string[]>? journal;
        lock (_routeJournal) { _routeJournal.TryGetValue(id, out journal); _routeJournal.Remove(id); }
        if (journal is null) return;
        for (var i = journal.Count - 1; i >= 0; i--) Shell.Run("/sbin/route", journal[i]);
    }

    public void ApplyDns(ResolvedTunnelSpec spec, string iface)
    {
        if (spec.DnsServers.Count == 0 || spec.DnsMode.Kind == DnsModeKind.Disabled) return;
        // MVP: global DNS on the primary service. (Split-DNS via SCDynamicStore — TODO.)
        var svc = PrimaryService();
        if (svc is null) return;
        var cur = Shell.Run("/usr/sbin/networksetup", "-getdnsservers", svc);
        var prev = cur.Stdout.Split('\n').Select(s => s.Trim())
            .Where(s => IpAddressRange.Pton(s) is not null).ToArray();
        lock (_dnsBackup) _dnsBackup[spec.Id] = new Dictionary<string, string[]> { [svc] = prev };
        Shell.Run("/usr/sbin/networksetup", new[] { "-setdnsservers", svc }.Concat(spec.DnsServers), null);
    }

    public void RollbackDns(Guid id, string iface)
    {
        Dictionary<string, string[]>? backup;
        lock (_dnsBackup) { _dnsBackup.TryGetValue(id, out backup); _dnsBackup.Remove(id); }
        if (backup is null) return;
        foreach (var (svc, dns) in backup)
        {
            if (dns.Length == 0) Shell.Run("/usr/sbin/networksetup", "-setdnsservers", svc, "Empty");
            else Shell.Run("/usr/sbin/networksetup", new[] { "-setdnsservers", svc }.Concat(dns), null);
        }
    }

    public void RebuildKillSwitch(IReadOnlyList<ActiveTunnel> active, bool enabled)
    {
        // pf-based kill switch. Kept minimal; full parity with the Swift pf anchor is a TODO.
        if (!enabled || active.Count == 0)
        {
            Shell.Run("/sbin/pfctl", "-a", "com.tunhub", "-F", "all");
            return;
        }
        var rules = "set skip on lo0\nblock drop out all\n";
        foreach (var t in active)
        {
            rules += $"pass out on {t.Interface} all\n";
            foreach (var ep in t.Endpoints)
            {
                var dst = ep.Ip.Contains(':') ? $"{{ {ep.Ip} }}" : ep.Ip;
                rules += $"pass out proto udp from any to {dst} port = {ep.Port}\n";
            }
        }
        rules += "pass out proto udp from any to any port { 67, 68, 546, 547 }\n";
        var rulesFile = Path.Combine(PlatformPaths.VarDir, "pf.rules");
        File.WriteAllText(rulesFile, rules);
        Shell.Run("/sbin/pfctl", "-a", "com.tunhub", "-f", rulesFile);
        Shell.Run("/sbin/pfctl", "-E");
    }

    public string? PhysicalDefaultInterface() => CurrentRoute("default", v6: false).Iface;

    private (string? Gateway, string? Iface) CurrentRoute(string dst, bool v6)
    {
        var r = Shell.Run("/sbin/route", "-n", "get", v6 ? "-inet6" : "-inet", dst);
        string? gw = null, ifc = null;
        foreach (var raw in r.Stdout.Split('\n'))
        {
            var t = raw.Trim();
            if (t.StartsWith("gateway:")) gw = t[8..].Trim();
            if (t.StartsWith("interface:")) ifc = t[10..].Trim();
        }
        if (gw is not null && IpAddressRange.Pton(gw) is null) gw = null;
        return (gw, ifc);
    }

    private string? PrimaryService()
    {
        var dev = PhysicalDefaultInterface();
        if (dev is null) return null;
        var r = Shell.Run("/usr/sbin/networksetup", "-listnetworkserviceorder");
        string? lastService = null;
        foreach (var raw in r.Stdout.Split('\n'))
        {
            var line = raw.Trim();
            if (line.StartsWith("(") && line.Contains(") "))
                lastService = line[(line.IndexOf(") ", StringComparison.Ordinal) + 2)..].Trim();
            else if (line.Contains($"Device: {dev}") && lastService is not null)
                return lastService;
        }
        return null;
    }
}
