using System.Net;

namespace TunHub.Core;

/// <summary>Parsing helpers for "host:port" endpoints (IPv4, IPv6-in-brackets, hostnames).</summary>
public static class EndpointUtil
{
    /// <summary>Split "host:port". Handles "[v6]:port". Returns null if malformed.</summary>
    public static (string Host, ushort Port)? Split(string endpoint)
    {
        endpoint = endpoint.Trim();
        if (endpoint.Length == 0) return null;

        string host;
        string portStr;
        if (endpoint.StartsWith('['))
        {
            var close = endpoint.IndexOf(']');
            if (close < 0) return null;
            host = endpoint[1..close];
            var rest = endpoint[(close + 1)..];
            if (!rest.StartsWith(':')) return null;
            portStr = rest[1..];
        }
        else
        {
            var lastColon = endpoint.LastIndexOf(':');
            // more than one colon and no brackets → bare IPv6 without a port
            if (lastColon < 0 || endpoint.IndexOf(':') != lastColon) return null;
            host = endpoint[..lastColon];
            portStr = endpoint[(lastColon + 1)..];
        }

        if (host.Length == 0) return null;
        if (!ushort.TryParse(portStr, out var port) || port == 0) return null;
        return (host, port);
    }

    /// <summary>True if the string is a numeric IPv4/IPv6 literal (not a hostname).</summary>
    public static bool IsIpLiteral(string s) => IPAddress.TryParse(s, out _);
}
