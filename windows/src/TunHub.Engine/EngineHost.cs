using TunHub.Core;
using TunHub.Engine.Ipc;
using TunHub.Engine.Platform;

namespace TunHub.Engine;

/// <summary>
/// The privileged process entry point: hosts the IPC server and dispatches requests
/// to the tunnel supervisor. Runs as a Windows Service / macOS root daemon.
/// </summary>
public sealed class EngineHost : IDisposable
{
    private readonly ITunnelPlatform _platform;
    private readonly FileLog _log;
    private readonly TunnelSupervisor _supervisor;
    private IpcServer? _server;

    public EngineHost(ITunnelPlatform platform, FileLog log)
    {
        _platform = platform;
        _log = log;
        _supervisor = new TunnelSupervisor(platform, log);
    }

    public void Run()
    {
        PlatformPaths.EnsureDirectories();
        _log.Info("daemon", $"═══ tunhub helper {TunHubInfo.ProtocolVersion} on {_platform.Name} (pid={System.Environment.ProcessId}) ═══");
        ReapOrphanCores();
        _supervisor.StartStatsLoop();
        _server = new IpcServer(PlatformPaths.IpcSocket, Handle);
        _server.Start();
        _log.Info("daemon", $"IPC listening at {PlatformPaths.IpcSocket}");
    }

    /// <summary>
    /// Kill core processes left behind by a previous run — after a crash, a kill, or a
    /// service/app update. They are matched by their executable living inside OUR install
    /// directory, so another product's WireGuard/OpenVPN is never touched. This runs once at
    /// startup, before anything is spawned, so every match is by definition an orphan.
    /// Killing a core tears down its adapter, which removes the stale routes it left behind —
    /// otherwise a dead tunnel's default route can hijack the next tunnel's endpoint.
    /// </summary>
    private void ReapOrphanCores()
    {
        var ourDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        var names = new[] { "amneziawg-go", "wireguard-go", "openvpn" };
        var me = System.Environment.ProcessId;
        var reaped = 0;

        foreach (var name in names)
        {
            System.Diagnostics.Process[] found;
            try { found = System.Diagnostics.Process.GetProcessesByName(name); }
            catch { continue; }

            foreach (var p in found)
            {
                try
                {
                    if (p.Id == me) continue;
                    var exe = p.MainModule?.FileName;
                    if (string.IsNullOrEmpty(exe)) continue;
                    // Only ours: the binary must sit in our install directory.
                    if (!exe.StartsWith(ourDir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;

                    _log.Warn("recover", $"reaping orphaned core pid={p.Id} ({name}) from a previous run");
                    p.Kill(entireProcessTree: true);
                    p.WaitForExit(3000);
                    reaped++;
                }
                catch { /* already gone, or not ours to inspect */ }
                finally { p.Dispose(); }
            }
        }

        if (reaped > 0) _log.Info("recover", $"reaped {reaped} orphaned core process(es)");

        // The ownership registry describes the previous run only; drop it so it can't be
        // mistaken for live state.
        try { File.Delete(PlatformPaths.OwnershipFile); } catch { }
    }

    private Task<IpcResponse> Handle(IpcRequest req)
    {
        try
        {
            switch (req.Method)
            {
                case IpcMethod.Version:
                    return Ok($"{TunHubInfo.ProtocolVersion}+{BuildStamp.Value}");

                case IpcMethod.StartTunnel:
                {
                    var spec = TunJson.Decode<ResolvedTunnelSpec>(req.Payload ?? "")
                               ?? throw new Exception("missing spec");
                    _supervisor.Start(spec);
                    return Ok();
                }
                case IpcMethod.StopTunnel:
                {
                    var p = TunJson.Decode<StopTunnelPayload>(req.Payload ?? "") ?? throw new Exception("missing id");
                    _supervisor.Stop(p.Id);
                    return Ok();
                }
                case IpcMethod.StopAll:
                    _supervisor.StopAll();
                    return Ok();

                case IpcMethod.RuntimeStates:
                    return Ok(TunJson.Encode(_supervisor.States()));

                case IpcMethod.SetKillSwitch:
                {
                    var p = TunJson.Decode<SetKillSwitchPayload>(req.Payload ?? "") ?? throw new Exception("missing flag");
                    _supervisor.SetKillSwitchEnabled(p.Enabled);
                    return Ok();
                }
                case IpcMethod.RecentLog:
                {
                    var p = TunJson.Decode<RecentLogPayload>(req.Payload ?? "") ?? new RecentLogPayload();
                    return Ok(TunJson.Encode(_log.Tail(p.MaxLines)));
                }
                case IpcMethod.SetLogMode:
                {
                    var p = TunJson.Decode<SetLogModePayload>(req.Payload ?? "") ?? throw new Exception("missing mode");
                    if (!Enum.TryParse<LogCaptureMode>(p.Mode, ignoreCase: true, out var mode))
                        throw new Exception($"unknown log mode: {p.Mode}");
                    if (!LogSettings.Write(mode)) throw new Exception("could not persist the log mode");
                    _log.Info("helper", $"log capture mode set to {mode} (applies on service restart)");
                    return Ok();
                }
                default:
                    return Task.FromResult(IpcResponse.Fail($"unknown method: {req.Method}"));
            }
        }
        catch (Exception ex)
        {
            return Task.FromResult(IpcResponse.Fail(ex.Message));
        }

        Task<IpcResponse> Ok(string? payload = null) => Task.FromResult(IpcResponse.Success(payload));
    }

    public void Dispose()
    {
        _supervisor.StopAll();
        _server?.Dispose();
    }
}

/// <summary>Build stamp injected by the build script (so UI and helper can detect a mismatch).</summary>
public static class BuildStamp
{
    // Overwritten by build-macos.sh / build-windows.ps1.
    public const string Value = "20260713144904-nogit";
}
