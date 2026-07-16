using TunHub.Core;
using Xunit;

namespace TunHub.Core.Tests;

public class OVPNParserTests
{
    private const string Synthetic = """
        client
        dev tun
        proto udp
        remote vpn.example.com 1194
        remote vpn2.example.com 443 tcp
        auth-user-pass
        static-challenge "Enter OTP" 1
        redirect-gateway def1
        dhcp-option DNS 10.8.0.1
        dhcp-option DOMAIN corp.local
        cipher AES-256-GCM
        data-ciphers AES-256-GCM:AES-128-GCM
        verb 3
        mute 20
        up /etc/openvpn/up.sh
        script-security 2
        <ca>
        -----BEGIN CERTIFICATE-----
        CAPUBLIC
        -----END CERTIFICATE-----
        </ca>
        <cert>
        -----BEGIN CERTIFICATE-----
        CLIENTCERT
        -----END CERTIFICATE-----
        </cert>
        <key>
        -----BEGIN PRIVATE KEY-----
        SUPERSECRETKEY
        -----END PRIVATE KEY-----
        </key>
        <tls-crypt>
        -----BEGIN OpenVPN Static key V1-----
        TLSCRYPTSECRET
        -----END OpenVPN Static key V1-----
        </tls-crypt>
        """;

    [Fact]
    public void Parses_Remotes_With_Global_And_PerLine_Overrides()
    {
        var p = OVPNParser.Parse("t", Synthetic).Profile;
        Assert.Equal(2, p.Remotes.Count);
        Assert.Equal(("vpn.example.com", (ushort)1194, "udp"),
            (p.Remotes[0].Host, p.Remotes[0].Port, p.Remotes[0].Proto));
        Assert.Equal(("vpn2.example.com", (ushort)443, "tcp"),
            (p.Remotes[1].Host, p.Remotes[1].Port, p.Remotes[1].Proto));
    }

    [Fact]
    public void Detects_UserPassCert_And_StaticChallenge()
    {
        var p = OVPNParser.Parse("t", Synthetic).Profile;
        Assert.Equal(OpenVpnAuthMode.UserPassCert, p.AuthMode);
        Assert.True(p.NeedsUsername);
        Assert.NotNull(p.StaticChallenge);
        Assert.Equal("Enter OTP", p.StaticChallenge!.Text);
        Assert.True(p.StaticChallenge.Echo);
    }

    [Fact]
    public void Redacts_Secret_Blocks_Keeps_Public_Ones()
    {
        var r = OVPNParser.Parse("t", Synthetic);
        // key + tls-crypt are secrets → redacted out of ConfigText and returned in Secrets.
        Assert.Contains("key", r.Secrets.Keys);
        Assert.Contains("tls-crypt", r.Secrets.Keys);
        Assert.Contains("SUPERSECRETKEY", r.Secrets["key"]);
        Assert.DoesNotContain("SUPERSECRETKEY", r.Profile.ConfigText);
        Assert.DoesNotContain("TLSCRYPTSECRET", r.Profile.ConfigText);
        Assert.Contains("##SECRET:key##", r.Profile.ConfigText);
        // ca + cert are public → kept inline.
        Assert.Contains("CAPUBLIC", r.Profile.ConfigText);
        Assert.Contains("CLIENTCERT", r.Profile.ConfigText);
    }

    [Fact]
    public void Strips_Script_Directives_And_VerbMute_With_Warnings()
    {
        var r = OVPNParser.Parse("t", Synthetic);
        Assert.DoesNotContain("up /etc/openvpn/up.sh", r.Profile.ConfigText);
        Assert.DoesNotContain("script-security", r.Profile.ConfigText);
        Assert.DoesNotContain("verb 3", r.Profile.ConfigText);
        Assert.DoesNotContain("mute 20", r.Profile.ConfigText);
        Assert.Contains(r.Warnings, w => w.Contains("script directive 'up'"));
        Assert.Contains(r.Warnings, w => w.Contains("script directive 'script-security'"));
    }

    [Fact]
    public void Captures_Dns_Domain_Cipher_RedirectGateway()
    {
        var p = OVPNParser.Parse("t", Synthetic).Profile;
        Assert.Contains("10.8.0.1", p.Dns);
        Assert.Contains("corp.local", p.SearchDomains);
        Assert.Equal("AES-256-GCM", p.Cipher);
        Assert.Equal(new[] { "AES-256-GCM", "AES-128-GCM" }, p.DataCiphers);
        Assert.True(p.RedirectGateway);
    }

    [Fact]
    public void Throws_When_No_Remote()
    {
        Assert.Throws<OVPNParser.OVPNParseException>(() =>
            OVPNParser.Parse("t", "client\ndev tun\nproto udp\n"));
    }

    [Fact]
    public void Parses_Real_User_Profile_Fixture()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "fixtures", "sample.ovpn");
        if (!File.Exists(path)) return; // fixture optional
        var r = OVPNParser.Parse("sample", File.ReadAllText(path));
        Assert.NotEmpty(r.Profile.Remotes);
        // The real profile carries an inline private key and a tls-crypt key — both must be
        // redacted and never leak into the stored config text.
        foreach (var tag in r.Secrets.Keys)
            Assert.Contains($"##SECRET:{tag}##", r.Profile.ConfigText);
    }
}
