namespace TunHub.Core;

// MARK: - OpenVPN models (mirrors the macOS Swift implementation)

public sealed class OpenVpnRemote
{
    public string Host { get; set; } = "";
    public ushort Port { get; set; } = 1194;
    public string Proto { get; set; } = "udp";   // "udp" / "tcp"

    public OpenVpnRemote() { }
    public OpenVpnRemote(string host, ushort port, string proto)
    {
        Host = host; Port = port; Proto = proto;
    }
}

public enum OpenVpnAuthMode
{
    Cert,            // certificate only
    UserPass,        // username/password only
    UserPassCert     // both
}

public sealed class OpenVpnStaticChallenge
{
    public string Text { get; set; } = "";
    public bool Echo { get; set; }   // whether the OTP field should be shown (not masked)

    public OpenVpnStaticChallenge() { }
    public OpenVpnStaticChallenge(string text, bool echo) { Text = text; Echo = echo; }
}

/// <summary>
/// Parsed metadata for an OpenVPN profile. The full <c>.ovpn</c> (with sensitive inline
/// blocks replaced by placeholders) lives in <see cref="ConfigText"/>; the actual secret
/// material and username/password live in the OS secret store. Scripts are never executed.
/// </summary>
public sealed class OpenVpnProfile
{
    public List<OpenVpnRemote> Remotes { get; set; } = new();
    public OpenVpnAuthMode AuthMode { get; set; } = OpenVpnAuthMode.Cert;
    public bool NeedsUsername { get; set; }            // `auth-user-pass` present (no inline creds)
    public OpenVpnStaticChallenge? StaticChallenge { get; set; }
    public string? Cipher { get; set; }                // legacy single `cipher`
    public List<string> DataCiphers { get; set; } = new();   // `data-ciphers`
    public bool RedirectGateway { get; set; }
    public List<string> Dns { get; set; } = new();           // `dhcp-option DNS`
    public List<string> SearchDomains { get; set; } = new(); // `dhcp-option DOMAIN`
    public bool UsesInlineCompression { get; set; }          // comp-lzo / compress (VORACLE warning)
    /// <summary>Raw .ovpn text with sensitive inline blocks replaced by ##SECRET:tag## placeholders,
    /// resolved at connect time.</summary>
    public string ConfigText { get; set; } = "";
}

/// <summary>
/// OpenVPN payload of a resolved spec: the full .ovpn with inline secrets already
/// substituted (ready to write to a locked-down temp file) plus credentials/OTP.
/// </summary>
public sealed class ResolvedOpenVpn
{
    public string ConfigText { get; set; } = "";       // complete .ovpn, secrets inlined
    public string? Username { get; set; }
    public string? Password { get; set; }
    public string? Otp { get; set; }                   // pre-entered OTP (static-challenge)
    public OpenVpnStaticChallenge? StaticChallenge { get; set; }
    public List<OpenVpnRemote> Remotes { get; set; } = new();  // for kill-switch endpoint pinning
    public List<string> Dns { get; set; } = new();     // client-side dhcp-option DNS (if any)
    public bool RedirectGateway { get; set; }
}

/// <summary>Parse result: profile metadata + secrets pulled out for the OS secret store.</summary>
public sealed class ParsedOpenVpn
{
    public OpenVpnProfile Profile { get; set; } = new();
    /// <summary>Sensitive material for the secret store (tag → PEM/base64, plus optional
    /// "username"/"password" if the profile carried inline credentials).</summary>
    public Dictionary<string, string> Secrets { get; set; } = new();
    public List<string> Warnings { get; set; } = new();
}
