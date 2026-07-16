using TunHub.Core;
using TunHub.Engine.Ipc;
using TunHub.Engine.Platform;

namespace TunHub.App.Services;

/// <summary>App-side wrapper over the IPC client to the privileged helper.</summary>
public sealed class DaemonClient
{
    private readonly IpcClient _ipc = new(PlatformPaths.IpcSocket);

    public Task<bool> PingAsync() => _ipc.PingAsync();

    public async Task<string?> VersionAsync()
    {
        try { return await _ipc.CallAsync(IpcMethod.Version); }
        catch { return null; }
    }

    public Task StartTunnelAsync(ResolvedTunnelSpec spec) => _ipc.CallAsync(IpcMethod.StartTunnel, spec);

    public Task StopTunnelAsync(Guid id) =>
        _ipc.CallAsync(IpcMethod.StopTunnel, new StopTunnelPayload { Id = id });

    public Task StopAllAsync() => _ipc.CallAsync(IpcMethod.StopAll);

    public Task SetKillSwitchAsync(bool enabled) =>
        _ipc.CallAsync(IpcMethod.SetKillSwitch, new SetKillSwitchPayload { Enabled = enabled });

    public async Task<List<TunnelRuntimeState>> RuntimeStatesAsync()
    {
        try
        {
            var payload = await _ipc.CallAsync(IpcMethod.RuntimeStates);
            return payload is null ? new() : TunJson.Decode<List<TunnelRuntimeState>>(payload) ?? new();
        }
        catch { return new(); }
    }
}
