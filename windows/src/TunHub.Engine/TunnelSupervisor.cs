using System.Diagnostics;
using TunHub.Core;
using TunHub.Engine.Platform;
using TunHub.Engine.Uapi;

namespace TunHub.Engine;

internal sealed class RunningTunnel
{
    public required ResolvedTunnelSpec Spec { get; init; }
    public required Process Process { get; init; }
    public required string Interface { get; init; }
    public required TunnelKind Kind { get; init; }
    public IReadOnlyDictionary<int, string> Endpoints { get; init; } = new Dictionary<int, string>();
    public TunnelPhase Phase { get; set; } = TunnelPhase.Starting;
    public List<PeerRuntime> Peers { get; set; } = new();
    public DateTimeOffset Since { get; set; } = DateTimeOffset.Now;
    public string? LastError { get; set; }
    public bool IntentionalStop { get; set; }
}

/// <summary>Core-process lifecycle + route/DNS/kill-switch orchestration (platform-abstracted).</summary>
public sealed class TunnelSupervisor
{
    private readonly ITunnelPlatform _platform;
    private readonly FileLog _log;
    private readonly object _gate = new();
    private readonly Dictionary<Guid, RunningTunnel> _running = new();
    private readonly Dictionary<Guid, (string Name, DateTimeOffset Since)> _starting = new();
    private readonly Dictionary<Guid, string> _stopping = new();
    private readonly Dictionary<Guid, TunnelRuntimeState> _failed = new();
    private Timer? _statsTimer;
    public bool KillSwitchEnabled { get; private set; } = true;

    public TunnelSupervisor(ITunnelPlatform platform, FileLog log)
    {
        _platform = platform;
        _log = log;
    }

    public void StartStatsLoop() =>
        _statsTimer = new Timer(_ => PollStats(), null, TimeSpan.FromMilliseconds(500), TimeSpan.FromMilliseconds(500));

    // MARK: Start

    public void Start(ResolvedTunnelSpec spec)
    {
        lock (_gate)
        {
            if (_running.ContainsKey(spec.Id)) throw new Exception("tunnel already running");
            if (_starting.ContainsKey(spec.Id)) throw new Exception("tunnel already starting");
            _starting[spec.Id] = (spec.Name, DateTimeOffset.Now);
            _failed.Remove(spec.Id);
        }
        _log.Info("start", $"accepted request “{spec.Name}” → background");
        Task.Run(() =>
        {
            try { StartCore(spec); }
            catch (Exception ex)
            {
                lock (_gate)
                {
                    _starting.Remove(spec.Id);
                    _failed[spec.Id] = new TunnelRuntimeState
                    {
                        Id = spec.Id, Name = spec.Name, Phase = TunnelPhase.Failed, ErrorMessage = ex.Message
                    };
                }
                _log.Error("start", $"✕ FAIL “{spec.Name}”: {ex.Message}");
            }
        });
    }

    private void StartCore(ResolvedTunnelSpec spec)
    {
        var t0 = DateTimeOffset.Now;
        _log.Info("start", $"▶ START “{spec.Name}” [{spec.Kind}] peers={spec.Peers.Count} routes={spec.Routes.Count}");

        var coreName = spec.Kind.CoreBinary();
        var coreExe = _platform.LocateCore(coreName) ?? throw new Exception($"core binary not found: {coreName}");

        var endpoints = ConfigRenderer.ResolveEndpoints(spec);
        var launch = _platform.BuildCoreLaunch(spec, coreExe, spec.Id);

        var psi = new ProcessStartInfo
        {
            FileName = launch.ExePath,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var a in launch.Args) psi.ArgumentList.Add(a);
        foreach (var (k, v) in launch.Environment) psi.Environment[k] = v;

        var process = Process.Start(psi) ?? throw new Exception("failed to launch core");
        process.ErrorDataReceived += (_, e) => { if (e.Data is { Length: > 0 }) _log.Debug($"core:{spec.Name}", e.Data); };
        process.OutputDataReceived += (_, e) => { if (e.Data is { Length: > 0 }) _log.Debug($"core:{spec.Name}", e.Data); };
        process.BeginErrorReadLine();
        process.BeginOutputReadLine();

        var iface = _platform.WaitForInterface(launch, process, TimeSpan.FromSeconds(8))
                    ?? throw new Exception("core did not create an interface (timeout)");
        _log.Info("start", $"interface: {iface}");

        var rt = new RunningTunnel
        {
            Spec = spec, Process = process, Interface = iface, Kind = spec.Kind, Endpoints = endpoints
        };
        lock (_gate) { _starting.Remove(spec.Id); _running[spec.Id] = rt; }

        try
        {
            using (var uapi = _platform.ConnectUapi(iface, spec.Kind, TimeSpan.FromSeconds(8)))
            {
                var config = ConfigRenderer.UapiSet(spec, endpoints);
                UapiClient.Set(uapi, config);
            }
            _platform.ConfigureInterface(iface, spec);
            _platform.ApplyRoutes(spec, iface, endpoints);
            _platform.ApplyDns(spec, iface);
            lock (_gate) { rt.Phase = TunnelPhase.Up; rt.Since = DateTimeOffset.Now; RebuildKillSwitchLocked(); }
            PersistOwnership();
            _log.Info("start", $"✔ UP “{spec.Name}” on {iface} in {(DateTimeOffset.Now - t0).TotalMilliseconds:0}ms");
        }
        catch (Exception ex)
        {
            _log.Error("start", $"✕ FAIL “{spec.Name}”: {ex.Message} — rolling back");
            Teardown(rt, killProcess: true);
            lock (_gate) _running.Remove(spec.Id);
            throw;
        }
    }

    // MARK: Stop

    public void Stop(Guid id)
    {
        RunningTunnel? rt;
        lock (_gate)
        {
            if (!_running.TryGetValue(id, out rt))
            {
                _failed.Remove(id);
                return;
            }
            rt.IntentionalStop = true;
            rt.Phase = TunnelPhase.Stopping;
            _running.Remove(id);
            _stopping[id] = rt.Spec.Name;
        }
        _log.Info("stop", $"■ STOP “{rt!.Spec.Name}” on {rt.Interface} → rollback in background");
        Task.Run(() =>
        {
            Teardown(rt, killProcess: true);
            lock (_gate) { _stopping.Remove(id); RebuildKillSwitchLocked(); }
            PersistOwnership();
            _log.Info("stop", $"✔ STOPPED “{rt.Spec.Name}”");
        });
    }

    public void StopAll()
    {
        List<RunningTunnel> all;
        lock (_gate) { all = _running.Values.ToList(); _running.Clear(); }
        foreach (var rt in all) { rt.IntentionalStop = true; Teardown(rt, killProcess: true); }
        lock (_gate) RebuildKillSwitchLocked();
        PersistOwnership();
        _log.Info("stop", "✔ all tunnels stopped");
    }

    private void Teardown(RunningTunnel rt, bool killProcess)
    {
        try { _platform.RollbackDns(rt.Spec.Id, rt.Interface); } catch (Exception ex) { _log.Warn("stop", $"dns rollback: {ex.Message}"); }
        try { _platform.RollbackRoutes(rt.Spec.Id, rt.Interface); } catch (Exception ex) { _log.Warn("stop", $"route rollback: {ex.Message}"); }
        if (killProcess && !rt.Process.HasExited)
        {
            try
            {
                rt.Process.Kill(entireProcessTree: true);
                rt.Process.WaitForExit(3000);
            }
            catch { /* already gone */ }
        }
    }

    // MARK: Kill switch

    public void SetKillSwitchEnabled(bool enabled)
    {
        lock (_gate) { KillSwitchEnabled = enabled; RebuildKillSwitchLocked(); }
    }

    private void RebuildKillSwitchLocked()
    {
        var active = _running.Values
            .Where(r => r.Spec.KillSwitch)
            .Select(r =>
            {
                var eps = r.Endpoints.Values
                    .Select(EndpointUtil.Split)
                    .Where(s => s is not null)
                    .Select(s => (s!.Value.Host, s.Value.Port))
                    .ToList();
                return new ActiveTunnel(r.Interface, eps);
            })
            .ToList();
        try { _platform.RebuildKillSwitch(active, KillSwitchEnabled); }
        catch (Exception ex) { _log.Error("firewall", ex.Message); }
    }

    // MARK: Stats

    private void PollStats()
    {
        List<RunningTunnel> snapshot;
        lock (_gate) snapshot = _running.Values.Where(r => r.Phase is TunnelPhase.Up or TunnelPhase.Degraded).ToList();

        foreach (var rt in snapshot)
        {
            try
            {
                using var uapi = _platform.ConnectUapi(rt.Interface, rt.Kind, TimeSpan.FromSeconds(2));
                var peers = UapiClient.Get(uapi);
                DateTimeOffset? lastHs = null;
                foreach (var p in peers)
                    if (p.LastHandshake is { } hs && (lastHs is null || hs > lastHs.Value)) lastHs = hs;
                var fresh = lastHs is { } h && (DateTimeOffset.Now - h).TotalSeconds < 185;
                var neverShook = peers.All(p => p.LastHandshake is null);
                var young = (DateTimeOffset.Now - rt.Since).TotalSeconds < 30;
                lock (_gate)
                {
                    rt.Peers = peers;
                    rt.Phase = (fresh || (neverShook && young)) ? TunnelPhase.Up : TunnelPhase.Degraded;
                }
            }
            catch (Exception ex) { _log.Warn("stats", $"“{rt.Spec.Name}”: UAPI get failed: {ex.Message}"); }
        }
    }

    // MARK: State

    public List<TunnelRuntimeState> States()
    {
        lock (_gate)
        {
            var outList = _running.Values.Select(rt => new TunnelRuntimeState
            {
                Id = rt.Spec.Id, Name = rt.Spec.Name, Phase = rt.Phase,
                UtunName = rt.Interface, ErrorMessage = rt.LastError, Peers = rt.Peers, Since = rt.Since
            }).ToList();
            foreach (var (id, info) in _starting)
                if (!_running.ContainsKey(id))
                    outList.Add(new TunnelRuntimeState { Id = id, Name = info.Name, Phase = TunnelPhase.Starting, Since = info.Since });
            foreach (var (id, name) in _stopping)
                if (!_running.ContainsKey(id))
                    outList.Add(new TunnelRuntimeState { Id = id, Name = name, Phase = TunnelPhase.Stopping });
            outList.AddRange(_failed.Values);
            return outList;
        }
    }

    private void PersistOwnership()
    {
        try
        {
            List<object> owned;
            lock (_gate)
                owned = _running.Values.Select(r => (object)new
                {
                    tunnelId = r.Spec.Id, name = r.Spec.Name, iface = r.Interface,
                    pid = r.Process.Id, core = r.Kind.CoreBinary()
                }).ToList();
            File.WriteAllText(PlatformPaths.OwnershipFile, TunJson.Encode(new { owned }));
        }
        catch { /* best effort */ }
    }
}
