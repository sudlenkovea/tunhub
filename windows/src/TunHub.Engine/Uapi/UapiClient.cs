using System.Net;
using System.Text;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.Uapi;

/// <summary>wireguard-go / amneziawg-go userspace API (set=1 / get=1) over a byte stream.</summary>
public static class UapiClient
{
    public static void Set(Stream stream, string config)
    {
        var writer = new StreamWriter(stream, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
        writer.Write(config);
        // Read response until the terminating blank line; expect errno=0.
        var reader = new StreamReader(stream, Encoding.UTF8);
        string? line;
        var errno = 0;
        while ((line = reader.ReadLine()) is not null)
        {
            if (line.Length == 0) break;
            if (line.StartsWith("errno=")) int.TryParse(line[6..], out errno);
        }
        if (errno != 0) throw new Exception($"UAPI set errno={errno}");
    }

    public static List<PeerRuntime> Get(Stream stream)
    {
        var writer = new StreamWriter(stream, new UTF8Encoding(false)) { NewLine = "\n", AutoFlush = true };
        writer.Write("get=1\n\n");
        var reader = new StreamReader(stream, Encoding.UTF8);

        var peers = new List<PeerRuntime>();
        PeerRuntime? current = null;
        string? line;
        while ((line = reader.ReadLine()) is not null)
        {
            if (line.Length == 0) break;
            var eq = line.IndexOf('=');
            if (eq < 0) continue;
            var key = line[..eq];
            var value = line[(eq + 1)..];
            switch (key)
            {
                case "public_key":
                    if (current is not null) peers.Add(current);
                    current = new PeerRuntime { PublicKey = HexToBase64(value) ?? value };
                    break;
                case "endpoint": if (current is not null) current.Endpoint = value; break;
                case "last_handshake_time_sec":
                    if (current is not null && long.TryParse(value, out var sec) && sec > 0)
                        current.LastHandshake = DateTimeOffset.FromUnixTimeSeconds(sec);
                    break;
                case "rx_bytes":
                    if (current is not null && ulong.TryParse(value, out var rx)) current.RxBytes = rx;
                    break;
                case "tx_bytes":
                    if (current is not null && ulong.TryParse(value, out var tx)) current.TxBytes = tx;
                    break;
            }
        }
        if (current is not null) peers.Add(current);
        return peers;
    }

    private static string? HexToBase64(string hex)
    {
        try { return Convert.ToBase64String(Convert.FromHexString(hex)); }
        catch { return null; }
    }
}

/// <summary>Renders a ResolvedTunnelSpec into a UAPI set=1 string and resolves endpoints.</summary>
public static class ConfigRenderer
{
    public static string UapiSet(ResolvedTunnelSpec spec, IReadOnlyDictionary<int, string> endpoints)
    {
        var pk = WgKey.Base64ToHex(spec.PrivateKey) ?? throw new Exception("bad private key");
        var sb = new StringBuilder();
        sb.Append("set=1\n");
        sb.Append($"private_key={pk}\n");
        if (spec.ListenPort is { } lp) sb.Append($"listen_port={lp}\n");

        if (spec.Kind == TunnelKind.AmneziaWg && spec.Awg is { IsEmpty: false } a)
        {
            void Put(string k, object? v) { if (v is not null) sb.Append($"{k}={v}\n"); }
            Put("jc", a.Jc); Put("jmin", a.Jmin); Put("jmax", a.Jmax);
            Put("s1", a.S1); Put("s2", a.S2); Put("s3", a.S3); Put("s4", a.S4);
            Put("h1", a.H1); Put("h2", a.H2); Put("h3", a.H3); Put("h4", a.H4);
            Put("i1", a.I1); Put("i2", a.I2); Put("i3", a.I3); Put("i4", a.I4); Put("i5", a.I5);
            Put("itime", a.ITime);
        }

        sb.Append("replace_peers=true\n");
        for (var i = 0; i < spec.Peers.Count; i++)
        {
            var p = spec.Peers[i];
            var pub = WgKey.Base64ToHex(p.PublicKey) ?? throw new Exception("bad peer public key");
            sb.Append($"public_key={pub}\n");
            if (p.PresharedKey is { } psk && WgKey.Base64ToHex(psk) is { } pskHex)
                sb.Append($"preshared_key={pskHex}\n");
            if (endpoints.TryGetValue(i, out var ep)) sb.Append($"endpoint={ep}\n");
            sb.Append("replace_allowed_ips=true\n");
            foreach (var r in p.AllowedIPs) sb.Append($"allowed_ip={r.Canonical}\n");
            if (p.Keepalive is { } ka && ka > 0) sb.Append($"persistent_keepalive_interval={ka}\n");
        }
        sb.Append('\n');
        return sb.ToString();
    }

    /// <summary>Resolve every peer endpoint to "ip:port" (or "[v6]:port"). Throws on total failure.</summary>
    public static Dictionary<int, string> ResolveEndpoints(ResolvedTunnelSpec spec)
    {
        var outMap = new Dictionary<int, string>();
        for (var i = 0; i < spec.Peers.Count; i++)
        {
            var p = spec.Peers[i];
            if (p.Endpoint is null || EndpointUtil.Split(p.Endpoint) is not { } split) continue;
            var (host, port) = split;
            string ip;
            if (EndpointUtil.IsIpLiteral(host))
            {
                ip = host;
            }
            else
            {
                var addrs = Dns.GetHostAddresses(host);
                var v4 = addrs.FirstOrDefault(a => a.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork);
                var chosen = v4 ?? addrs.FirstOrDefault()
                    ?? throw new Exception($"could not resolve endpoint {host}");
                ip = chosen.ToString();
            }
            outMap[i] = ip.Contains(':') ? $"[{ip}]:{port}" : $"{ip}:{port}";
        }
        return outMap;
    }
}
