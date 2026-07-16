using TunHub.Core;
using TunHub.Engine.Platform;
using TunHub.Engine.Platforms;

namespace TunHub.App.Services;

/// <summary>User-side storage: tunnel configs (JSON, no secrets) + the secret store.</summary>
public sealed class AppStore
{
    private readonly string _dir;
    private readonly ISecretStore _secrets = PlatformFactory.CreateAppSecretStore();

    public AppStore()
    {
        _dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "TunHub", "tunnels");
        Directory.CreateDirectory(_dir);
    }

    public List<TunnelConfig> LoadAll()
    {
        var outList = new List<TunnelConfig>();
        foreach (var f in Directory.EnumerateFiles(_dir, "*.json"))
        {
            try
            {
                var cfg = TunJson.Decode<TunnelConfig>(File.ReadAllText(f));
                if (cfg is not null) outList.Add(cfg);
            }
            catch { /* skip malformed */ }
        }
        return outList.OrderBy(c => c.Meta.SortOrder).ThenBy(c => c.Name).ToList();
    }

    public void Save(TunnelConfig cfg) =>
        File.WriteAllText(Path.Combine(_dir, $"{cfg.Id}.json"), TunJson.Encode(cfg));

    public void Delete(TunnelConfig cfg)
    {
        var f = Path.Combine(_dir, $"{cfg.Id}.json");
        if (File.Exists(f)) File.Delete(f);
        _secrets.Delete(cfg.Id);
    }

    public void SaveSecrets(Guid id, TunnelSecrets secrets) => _secrets.Save(id, secrets);

    /// <summary>Build a resolved spec (with plaintext secrets) to send to the helper.</summary>
    public ResolvedTunnelSpec ResolveSpec(TunnelConfig cfg)
    {
        var secrets = _secrets.Load(cfg.Id)
                      ?? throw new Exception($"secrets for “{cfg.Name}” not found");
        var peers = cfg.Peers.Select(p => new ResolvedPeer
        {
            PublicKey = p.PublicKey,
            PresharedKey = secrets.Psks.TryGetValue(p.Id.ToString(), out var psk) ? psk : null,
            Endpoint = p.Endpoint,
            AllowedIPs = p.AllowedIPs,
            Keepalive = p.PersistentKeepalive
        }).ToList();

        return new ResolvedTunnelSpec
        {
            Id = cfg.Id,
            Name = cfg.Name,
            Kind = cfg.Kind,
            PrivateKey = secrets.PrivateKey,
            Addresses = cfg.Interface.Addresses,
            ListenPort = cfg.Interface.ListenPort,
            Mtu = cfg.Interface.Mtu,
            DnsServers = cfg.Interface.Dns,
            DnsSearchDomains = cfg.Interface.DnsSearchDomains,
            DnsMode = cfg.EffectiveDnsMode(),
            Routes = cfg.EffectiveRoutes(),
            Awg = cfg.Awg,
            KillSwitch = cfg.Options.KillSwitch,
            Peers = peers
        };
    }

    public string UniqueName(string name, IReadOnlyList<TunnelConfig> existing)
    {
        var names = existing.Select(t => t.Name).ToHashSet();
        if (!names.Contains(name)) return name;
        var i = 2;
        while (names.Contains($"{name} ({i})")) i++;
        return $"{name} ({i})";
    }
}
