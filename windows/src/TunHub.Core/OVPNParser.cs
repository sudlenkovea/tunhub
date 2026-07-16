using System.Text;

namespace TunHub.Core;

/// <summary>
/// Parser for OpenVPN <c>.ovpn</c> profiles. Inline secret blocks (<c>&lt;key&gt;</c>,
/// <c>&lt;tls-auth&gt;</c>, <c>&lt;tls-crypt&gt;</c>, <c>&lt;tls-crypt-v2&gt;</c>,
/// <c>&lt;pkcs12&gt;</c>) are stripped to placeholders and their contents returned as secrets.
/// Script directives are never executed — they are dropped with a warning (same policy as
/// wg-quick PreUp/PostUp).
/// </summary>
public static class OVPNParser
{
    public sealed class OVPNParseException : Exception
    {
        public OVPNParseException(string message) : base(message) { }
    }

    /// <summary>Inline blocks that hold secrets (redacted out of the stored config text).</summary>
    private static readonly HashSet<string> SecretTags =
        new(StringComparer.OrdinalIgnoreCase) { "key", "tls-auth", "tls-crypt", "tls-crypt-v2", "pkcs12" };

    /// <summary>Directives that run external code — dropped for safety (never executed).</summary>
    private static readonly HashSet<string> ScriptDirectives =
        new(StringComparer.OrdinalIgnoreCase)
        {
            "up", "down", "route-up", "route-pre-down", "ipchange", "tls-verify",
            "auth-user-pass-verify", "client-connect", "client-disconnect",
            "learn-address", "up-restart", "script-security"
        };

    public static ParsedOpenVpn Parse(string name, string text)
    {
        var profile = new OpenVpnProfile();
        var secrets = new Dictionary<string, string>();
        var warnings = new List<string>();

        bool hasCert = false, hasKey = false, hasAuthUserPass = false;
        string globalProto = "udp";
        ushort globalPort = 1194;
        var rawRemotes = new List<(string Host, ushort? Port, string? Proto)>();
        var outLines = new List<string>();

        var lines = text.Replace("\r\n", "\n").Replace("\r", "\n").Split('\n');
        int i = 0;
        while (i < lines.Length)
        {
            var raw = lines[i];
            var trimmed = raw.Trim();

            // Inline block: <tag> ... </tag>
            if (trimmed.StartsWith('<') && trimmed.EndsWith('>') && !trimmed.StartsWith("</"))
            {
                var tag = trimmed.Substring(1, trimmed.Length - 2);
                var body = new List<string>();
                i++;
                while (i < lines.Length && lines[i].Trim() != $"</{tag}>")
                {
                    body.Add(lines[i]); i++;
                }
                var content = string.Join("\n", body);
                if (tag.Equals("cert", StringComparison.OrdinalIgnoreCase)) hasCert = true;
                if (SecretTags.Contains(tag))
                {
                    secrets[tag] = content;
                    if (tag.Equals("key", StringComparison.OrdinalIgnoreCase)) hasKey = true;
                    outLines.Add($"<{tag}>");
                    outLines.Add($"##SECRET:{tag}##");
                    outLines.Add($"</{tag}>");
                }
                else
                {
                    // Public material (ca, cert, dh, extra-certs): keep inline.
                    outLines.Add($"<{tag}>");
                    outLines.AddRange(body);
                    outLines.Add($"</{tag}>");
                }
                i++;
                continue;
            }

            bool isComment = trimmed.StartsWith('#') || trimmed.StartsWith(';');
            if (trimmed.Length == 0 || isComment) { outLines.Add(raw); i++; continue; }

            var tokens = Tokenize(trimmed);
            var key = tokens[0].ToLowerInvariant();
            var args = tokens.Skip(1).ToList();

            switch (key)
            {
                case "remote":
                {
                    if (args.Count == 0) break;
                    ushort? port = args.Count > 1 && ushort.TryParse(args[1], out var pv) ? pv : null;
                    string? proto = args.Count > 2 ? NormalizeProto(args[2]) : null;
                    rawRemotes.Add((args[0], port, proto));
                    outLines.Add(raw);
                    break;
                }
                case "proto":
                    if (args.Count > 0) globalProto = NormalizeProto(args[0]);
                    outLines.Add(raw);
                    break;
                case "port":
                    if (args.Count > 0 && ushort.TryParse(args[0], out var gp)) globalPort = gp;
                    outLines.Add(raw);
                    break;
                case "cipher":
                    profile.Cipher = args.FirstOrDefault();
                    outLines.Add(raw);
                    break;
                case "data-ciphers":
                case "ncp-ciphers":
                    profile.DataCiphers = (args.FirstOrDefault() ?? "")
                        .Split(':', StringSplitOptions.RemoveEmptyEntries).ToList();
                    outLines.Add(raw);
                    break;
                case "auth-user-pass":
                    hasAuthUserPass = true;
                    // A file argument can't be trusted; the management interface supplies credentials.
                    outLines.Add("auth-user-pass");
                    break;
                case "static-challenge":
                {
                    // static-challenge "prompt text" <echo 0|1>
                    bool echo = args.Count > 0 && args[^1] == "1";
                    var promptTokens = echo ? args.Take(args.Count - 1) : args;
                    var promptText = string.Join(" ", promptTokens).Trim('"');
                    profile.StaticChallenge = new OpenVpnStaticChallenge(promptText, echo);
                    outLines.Add(raw);
                    break;
                }
                case "redirect-gateway":
                case "redirect-private":
                    profile.RedirectGateway = true;
                    outLines.Add(raw);
                    break;
                case "dhcp-option":
                    if (args.Count >= 2)
                    {
                        switch (args[0].ToUpperInvariant())
                        {
                            case "DNS":
                            case "DNS6": profile.Dns.Add(args[1]); break;
                            case "DOMAIN":
                            case "DOMAIN-SEARCH":
                            case "ADAPTER_DOMAIN_SUFFIX": profile.SearchDomains.Add(args[1]); break;
                        }
                    }
                    outLines.Add(raw);
                    break;
                case "comp-lzo":
                case "compress":
                    profile.UsesInlineCompression = true;
                    warnings.Add($"compression enabled ({key}) — vulnerable to VORACLE; consider disabling on the server");
                    outLines.Add(raw);
                    break;
                case "verb":
                case "mute":
                    // Verbosity is controlled by the helper (so errors aren't hidden by --mute).
                    break;
                case "key":
                case "tls-auth":
                case "tls-crypt":
                case "tls-crypt-v2":
                case "pkcs12":
                    // File-based secret reference (not inline). We can't bundle the external file.
                    warnings.Add($"directive '{key}' points to an external file — inline it into the .ovpn (<{key}>…</{key}>) so TunHub can store it securely");
                    if (key == "key") hasKey = true;
                    outLines.Add(raw);
                    break;
                case "cert":
                    hasCert = true;
                    outLines.Add(raw);
                    break;
                default:
                    if (ScriptDirectives.Contains(key))
                        warnings.Add($"script directive '{key}' is ignored for security (never executed)");
                    else
                        outLines.Add(raw);
                    break;
            }
            i++;
        }

        // Resolve remotes against global proto/port.
        foreach (var r in rawRemotes)
            profile.Remotes.Add(new OpenVpnRemote(r.Host, r.Port ?? globalPort, r.Proto ?? globalProto));

        if (profile.Remotes.Count == 0)
            throw new OVPNParseException("no `remote` found in the OpenVPN profile");

        // Auth mode.
        if (hasAuthUserPass && (hasCert || hasKey)) profile.AuthMode = OpenVpnAuthMode.UserPassCert;
        else if (hasAuthUserPass) { profile.AuthMode = OpenVpnAuthMode.UserPass; profile.NeedsUsername = true; }
        else profile.AuthMode = OpenVpnAuthMode.Cert;
        if (hasAuthUserPass && profile.AuthMode == OpenVpnAuthMode.UserPassCert) profile.NeedsUsername = true;

        profile.ConfigText = string.Join("\n", outLines);
        return new ParsedOpenVpn { Profile = profile, Secrets = secrets, Warnings = warnings };
    }

    // MARK: helpers

    private static string NormalizeProto(string p) =>
        p.ToLowerInvariant().StartsWith("tcp") ? "tcp" : "udp";

    /// <summary>Split a directive line into tokens, honoring double quotes (for static-challenge).</summary>
    private static List<string> Tokenize(string line)
    {
        var tokens = new List<string>();
        var cur = new StringBuilder();
        bool inQuotes = false;
        foreach (var ch in line)
        {
            if (ch == '"') { inQuotes = !inQuotes; continue; }
            if ((ch == ' ' || ch == '\t') && !inQuotes)
            {
                if (cur.Length > 0) { tokens.Add(cur.ToString()); cur.Clear(); }
            }
            else cur.Append(ch);
        }
        if (cur.Length > 0) tokens.Add(cur.ToString());
        return tokens;
    }
}
