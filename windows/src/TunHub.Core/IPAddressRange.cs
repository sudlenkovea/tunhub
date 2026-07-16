using System.Net;

namespace TunHub.Core;

/// <summary>CIDR range (v4/v6) with canonicalization and overlap math.</summary>
public readonly struct IpAddressRange : IEquatable<IpAddressRange>
{
    /// <summary>Address as the user typed it (address part only).</summary>
    public string AddressString { get; }
    public int Prefix { get; }
    /// <summary>4 or 16 bytes, network byte order.</summary>
    public byte[] Bytes { get; }

    public bool IsIPv6 => Bytes.Length == 16;
    public int MaxPrefix => IsIPv6 ? 128 : 32;

    private IpAddressRange(string addressString, int prefix, byte[] bytes)
    {
        AddressString = addressString;
        Prefix = prefix;
        Bytes = bytes;
    }

    /// <summary>Parse "addr" or "addr/prefix". Returns null on failure.</summary>
    public static IpAddressRange? Parse(string text)
    {
        text = text.Trim();
        if (text.Length == 0) return null;

        string addrPart;
        int prefix;
        var slash = text.IndexOf('/');
        if (slash >= 0)
        {
            addrPart = text[..slash];
            if (!int.TryParse(text[(slash + 1)..], out prefix)) return null;
        }
        else
        {
            addrPart = text;
            prefix = -1; // filled in after we know the family
        }

        var bytes = Pton(addrPart);
        if (bytes is null) return null;

        var maxPrefix = bytes.Length == 16 ? 128 : 32;
        if (prefix < 0) prefix = maxPrefix;
        if (prefix < 0 || prefix > maxPrefix) return null;

        return new IpAddressRange(addrPart, prefix, bytes);
    }

    /// <summary>Numeric IP literal → raw bytes (4 or 16). Null if not a literal.</summary>
    public static byte[]? Pton(string host)
    {
        return IPAddress.TryParse(host, out var ip) ? ip.GetAddressBytes() : null;
    }

    public static string Ntop(byte[] bytes) => new IPAddress(bytes).ToString();

    public string AddressStringCanonical => Ntop(Bytes);

    /// <summary>Network address (host bits zeroed).</summary>
    public byte[] NetworkBytes()
    {
        var outBytes = (byte[])Bytes.Clone();
        var fullBytes = Prefix / 8;
        var remBits = Prefix % 8;
        for (var i = 0; i < outBytes.Length; i++)
        {
            if (i < fullBytes) continue;
            if (i == fullBytes && remBits > 0)
                outBytes[i] &= (byte)((0xFF << (8 - remBits)) & 0xFF);
            else
                outBytes[i] = 0;
        }
        return outBytes;
    }

    /// <summary>Canonical form "network/prefix".</summary>
    public string Canonical => $"{Ntop(NetworkBytes())}/{Prefix}";

    public override string ToString() => Canonical;

    /// <summary>self (as a network) fully contains the other network.</summary>
    public bool Contains(IpAddressRange other)
    {
        if (IsIPv6 != other.IsIPv6 || Prefix > other.Prefix) return false;
        var a = NetworkBytes();
        var b = other.NetworkBytes();
        var fullBytes = Prefix / 8;
        var remBits = Prefix % 8;
        for (var i = 0; i < fullBytes; i++)
            if (a[i] != b[i]) return false;
        if (remBits > 0)
        {
            var mask = (byte)((0xFF << (8 - remBits)) & 0xFF);
            if ((a[fullBytes] & mask) != (b[fullBytes] & mask)) return false;
        }
        return true;
    }

    /// <summary>Two ranges overlap if either contains the other.</summary>
    public bool Overlaps(IpAddressRange other) => Contains(other) || other.Contains(this);

    /// <summary>Whether the range contains a specific address literal.</summary>
    public bool ContainsAddress(string addr)
    {
        var r = Parse(addr);
        return r is not null && Contains(r.Value);
    }

    public bool Equals(IpAddressRange other) => Canonical == other.Canonical;
    public override bool Equals(object? obj) => obj is IpAddressRange r && Equals(r);
    public override int GetHashCode() => Canonical.GetHashCode();
}
