using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using TunHub.Core;

namespace TunHub.Engine.Ipc;

/// <summary>
/// Newline-delimited JSON over a Unix domain socket. AF_UNIX is supported on both
/// macOS and Windows 10+, so a single transport serves both platforms.
/// </summary>
public static class IpcJson
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull
    };
}

public sealed class IpcServer : IDisposable
{
    private readonly string _socketPath;
    private readonly Func<IpcRequest, Task<IpcResponse>> _handler;
    private Socket? _listener;
    private CancellationTokenSource? _cts;

    public IpcServer(string socketPath, Func<IpcRequest, Task<IpcResponse>> handler)
    {
        _socketPath = socketPath;
        _handler = handler;
    }

    public void Start()
    {
        if (File.Exists(_socketPath)) File.Delete(_socketPath);
        Directory.CreateDirectory(Path.GetDirectoryName(_socketPath)!);

        _listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        _listener.Bind(new UnixDomainSocketEndPoint(_socketPath));
        _listener.Listen(16);
        // The helper runs elevated (root/SYSTEM); allow the user's UI process to connect.
        // TODO(Windows): tighten to an explicit ACL instead of world-accessible.
        if (!OperatingSystem.IsWindows())
        {
            try { File.SetUnixFileMode(_socketPath,
                UnixFileMode.UserRead | UnixFileMode.UserWrite |
                UnixFileMode.GroupRead | UnixFileMode.GroupWrite |
                UnixFileMode.OtherRead | UnixFileMode.OtherWrite); }
            catch { /* best effort */ }
        }
        _cts = new CancellationTokenSource();
        _ = AcceptLoopAsync(_cts.Token);
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            Socket conn;
            try { conn = await _listener!.AcceptAsync(ct); }
            catch (OperationCanceledException) { break; }
            catch { continue; }
            _ = HandleConnectionAsync(conn);
        }
    }

    private async Task HandleConnectionAsync(Socket conn)
    {
        try
        {
            await using var stream = new NetworkStream(conn, ownsSocket: true);
            using var reader = new StreamReader(stream, Encoding.UTF8, false, 1 << 16, leaveOpen: true);
            await using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

            var line = await reader.ReadLineAsync();
            if (line is null) return;

            IpcResponse response;
            try
            {
                var req = JsonSerializer.Deserialize<IpcRequest>(line, IpcJson.Options)
                          ?? throw new Exception("empty request");
                response = await _handler(req);
            }
            catch (Exception ex)
            {
                response = IpcResponse.Fail(ex.Message);
            }
            await writer.WriteLineAsync(JsonSerializer.Serialize(response, IpcJson.Options));
        }
        catch { /* connection dropped */ }
    }

    public void Dispose()
    {
        _cts?.Cancel();
        _listener?.Dispose();
        try { if (File.Exists(_socketPath)) File.Delete(_socketPath); } catch { }
    }
}

public sealed class IpcClient
{
    private readonly string _socketPath;
    private readonly TimeSpan _timeout;

    public IpcClient(string socketPath, TimeSpan? timeout = null)
    {
        _socketPath = socketPath;
        _timeout = timeout ?? TimeSpan.FromSeconds(20);
    }

    /// <summary>Send one request, await one response. Returns null on transport failure.</summary>
    public async Task<IpcResponse?> SendAsync(IpcRequest request)
    {
        using var cts = new CancellationTokenSource(_timeout);
        try
        {
            using var sock = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            await sock.ConnectAsync(new UnixDomainSocketEndPoint(_socketPath), cts.Token);
            await using var stream = new NetworkStream(sock, ownsSocket: true);
            using var reader = new StreamReader(stream, Encoding.UTF8, false, 1 << 16, leaveOpen: true);
            await using var writer = new StreamWriter(stream, new UTF8Encoding(false)) { AutoFlush = true };

            await writer.WriteLineAsync(JsonSerializer.Serialize(request, IpcJson.Options));
            var line = await reader.ReadLineAsync(cts.Token);
            return line is null ? null : JsonSerializer.Deserialize<IpcResponse>(line, IpcJson.Options);
        }
        catch
        {
            return null;
        }
    }

    // Convenience wrappers used by the app.

    public async Task<string?> CallAsync(string method, object? payload = null)
    {
        var req = new IpcRequest
        {
            Method = method,
            Payload = payload is null ? null : JsonSerializer.Serialize(payload, TunJson.Compact)
        };
        var resp = await SendAsync(req);
        if (resp is null) return null;
        return resp.Ok ? resp.Payload ?? "" : throw new IpcException(resp.Error ?? "unknown error");
    }

    public async Task<bool> PingAsync()
    {
        var resp = await SendAsync(new IpcRequest { Method = IpcMethod.Version });
        return resp is { Ok: true };
    }
}

public sealed class IpcException : Exception
{
    public IpcException(string message) : base(message) { }
}
