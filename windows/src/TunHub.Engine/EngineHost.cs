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
        _supervisor.StartStatsLoop();
        _server = new IpcServer(PlatformPaths.IpcSocket, Handle);
        _server.Start();
        _log.Info("daemon", $"IPC listening at {PlatformPaths.IpcSocket}");
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
