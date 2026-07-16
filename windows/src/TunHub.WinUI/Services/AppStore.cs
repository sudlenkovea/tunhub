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
    public TunnelSecrets? LoadSecrets(Guid id) => _secrets.Load(id);

    /// <summary>Build a resolved spec (with plaintext secrets) to send to the helper.</summary>
    public ResolvedTunnelSpec ResolveSpec(TunnelConfig cfg)
    {
        var secrets = _secrets.Load(cfg.Id)
                      ?? throw new Exception($"secrets for “{cfg.Name}” not found");

        // OpenVPN: inline the redacted secret blocks back into the config text and attach
        // credentials/OTP (mirrors the macOS resolveSpec). No WireGuard private key involved.
        if (cfg.Kind == TunnelKind.OpenVpn)
        {
            var profile = cfg.OpenVpn ?? throw new Exception("OpenVPN profile missing");
            var configText = profile.ConfigText;
            foreach (var (tag, material) in secrets.OpenVpn)
            {
                if (tag is "username" or "password" or "otp") continue;
                configText = configText.Replace($"##SECRET:{tag}##", material);
            }
            var resolvedOvpn = new ResolvedOpenVpn
            {
                ConfigText = configText,
                Username = secrets.OpenVpn.TryGetValue("username", out var u) ? u : null,
                Password = secrets.OpenVpn.TryGetValue("password", out var p2) ? p2 : null,
                Otp = secrets.OpenVpn.TryGetValue("otp", out var o) ? o : null,
                StaticChallenge = profile.StaticChallenge,
                Remotes = profile.Remotes,
                Dns = profile.Dns,
                RedirectGateway = profile.RedirectGateway
            };
            return new ResolvedTunnelSpec
            {
                Id = cfg.Id, Name = cfg.Name, Kind = cfg.Kind, PrivateKey = "",
                DnsServers = profile.Dns, DnsMode = cfg.EffectiveDnsMode(),
                KillSwitch = cfg.Options.KillSwitch, OpenVpn = resolvedOvpn
            };
        }

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

    /// <summary>Run the cross-platform conflict checker over every stored tunnel.</summary>
    public List<ConflictFinding> CheckAllConflicts() => ConflictChecker.CheckAll(LoadAll());

    public string UniqueName(string name, IReadOnlyList<TunnelConfig> existing)
    {
        var names = existing.Select(t => t.Name).ToHashSet();
        if (!names.Contains(name)) return name;
        var i = 2;
        while (names.Contains($"{name} ({i})")) i++;
        return $"{name} ({i})";
    }
}
