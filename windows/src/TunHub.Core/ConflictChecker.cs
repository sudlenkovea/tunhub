namespace TunHub.Core;

public enum FindingSeverity { Info, Warning, Error }

public sealed class ConflictFinding
{
    public Guid Id { get; set; } = Guid.NewGuid();
    public FindingSeverity Severity { get; set; }
    public string Code { get; set; } = "";
    public string Message { get; set; } = "";
    public List<string> TunnelNames { get; set; } = new();
    public string? FixHint { get; set; }

    public ConflictFinding() { }

    public ConflictFinding(FindingSeverity severity, string code, string message,
        IEnumerable<string> tunnels, string? fixHint = null)
    {
        Severity = severity;
        Code = code;
        Message = message;
        TunnelNames = tunnels.ToList();
        FixHint = fixHint;
    }
}

/// <summary>Checks route/DNS/address/port overlaps between tunnels. Design §5.3.</summary>
public static class ConflictChecker
{
    /// <summary>Candidate against the set of active tunnels.</summary>
    public static List<ConflictFinding> Check(TunnelConfig candidate, IReadOnlyList<TunnelConfig> active)
    {
        var outList = new List<ConflictFinding>();
        outList.AddRange(SelfCheck(candidate));
        foreach (var other in active.Where(o => o.Id != candidate.Id))
            outList.AddRange(PairCheck(candidate, other));
        return SortBySeverity(outList);
    }

    /// <summary>All pairs in the set (for "Check all").</summary>
    public static List<ConflictFinding> CheckAll(IReadOnlyList<TunnelConfig> tunnels)
    {
        var outList = new List<ConflictFinding>();
        foreach (var t in tunnels) outList.AddRange(SelfCheck(t));
        for (var i = 0; i < tunnels.Count; i++)
            for (var j = i + 1; j < tunnels.Count; j++)
                outList.AddRange(PairCheck(tunnels[i], tunnels[j]));
        return SortBySeverity(outList);
    }

    private static List<ConflictFinding> SortBySeverity(List<ConflictFinding> f) =>
        f.OrderByDescending(x => x.Severity).ToList();

    // MARK: - Single-tunnel checks

    private static List<ConflictFinding> SelfCheck(TunnelConfig t)
    {
        var outList = new List<ConflictFinding>();
        var routes = t.EffectiveRoutes();

        // DNSUnreachable: DNS server not covered by AllowedIPs
        foreach (var dns in t.Interface.Dns)
        {
            if (!routes.Any(r => r.ContainsAddress(dns)))
                outList.Add(new(FindingSeverity.Warning, "DNSUnreachable",
                    $"DNS {dns} of “{t.Name}” is not covered by its AllowedIPs — resolution will bypass the tunnel",
                    new[] { t.Name },
                    $"Add {dns}/32 to AllowedIPs or change the DNS"));
        }

        // EndpointInsideTunnel (the tunnel's own routes)
        foreach (var p in t.Peers)
        {
            if (p.Endpoint is null || EndpointUtil.Split(p.Endpoint) is not { } split) continue;
            var host = split.Host;
            if (!EndpointUtil.IsIpLiteral(host)) continue;
            if (routes.Any(r => r.ContainsAddress(host)))
                outList.Add(new(FindingSeverity.Info, "EndpointPinned",
                    $"Endpoint {host} falls inside “{t.Name}” routes — TunHub will pin it via the physical gateway automatically",
                    new[] { t.Name }));
        }

        if (t.Awg is { } a)
            foreach (var e in a.Validate())
                outList.Add(new(FindingSeverity.Error, "AWGParamInvalid", $"“{t.Name}”: {e}", new[] { t.Name }));

        return outList;
    }

    // MARK: - Pair checks

    private static List<ConflictFinding> PairCheck(TunnelConfig a, TunnelConfig b)
    {
        var outList = new List<ConflictFinding>();
        var ra = a.EffectiveRoutes();
        var rb = b.EffectiveRoutes();

        // 1. Default route clash (prefix 0 or the /1 pair)
        if (a.HasDefaultRoute && b.HasDefaultRoute)
            outList.Add(new(FindingSeverity.Error, "DefaultRouteClash",
                $"“{a.Name}” and “{b.Name}” both claim all traffic (default route). They cannot run at the same time.",
                new[] { a.Name, b.Name },
                "Keep the default route on one; move the other to specific subnets or split DNS"));

        // 2/3. Routes: exact duplicate / shadowing
        foreach (var x in ra)
        foreach (var y in rb)
        {
            if (x.IsIPv6 != y.IsIPv6) continue;
            if (x.Prefix <= 1 || y.Prefix <= 1) continue; // default handled above
            if (x.Canonical == y.Canonical)
                outList.Add(new(FindingSeverity.Error, "ExactDuplicate",
                    $"Identical route {x.Canonical} in “{a.Name}” and “{b.Name}”",
                    new[] { a.Name, b.Name }));
            else if (x.Contains(y))
                outList.Add(new(FindingSeverity.Warning, "SubnetShadowing",
                    $"{y.Canonical} (“{b.Name}”) is nested in {x.Canonical} (“{a.Name}”) — traffic goes to the more specific “{b.Name}”",
                    new[] { a.Name, b.Name }));
            else if (y.Contains(x))
                outList.Add(new(FindingSeverity.Warning, "SubnetShadowing",
                    $"{x.Canonical} (“{a.Name}”) is nested in {y.Canonical} (“{b.Name}”) — traffic goes to the more specific “{a.Name}”",
                    new[] { a.Name, b.Name }));
        }

        // 4. Interface address overlap
        foreach (var x in a.Interface.Addresses)
        foreach (var y in b.Interface.Addresses)
            if (x.Overlaps(y))
                outList.Add(new(FindingSeverity.Error, "AddressOverlap",
                    $"Interface addresses overlap: {x.Canonical} (“{a.Name}”) and {y.Canonical} (“{b.Name}”)",
                    new[] { a.Name, b.Name }));

        // 5. ListenPort clash
        if (a.Interface.ListenPort is { } pa && b.Interface.ListenPort is { } pb && pa == pb)
            outList.Add(new(FindingSeverity.Error, "ListenPortClash",
                $"Same ListenPort {pa} on “{a.Name}” and “{b.Name}”",
                new[] { a.Name, b.Name }));

        // 6. DNS: global clash (by effective mode — split tunnels don't take global DNS)
        var aGlobal = a.EffectiveDnsMode().Kind == DnsModeKind.Global && a.Interface.Dns.Count > 0;
        var bGlobal = b.EffectiveDnsMode().Kind == DnsModeKind.Global && b.Interface.Dns.Count > 0;
        if (aGlobal && bGlobal)
            outList.Add(new(FindingSeverity.Error, "GlobalDNSClash",
                $"“{a.Name}” and “{b.Name}” both want to be the system's global DNS",
                new[] { a.Name, b.Name },
                "Switch one tunnel to split DNS (by domain) in its settings"));

        // 7. Split domain overlap
        if (a.Options.DnsMode.Kind == DnsModeKind.Split && b.Options.DnsMode.Kind == DnsModeKind.Split)
            foreach (var x in a.Options.DnsMode.MatchDomains)
            foreach (var y in b.Options.DnsMode.MatchDomains)
                if (DomainOverlap(x, y))
                    outList.Add(new(FindingSeverity.Warning, "SplitDomainOverlap",
                        $"DNS domains overlap: {x} (“{a.Name}”) and {y} (“{b.Name}”)",
                        new[] { a.Name, b.Name }));

        // 8. Endpoint inside another tunnel
        var pairs = new[] { (Src: a, DstRoutes: rb, DstName: b.Name), (Src: b, DstRoutes: ra, DstName: a.Name) };
        foreach (var (src, dstRoutes, dstName) in pairs)
            foreach (var p in src.Peers)
            {
                if (p.Endpoint is null || EndpointUtil.Split(p.Endpoint) is not { } split) continue;
                var host = split.Host;
                if (!EndpointUtil.IsIpLiteral(host)) continue;
                if (dstRoutes.Any(r => r.ContainsAddress(host)))
                    outList.Add(new(FindingSeverity.Error, "EndpointInsideTunnel",
                        $"Endpoint {host} of “{src.Name}” falls inside “{dstName}” routes — possible loop / black hole",
                        new[] { src.Name, dstName },
                        "TunHub pins the endpoint via the physical gateway on start; verify this is expected"));
            }

        return outList;
    }

    private static bool DomainOverlap(string a, string b)
    {
        var x = a.ToLowerInvariant().Trim('.');
        var y = b.ToLowerInvariant().Trim('.');
        return x == y || x.EndsWith("." + y) || y.EndsWith("." + x);
    }

    public static bool HasErrors(IEnumerable<ConflictFinding> findings) =>
        findings.Any(f => f.Severity == FindingSeverity.Error);
}
