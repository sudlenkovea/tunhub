using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.OpenVpn;

/// <summary>
/// Lifecycle of a single OpenVPN tunnel: spawns <c>openvpn(.exe)</c> driven entirely through
/// its management interface (TCP loopback, password-protected). Credentials/OTP are supplied
/// over the management channel — never written to the config or to disk. Server-pushed routes
/// and the connected remote are parsed from the management log stream for display.
///
/// Mirrors the macOS <c>OpenVPNSupervisor</c> + <c>OpenVPNManagement</c>.
/// </summary>
public sealed class OpenVpnSession : IDisposable
{
    private readonly ResolvedTunnelSpec _spec;
    private readonly string _coreExe;
    private readonly FileLog _log;
    private readonly object _gate = new();

    private Process? _process;
    private OpenVpnManagement? _mgmt;
    private string? _configPath;
    private string? _pwPath;
    private readonly HashSet<string> _answeredRealms = new();

    public Guid Id => _spec.Id;
    public string Name => _spec.Name;
    public TunnelPhase Phase { get; private set; } = TunnelPhase.Starting;
    public string? LastError { get; private set; }
    public DateTimeOffset Since { get; private set; } = DateTimeOffset.Now;
    public bool IntentionalStop { get; set; }
    public ulong RxBytes { get; private set; }
    public ulong TxBytes { get; private set; }
    public string? ConnectedRemote { get; private set; }
    public List<string> PushedRoutes { get; } = new();

    public OpenVpnSession(ResolvedTunnelSpec spec, string coreExe, FileLog log)
    {
        _spec = spec; _coreExe = coreExe; _log = log;
    }

    // MARK: - Start

    public void Start()
    {
        var ovpn = _spec.OpenVpn ?? throw new Exception("openvpn payload missing from spec");

        _configPath = Path.Combine(PlatformPaths.TempDir, $"tunhub-{_spec.Id:N}.ovpn");
        File.WriteAllText(_configPath, ovpn.ConfigText);
        HardenFile(_configPath);

        // One-time management password (protects the loopback control socket).
        var pw = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
        _pwPath = Path.Combine(PlatformPaths.TempDir, $"tunhub-{_spec.Id:N}.mgmt");
        File.WriteAllText(_pwPath, pw + "\n");
        HardenFile(_pwPath);

        var port = FreeTcpPort();

        var args = new List<string>
        {
            "--config", _configPath,
            "--management", "127.0.0.1", port.ToString(), _pwPath,
            "--management-hold",
            "--management-query-passwords",
            "--auth-nocache",
            "--auth-retry", "none",
            "--connect-retry-max", "3",
            "--script-security", "1",   // allow openvpn's OWN ifconfig/route helpers, no user scripts
            "--verb", "4",
            "--mute", "0"
        };
        if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            args.AddRange(new[] { "--windows-driver", "wintun" });

        var psi = new ProcessStartInfo
        {
            FileName = _coreExe,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        foreach (var a in args) psi.ArgumentList.Add(a);

        _process = Process.Start(psi) ?? throw new Exception("failed to launch openvpn");
        _process.ErrorDataReceived += (_, e) => { if (e.Data is { Length: > 0 }) OnCoreLog(e.Data); };
        _process.OutputDataReceived += (_, e) => { if (e.Data is { Length: > 0 }) OnCoreLog(e.Data); };
        _process.BeginErrorReadLine();
        _process.BeginOutputReadLine();

        _log.Info($"ovpn:{Name}", $"▶ openvpn pid={_process.Id}, management 127.0.0.1:{port}");

        _mgmt = new OpenVpnManagement("127.0.0.1", port, pw, _log, Name);
        _mgmt.StateChanged += OnState;
        _mgmt.Bytecount += OnBytecount;
        _mgmt.PasswordRequested += OnPasswordRequested;
        _mgmt.LogLine += OnCoreLog;
        _mgmt.Fatal += msg => Fail($"fatal: {msg}");
        _mgmt.Connect(TimeSpan.FromSeconds(8));
        _mgmt.EnableNotifications();
        _mgmt.HoldRelease();
    }

    // MARK: - Management events

    private void OnPasswordRequested(string realm)
    {
        var ovpn = _spec.OpenVpn!;
        lock (_gate)
        {
            if (!_answeredRealms.Add(realm)) return;  // answer each realm once (auth-retry none)
        }
        _log.Info($"ovpn:{Name}", $"credentials requested for '{realm}' — sending " +
            $"user={(string.IsNullOrEmpty(ovpn.Username) ? "<empty>" : "set")}, " +
            $"pass={(string.IsNullOrEmpty(ovpn.Password) ? "<empty>" : "set")}, " +
            $"otp={(string.IsNullOrEmpty(ovpn.Otp) ? "<none>" : "set")}");
        _mgmt!.SendCredentials(realm, ovpn.Username, ovpn.Password, ovpn.Otp);
    }

    private void OnState(string state)
    {
        _log.Debug($"ovpn:{Name}", $"state: {state}");
        lock (_gate)
        {
            switch (state.ToUpperInvariant())
            {
                case "CONNECTED":
                    Phase = TunnelPhase.Up; Since = DateTimeOffset.Now; LastError = null;
                    break;
                case "AUTH_FAILED":
                    Phase = TunnelPhase.Failed;
                    LastError = "authentication failed";
                    break;
                case "RECONNECTING":
                    Phase = TunnelPhase.Degraded;
                    break;
                case "EXITING":
                    if (Phase != TunnelPhase.Failed)
                        Phase = IntentionalStop ? TunnelPhase.Stopping : TunnelPhase.Failed;
                    break;
                case "CONNECTING":
                case "WAIT":
                case "AUTH":
                case "GET_CONFIG":
                case "ASSIGN_IP":
                case "ADD_ROUTES":
                    if (Phase is not (TunnelPhase.Up or TunnelPhase.Failed)) Phase = TunnelPhase.Starting;
                    break;
            }
        }
    }

    private void OnBytecount(ulong inb, ulong outb)
    {
        lock (_gate) { RxBytes = inb; TxBytes = outb; }
    }

    // Parse OpenVPN log lines for the connected remote, pushed routes and auth failures.
    private void OnCoreLog(string line)
    {
        if (line.Contains("AUTH_FAILED", StringComparison.OrdinalIgnoreCase) ||
            line.Contains("verification failed", StringComparison.OrdinalIgnoreCase))
        {
            Fail("authentication failed");
            return;
        }

        // "Peer Connection Initiated with [AF_INET]1.2.3.4:1194"
        var afIdx = line.IndexOf("[AF_INET]", StringComparison.Ordinal);
        if (afIdx >= 0)
        {
            var rest = line.Substring(afIdx + "[AF_INET]".Length).Trim();
            var end = rest.IndexOf(' ');
            var ep = end > 0 ? rest.Substring(0, end) : rest;
            lock (_gate) ConnectedRemote = ep.Trim();
        }

        // PUSH_REPLY routes: "PUSH: Received control message: 'PUSH_REPLY,route 10.0.0.0 255.0.0.0,...'"
        var pr = line.IndexOf("PUSH_REPLY", StringComparison.Ordinal);
        if (pr >= 0)
        {
            var payload = line.Substring(pr);
            foreach (var part in payload.Split(','))
            {
                var toks = part.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (toks.Length >= 3 && toks[0] == "route")
                {
                    var cidr = MaskToCidr(toks[1], toks[2]);
                    if (cidr is not null) AddRoute(cidr);
                }
                else if (toks.Length >= 2 && toks[0] == "route-ipv6")
                {
                    AddRoute(toks[1]);
                }
                else if (toks.Length >= 1 && toks[0] == "redirect-gateway")
                {
                    AddRoute("0.0.0.0/0");
                }
            }
        }
    }

    private void AddRoute(string cidr)
    {
        lock (_gate) { if (!PushedRoutes.Contains(cidr)) PushedRoutes.Add(cidr); }
    }

    private void Fail(string reason)
    {
        lock (_gate) { Phase = TunnelPhase.Failed; LastError = reason; }
        _log.Error($"ovpn:{Name}", $"✕ {reason}");
    }

    // MARK: - Stop

    public void Stop()
    {
        IntentionalStop = true;
        lock (_gate) if (Phase is not TunnelPhase.Failed) Phase = TunnelPhase.Stopping;
        try { _mgmt?.SignalTerm(); } catch { }
        try { if (_process is { HasExited: false }) { _process.WaitForExit(2500); } } catch { }
        try { if (_process is { HasExited: false }) _process.Kill(entireProcessTree: true); } catch { }
        Cleanup();
        _log.Info($"ovpn:{Name}", "✔ stopped");
    }

    private void Cleanup()
    {
        try { _mgmt?.Dispose(); } catch { }
        try { if (_configPath is not null) File.Delete(_configPath); } catch { }
        try { if (_pwPath is not null) File.Delete(_pwPath); } catch { }
    }

    public TunnelRuntimeState Snapshot()
    {
        lock (_gate)
        {
            var peer = new PeerRuntime
            {
                PublicKey = ConnectedRemote ?? "",
                Endpoint = ConnectedRemote,
                RxBytes = RxBytes,
                TxBytes = TxBytes,
                LastHandshake = Phase == TunnelPhase.Up ? Since : null
            };
            return new TunnelRuntimeState
            {
                Id = _spec.Id,
                Name = _spec.Name,
                Phase = Phase,
                UtunName = "openvpn",
                ErrorMessage = LastError,
                Peers = new List<PeerRuntime> { peer },
                Since = Since,
                Routes = PushedRoutes.Count > 0 ? PushedRoutes.ToList() : null
            };
        }
    }

    public void Dispose() => Cleanup();

    // MARK: - helpers

    private static int FreeTcpPort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        var port = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return port;
    }

    private static void HardenFile(string path)
    {
        // Best-effort: on POSIX restrict to owner; on Windows the temp dir under the service
        // profile is already ACL-restricted to SYSTEM/Administrators.
        try
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite);
        }
        catch { }
    }

    private static string? MaskToCidr(string net, string mask)
    {
        if (!IPAddress.TryParse(mask, out var m)) return null;
        var bytes = m.GetAddressBytes();
        int prefix = 0;
        foreach (var b in bytes)
        {
            var v = b;
            while (v != 0) { prefix += v & 1; v >>= 1; }
        }
        return $"{net}/{prefix}";
    }
}
