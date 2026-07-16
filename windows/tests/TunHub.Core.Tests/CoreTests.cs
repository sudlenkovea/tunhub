using TunHub.Core;
using Xunit;

namespace TunHub.Core.Tests;

public class IpAddressRangeTests
{
    [Theory]
    [InlineData("10.0.0.0/8", "10.1.2.3/32", true)]
    [InlineData("10.0.0.0/8", "11.0.0.0/8", false)]
    [InlineData("0.0.0.0/0", "8.8.8.8/32", true)]
    [InlineData("192.168.1.0/24", "192.168.2.0/24", false)]
    public void Contains_Works(string outer, string inner, bool expected)
    {
        var a = IpAddressRange.Parse(outer)!.Value;
        var b = IpAddressRange.Parse(inner)!.Value;
        Assert.Equal(expected, a.Contains(b));
    }

    [Fact]
    public void Canonical_ZeroesHostBits()
    {
        Assert.Equal("10.0.0.0/8", IpAddressRange.Parse("10.1.2.3/8")!.Value.Canonical);
        Assert.Equal("192.168.1.0/24", IpAddressRange.Parse("192.168.1.55/24")!.Value.Canonical);
    }

    [Fact]
    public void Parse_BareAddress_DefaultsToFullPrefix()
    {
        Assert.Equal(32, IpAddressRange.Parse("1.2.3.4")!.Value.Prefix);
        Assert.Equal(128, IpAddressRange.Parse("2001:db8::1")!.Value.Prefix);
    }

    [Fact]
    public void ContainsAddress_Works()
    {
        Assert.True(IpAddressRange.Parse("0.0.0.0/0")!.Value.ContainsAddress("1.1.1.1"));
        Assert.False(IpAddressRange.Parse("10.0.0.0/8")!.Value.ContainsAddress("11.0.0.1"));
    }
}

public class EndpointUtilTests
{
    [Theory]
    [InlineData("1.2.3.4:51820", "1.2.3.4", 51820)]
    [InlineData("[2001:db8::1]:443", "2001:db8::1", 443)]
    [InlineData("vpn.example.com:1194", "vpn.example.com", 1194)]
    public void Split_Works(string ep, string host, ushort port)
    {
        var r = EndpointUtil.Split(ep);
        Assert.NotNull(r);
        Assert.Equal(host, r!.Value.Host);
        Assert.Equal(port, r.Value.Port);
    }

    [Theory]
    [InlineData("no-port")]
    [InlineData("2001:db8::1")] // bare IPv6, ambiguous → rejected
    [InlineData("host:0")]
    public void Split_RejectsMalformed(string ep) => Assert.Null(EndpointUtil.Split(ep));
}

public class WgQuickParserTests
{
    // Valid AmneziaWG config with ranged headers H1=1..H4=4 (the case that used to be rejected).
    private const string AwgConfig = """
        [Interface]
        PrivateKey = 6svAEKX57qt6VUQ0jMq+RbQwDSRxBNhA9tAqFkRPrEU=
        Address = 10.8.0.2/32
        DNS = 1.1.1.1
        Jc = 4
        Jmin = 40
        Jmax = 70
        S1 = 100
        S2 = 100
        H1 = 1
        H2 = 2
        H3 = 3
        H4 = 4

        [Peer]
        PublicKey = 0I9GgaHZgfT4blf1nbWWKXaLli6ryV3ApTMhFjsbUpQ=
        AllowedIPs = 0.0.0.0/0
        Endpoint = 1.2.3.4:51820
        PersistentKeepalive = 25
        """;

    [Fact]
    public void Parses_ValidAwgConfig_WithRangedHeaders()
    {
        var parsed = WgQuickParser.Parse("latvia", AwgConfig);
        Assert.Equal(TunnelKind.AmneziaWg, parsed.Config.Kind);
        Assert.Equal(1u, parsed.Config.Awg!.H1);
        Assert.Equal(4u, parsed.Config.Awg!.H4);
        Assert.Single(parsed.Config.Peers);
        Assert.Equal("1.2.3.4:51820", parsed.Config.Peers[0].Endpoint);
        Assert.True(parsed.Config.HasDefaultRoute);
    }

    [Fact]
    public void RoundTrips_SerializeThenParse()
    {
        var parsed = WgQuickParser.Parse("t", AwgConfig);
        var text = WgQuickParser.Serialize(parsed.Config, parsed.PrivateKey, parsed.PresharedKeys, redactSecrets: false);
        var again = WgQuickParser.Parse("t", text);
        Assert.Equal(parsed.Config.Awg!.H4, again.Config.Awg!.H4);
        Assert.Equal(parsed.Config.Peers[0].AllowedIPs[0].Canonical,
                     again.Config.Peers[0].AllowedIPs[0].Canonical);
    }

    [Fact]
    public void Rejects_MissingPrivateKey()
    {
        var ex = Assert.Throws<ParseException>(() =>
            WgQuickParser.Parse("x", "[Interface]\nAddress = 10.0.0.2/32\n\n[Peer]\nPublicKey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=\nAllowedIPs = 0.0.0.0/0\n"));
        Assert.Contains("PrivateKey", ex.Message);
    }
}

public class ConflictCheckerTests
{
    private static TunnelConfig DefaultRouteTunnel(string name)
    {
        var cfg = new TunnelConfig { Name = name };
        cfg.Peers.Add(new PeerConfig { AllowedIPs = { IpAddressRange.Parse("0.0.0.0/0")!.Value } });
        return cfg;
    }

    [Fact]
    public void DetectsDefaultRouteClash()
    {
        var a = DefaultRouteTunnel("a");
        var b = DefaultRouteTunnel("b");
        var findings = ConflictChecker.Check(a, new[] { b });
        Assert.Contains(findings, f => f.Code == "DefaultRouteClash");
        Assert.True(ConflictChecker.HasErrors(findings));
    }
}
