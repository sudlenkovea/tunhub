using System.Text;

namespace TunHub.Core;

/// <summary>Parse result: config without secrets + secrets separately (for the secret store).</summary>
public sealed class ParsedTunnel
{
    public required TunnelConfig Config { get; init; }
    public required string PrivateKey { get; init; }
    public Dictionary<Guid, string> PresharedKeys { get; init; } = new();  // peer.Id → PSK
    public List<string> Warnings { get; init; } = new();
}

public sealed class ParseException : Exception
{
    public int Line { get; }
    public ParseException(int line, string message) : base($"line {line}: {message}") => Line = line;
}

/// <summary>
/// wg-quick .conf parser/serializer with AmneziaWG extensions
/// (Jc, Jmin, Jmax, S1, S2, S3, S4, H1–H4, I1–I5, ITime).
/// </summary>
public static class WgQuickParser
{
    private enum Section { None, Interface, Peer }

    public static ParsedTunnel Parse(string name, string text)
    {
        var section = Section.None;
        var cfg = new TunnelConfig { Name = name };
        var awg = new AwgParams();
        string? privateKey = null;
        PeerConfig? currentPeer = null;
        string? currentPsk = null;
        var peerPsks = new Dictionary<Guid, string>();
        var warnings = new List<string>();

        void FlushPeer(int line)
        {
            if (currentPeer is null) return;
            if (!WgKey.IsValidKey(currentPeer.PublicKey))
                throw new ParseException(line, "[Peer] has no valid PublicKey");
            if (currentPsk is not null) peerPsks[currentPeer.Id] = currentPsk;
            if (currentPeer.AllowedIPs.Count == 0)
                warnings.Add($"peer {Prefix8(currentPeer.PublicKey)}…: empty AllowedIPs");
            cfg.Peers.Add(currentPeer);
            currentPeer = null;
            currentPsk = null;
        }

        var lines = text.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
        for (var i = 0; i < lines.Length; i++)
        {
            var lineNo = i + 1;
            var line = lines[i];
            var hash = line.IndexOf('#');
            if (hash >= 0) line = line[..hash];
            line = line.Trim();
            if (line.Length == 0) continue;

            var lower = line.ToLowerInvariant();
            if (lower == "[interface]") { FlushPeer(lineNo); section = Section.Interface; continue; }
            if (lower == "[peer]")
            {
                FlushPeer(lineNo);
                section = Section.Peer;
                currentPeer = new PeerConfig();
                continue;
            }

            var eq = line.IndexOf('=');
            if (eq < 0) throw new ParseException(lineNo, "expected key = value");
            var key = line[..eq].Trim().ToLowerInvariant();
            var value = line[(eq + 1)..].Trim();
            var list = value.Split(',').Select(s => s.Trim()).Where(s => s.Length > 0).ToList();

            switch (section)
            {
                case Section.Interface:
                    ParseInterfaceKey(key, value, list, lineNo, cfg, awg, ref privateKey, warnings);
                    break;
                case Section.Peer:
                    ParsePeerKey(key, value, list, lineNo, currentPeer!, ref currentPsk, warnings);
                    break;
                default:
                    throw new ParseException(lineNo, "key outside an [Interface]/[Peer] section");
            }
        }
        FlushPeer(lines.Length);

        if (privateKey is null) throw new ParseException(0, "no PrivateKey in [Interface]");
        if (cfg.Peers.Count == 0) throw new ParseException(0, "no [Peer] section found");

        var awgErrors = awg.Validate();
        if (awgErrors.Count > 0) throw new ParseException(0, string.Join("; ", awgErrors));

        if (!awg.IsEmpty)
        {
            cfg.Kind = TunnelKind.AmneziaWg;
            cfg.Awg = awg;
        }
        cfg.Interface.PublicKey = WgKey.PublicKey(privateKey) ?? "";
        if (cfg.Interface.PostUp.Count > 0 || cfg.Interface.PreUp.Count > 0)
            warnings.Add("config contains PreUp/PostUp scripts — TunHub stores but never executes them (security)");
        if (cfg.Interface.Addresses.Count == 0) warnings.Add("no Address in [Interface]");

        return new ParsedTunnel
        {
            Config = cfg,
            PrivateKey = privateKey,
            PresharedKeys = peerPsks,
            Warnings = warnings
        };
    }

    private static void ParseInterfaceKey(string key, string value, List<string> list, int lineNo,
        TunnelConfig cfg, AwgParams awg, ref string? privateKey, List<string> warnings)
    {
        switch (key)
        {
            case "privatekey":
                if (!WgKey.IsValidKey(value))
                    throw new ParseException(lineNo, "PrivateKey is not a 32-byte base64 key");
                privateKey = value;
                break;
            case "address":
                foreach (var a in list)
                {
                    var r = IpAddressRange.Parse(a) ?? throw new ParseException(lineNo, $"invalid Address: {a}");
                    cfg.Interface.Addresses.Add(r);
                }
                break;
            case "listenport":
                if (!ushort.TryParse(value, out var lp)) throw new ParseException(lineNo, "invalid ListenPort");
                cfg.Interface.ListenPort = lp;
                break;
            case "dns":
                foreach (var d in list)
                {
                    if (EndpointUtil.IsIpLiteral(d)) cfg.Interface.Dns.Add(d);
                    else cfg.Interface.DnsSearchDomains.Add(d);
                }
                break;
            case "mtu":
                if (!int.TryParse(value, out var m) || m < 576 || m > 9200)
                    throw new ParseException(lineNo, "invalid MTU");
                cfg.Interface.Mtu = m;
                break;
            case "table" or "fwmark" or "saveconfig":
                warnings.Add($"line {lineNo}: {key} is not supported on this platform, ignored");
                break;
            case "preup": cfg.Interface.PreUp.Add(value); break;
            case "postup": cfg.Interface.PostUp.Add(value); break;
            case "predown": cfg.Interface.PreDown.Add(value); break;
            case "postdown": cfg.Interface.PostDown.Add(value); break;
            // AmneziaWG
            case "jc": awg.Jc = ParseInt(value); break;
            case "jmin": awg.Jmin = ParseInt(value); break;
            case "jmax": awg.Jmax = ParseInt(value); break;
            case "s1": awg.S1 = ParseInt(value); break;
            case "s2": awg.S2 = ParseInt(value); break;
            case "s3": awg.S3 = ParseInt(value); break;
            case "s4": awg.S4 = ParseInt(value); break;
            case "h1": awg.H1 = ParseUInt(value); break;
            case "h2": awg.H2 = ParseUInt(value); break;
            case "h3": awg.H3 = ParseUInt(value); break;
            case "h4": awg.H4 = ParseUInt(value); break;
            case "i1": awg.I1 = value; break;
            case "i2": awg.I2 = value; break;
            case "i3": awg.I3 = value; break;
            case "i4": awg.I4 = value; break;
            case "i5": awg.I5 = value; break;
            case "itime": awg.ITime = ParseInt(value); break;
            case "j1" or "j2" or "j3":
                warnings.Add($"line {lineNo}: parameter {key.ToUpperInvariant()} is not yet supported by the core, ignored");
                break;
            default:
                warnings.Add($"line {lineNo}: unknown [Interface] key {key}");
                break;
        }
    }

    private static void ParsePeerKey(string key, string value, List<string> list, int lineNo,
        PeerConfig peer, ref string? currentPsk, List<string> warnings)
    {
        switch (key)
        {
            case "publickey":
                if (!WgKey.IsValidKey(value))
                    throw new ParseException(lineNo, "PublicKey is not a 32-byte base64 key");
                peer.PublicKey = value;
                break;
            case "presharedkey":
                if (!WgKey.IsValidKey(value))
                    throw new ParseException(lineNo, "PresharedKey is not a 32-byte base64 key");
                currentPsk = value;
                break;
            case "allowedips":
                foreach (var a in list)
                {
                    var r = IpAddressRange.Parse(a) ?? throw new ParseException(lineNo, $"invalid AllowedIPs: {a}");
                    peer.AllowedIPs.Add(r);
                }
                break;
            case "endpoint":
                if (EndpointUtil.Split(value) is null)
                    throw new ParseException(lineNo, "invalid Endpoint (expected host:port)");
                peer.Endpoint = value;
                break;
            case "persistentkeepalive":
                if (!ushort.TryParse(value, out var k)) throw new ParseException(lineNo, "invalid PersistentKeepalive");
                peer.PersistentKeepalive = k;
                break;
            default:
                warnings.Add($"line {lineNo}: unknown [Peer] key {key}");
                break;
        }
    }

    private static int? ParseInt(string s) => int.TryParse(s, out var v) ? v : null;
    private static uint? ParseUInt(string s) => uint.TryParse(s, out var v) ? v : null;
    private static string Prefix8(string s) => s.Length <= 8 ? s : s[..8];

    // MARK: - Serialize

    public static string Serialize(TunnelConfig config, string? privateKey,
        IReadOnlyDictionary<Guid, string> presharedKeys, bool redactSecrets)
    {
        var sb = new StringBuilder();
        sb.Append("[Interface]\n");
        sb.Append($"PrivateKey = {(redactSecrets ? "<REDACTED>" : privateKey ?? "<MISSING>")}\n");
        if (config.Interface.Addresses.Count > 0)
            sb.Append($"Address = {string.Join(", ", config.Interface.Addresses.Select(a => $"{a.AddressString}/{a.Prefix}"))}\n");
        if (config.Interface.ListenPort is { } lp) sb.Append($"ListenPort = {lp}\n");
        var dnsAll = config.Interface.Dns.Concat(config.Interface.DnsSearchDomains).ToList();
        if (dnsAll.Count > 0) sb.Append($"DNS = {string.Join(", ", dnsAll)}\n");
        if (config.Interface.Mtu is { } mtu) sb.Append($"MTU = {mtu}\n");

        if (config.Awg is { } a && config.Kind == TunnelKind.AmneziaWg)
        {
            void Put(string k, object? v) { if (v is not null) sb.Append($"{k} = {v}\n"); }
            Put("Jc", a.Jc); Put("Jmin", a.Jmin); Put("Jmax", a.Jmax);
            Put("S1", a.S1); Put("S2", a.S2); Put("S3", a.S3); Put("S4", a.S4);
            Put("H1", a.H1); Put("H2", a.H2); Put("H3", a.H3); Put("H4", a.H4);
            Put("I1", a.I1); Put("I2", a.I2); Put("I3", a.I3); Put("I4", a.I4); Put("I5", a.I5);
            Put("ITime", a.ITime);
        }

        foreach (var p in config.Peers)
        {
            sb.Append("\n[Peer]\n");
            sb.Append($"PublicKey = {p.PublicKey}\n");
            if (p.PresharedKeyRef is not null || presharedKeys.ContainsKey(p.Id))
            {
                presharedKeys.TryGetValue(p.Id, out var psk);
                sb.Append($"PresharedKey = {(redactSecrets ? "<REDACTED>" : psk ?? "<MISSING>")}\n");
            }
            if (p.AllowedIPs.Count > 0)
                sb.Append($"AllowedIPs = {string.Join(", ", p.AllowedIPs.Select(r => $"{r.AddressString}/{r.Prefix}"))}\n");
            if (p.Endpoint is { } e) sb.Append($"Endpoint = {e}\n");
            if (p.PersistentKeepalive is { } ka) sb.Append($"PersistentKeepalive = {ka}\n");
        }
        return sb.ToString();
    }
}
