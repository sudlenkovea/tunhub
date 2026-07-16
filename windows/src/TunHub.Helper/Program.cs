using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.DependencyInjection;
using TunHub.Engine;
using TunHub.Engine.Platform;
using TunHub.Engine.Platforms;

// The privileged helper. Runs as a Windows Service (SYSTEM) or a macOS root LaunchDaemon.
// Hosts the IPC server and the tunnel supervisor.

var builder = Host.CreateApplicationBuilder(args);
builder.Services.AddWindowsService(o => o.ServiceName = "TunHubHelper");
builder.Services.AddHostedService<HelperService>();

var host = builder.Build();
host.Run();

internal sealed class HelperService : BackgroundService
{
    private EngineHost? _engine;

    protected override Task ExecuteAsync(CancellationToken stoppingToken)
    {
        PlatformPaths.EnsureDirectories();
        var log = new FileLog(PlatformPaths.LogFile);
        var platform = PlatformFactory.Create(log);
        _engine = new EngineHost(platform, log);
        _engine.Run();
        return Task.CompletedTask; // the IPC server runs on its own accept loop
    }

    public override Task StopAsync(CancellationToken cancellationToken)
    {
        _engine?.Dispose();
        return base.StopAsync(cancellationToken);
    }
}
