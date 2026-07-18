using System.Net;
using System.Net.Sockets;
using System.Text;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.OpenVpn;

/// <summary>
/// Client for OpenVPN's management interface. On Windows the interface is exposed over a
/// TCP loopback socket (AF_UNIX management is a macOS/Linux nicety; TCP-on-loopback is the
/// portable choice and is protected by a one-time password file). The client drives the
/// connection: releases the initial hold, enables state/bytecount/log notifications, answers
/// password/challenge prompts, and issues the shutdown signal.
///
/// Static challenge is encoded as <c>SCRV1:base64(password):base64(otp)</c>; a plain
/// user/pass profile sends the password verbatim.
/// </summary>
public sealed class OpenVpnManagement : IDisposable
{
    private readonly string _host;
    private readonly int _port;
    private readonly string? _password;      // management password (first line the client sends)
    private readonly FileLog _log;
    private readonly string _tag;
    private TcpClient? _client;
    private NetworkStream? _stream;
    private StreamWriter? _writer;
    private Thread? _reader;
    private volatile bool _closed;

    // Events surfaced to the session.
    public event Action<string>? StateChanged;                 // e.g. "CONNECTED", "RECONNECTING", "AUTH"
    public event Action<ulong, ulong>? Bytecount;              // (bytesIn, bytesOut)
    public event Action<string>? PasswordRequested;            // realm, e.g. "Auth" / "Private Key"
    public event Action<string>? LogLine;                      // raw log lines (PUSH_REPLY etc.)
    public event Action<string>? Fatal;                        // >FATAL: message

    public OpenVpnManagement(string host, int port, string? password, FileLog log, string tag)
    {
        _host = host; _port = port; _password = password; _log = log; _tag = tag;
    }

    public void Connect(TimeSpan timeout)
    {
        var deadline = DateTimeOffset.Now + timeout;
        Exception? last = null;
        while (DateTimeOffset.Now < deadline)
        {
            try
            {
                _client = new TcpClient();
                _client.Connect(IPAddress.Loopback, _port);
                _stream = _client.GetStream();
                _writer = new StreamWriter(_stream, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
                if (_password is { Length: > 0 }) _writer.WriteLine(_password);
                _reader = new Thread(ReadLoop) { IsBackground = true, Name = $"ovpn-mgmt:{_tag}" };
                _reader.Start();
                return;
            }
            catch (Exception ex) { last = ex; Thread.Sleep(150); }
        }
        throw new Exception($"management connect failed: {last?.Message}");
    }

    // MARK: - Commands

    public void EnableNotifications()
    {
        Send("state on");
        Send("log on all");
        Send("bytecount 2");
    }

    public void HoldRelease() => Send("hold release");

    /// <summary>Answer a >PASSWORD request. Realm is usually "Auth"; static-challenge is encoded
    /// into the password field as SCRV1.</summary>
    public void SendCredentials(string realm, string? username, string? password, string? otp)
    {
        // OpenVPN's management interface expects BOTH a username and a password reply for the
        // realm; sending only one leaves it blocked forever ("could not read … from management").
        Send($"username \"{realm}\" {Escape(username ?? "")}");
        var pw = password ?? "";
        if (otp is { Length: > 0 })
        {
            var b64p = Convert.ToBase64String(Encoding.UTF8.GetBytes(pw));
            var b64o = Convert.ToBase64String(Encoding.UTF8.GetBytes(otp));
            pw = $"SCRV1:{b64p}:{b64o}";
        }
        Send($"password \"{realm}\" {Escape(pw)}");
    }

    public void SignalTerm() => Send("signal SIGTERM");

    private void Send(string cmd)
    {
        try { _writer?.WriteLine(cmd); }
        catch (Exception ex) { _log.Warn($"ovpn:{_tag}", $"mgmt send failed: {ex.Message}"); }
    }

    private static string Escape(string s) => s.Replace("\\", "\\\\").Replace("\"", "\\\"");

    // MARK: - Read loop / notification parsing

    private void ReadLoop()
    {
        try
        {
            using var sr = new StreamReader(_stream!, Encoding.UTF8);
            string? line;
            while (!_closed && (line = sr.ReadLine()) is not null)
                Dispatch(line);
        }
        catch (Exception ex) { if (!_closed) _log.Warn($"ovpn:{_tag}", $"mgmt read ended: {ex.Message}"); }
    }

    private void Dispatch(string line)
    {
        // Real-time notifications are prefixed with '>'.
        if (line.StartsWith(">STATE:"))
        {
            // >STATE:<time>,<state>,<detail>,<localIP>,<remoteIP>,<remotePort>,...
            var parts = line.Substring(7).Split(',');
            if (parts.Length >= 2) StateChanged?.Invoke(parts[1]);
        }
        else if (line.StartsWith(">BYTECOUNT:"))
        {
            var parts = line.Substring(11).Split(',');
            if (parts.Length >= 2 &&
                ulong.TryParse(parts[0], out var inb) && ulong.TryParse(parts[1], out var outb))
                Bytecount?.Invoke(inb, outb);
        }
        else if (line.StartsWith(">PASSWORD:"))
        {
            // >PASSWORD:Need 'Auth' username/password  OR  >PASSWORD:Verification Failed: 'Auth'
            var body = line.Substring(10);
            if (body.StartsWith("Verification Failed", StringComparison.OrdinalIgnoreCase))
                StateChanged?.Invoke("AUTH_FAILED");
            else
            {
                var realm = ExtractQuoted(body) ?? "Auth";
                PasswordRequested?.Invoke(realm);
            }
        }
        else if (line.StartsWith(">LOG:"))
        {
            // >LOG:<time>,<flags>,<message>
            var body = line.Substring(5);
            var idx = body.IndexOf(',');
            var idx2 = idx >= 0 ? body.IndexOf(',', idx + 1) : -1;
            var msg = idx2 >= 0 ? body.Substring(idx2 + 1) : body;
            LogLine?.Invoke(msg);
        }
        else if (line.StartsWith(">FATAL:"))
        {
            Fatal?.Invoke(line.Substring(7));
        }
        else if (line.StartsWith(">INFO:") || line.StartsWith(">HOLD:") || line.StartsWith("SUCCESS:"))
        {
            // informational; ignore
        }
    }

    private static string? ExtractQuoted(string s)
    {
        var a = s.IndexOf('\'');
        if (a < 0) return null;
        var b = s.IndexOf('\'', a + 1);
        return b > a ? s.Substring(a + 1, b - a - 1) : null;
    }

    public void Dispose()
    {
        _closed = true;
        try { _writer?.Flush(); } catch { }
        try { _stream?.Dispose(); } catch { }
        try { _client?.Dispose(); } catch { }
    }
}
