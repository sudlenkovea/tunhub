using System.Collections.Concurrent;
using TunHub.Engine.Ipc;

namespace TunHub.Engine.Platform;

public enum LogLevel { Trace, Debug, Info, Warn, Error }

/// <summary>
/// How much detail we capture. <c>Verbose</c> is a debugging mode: it multiplies log volume
/// (every command we run, plus the tunnel core's own debug stream) and costs real CPU, so it
/// is opt-in and only takes effect after a restart.
/// </summary>
public enum LogCaptureMode { Normal, Verbose }

public static class LogCaptureModeExtensions
{
    public static LogLevel MinLevel(this LogCaptureMode m) =>
        m == LogCaptureMode.Verbose ? LogLevel.Trace : LogLevel.Info;

    /// <summary>LOG_LEVEL handed to wireguard-go / amneziawg-go.</summary>
    public static string CoreLogLevel(this LogCaptureMode m) =>
        m == LogCaptureMode.Verbose ? "verbose" : "error";

    public static string Label(this LogCaptureMode m) =>
        m == LogCaptureMode.Verbose ? "Verbose (debug)" : "Normal";
}

/// <summary>
/// Persisted capture mode, shared by the UI and the privileged helper. Read once at process
/// start and applied only on the next launch — switching verbosity live would leave a
/// half-verbose log and would require respawning every tunnel core.
/// </summary>
public static class LogSettings
{
    public static LogCaptureMode Read()
    {
        try
        {
            var s = File.ReadAllText(PlatformPaths.LogModeFile).Trim();
            return Enum.TryParse<LogCaptureMode>(s, ignoreCase: true, out var m) ? m : LogCaptureMode.Normal;
        }
        catch { return LogCaptureMode.Normal; }
    }

    public static bool Write(LogCaptureMode mode)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(PlatformPaths.LogModeFile)!);
            File.WriteAllText(PlatformPaths.LogModeFile, mode.ToString());
            return true;
        }
        catch { return false; }
    }
}

/// <summary>Thread-safe file logger + in-memory ring buffer (served to the UI over IPC).</summary>
public sealed class FileLog : IDisposable
{
    private readonly string _path;
    private readonly object _gate = new();
    private readonly ConcurrentQueue<LogLine> _ring = new();
    private const int RingCapacity = 1500;

    /// <summary>Single file, trimmed in place to the last 5 MB (no unbounded growth).</summary>
    private const long MaxBytes = 5_000_000;

    private StreamWriter? _writer;
    private long _written;

    /// <summary>
    /// Defaults to Info: Trace/Debug capture is a debugging mode that floods the file and
    /// burns CPU in both the writer and the log viewer. Opt in via <see cref="LogCaptureMode"/>.
    /// </summary>
    public LogLevel MinLevel { get; set; } = LogLevel.Info;

    /// <summary>Mirroring to the console doubles the cost of every line; debugging only.</summary>
    public bool EchoConsole { get; set; }

    public FileLog(string path)
    {
        _path = path;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        OpenWriter();
    }

    private void OpenWriter()
    {
        try
        {
            var fi = new FileInfo(_path);
            _written = fi.Exists ? fi.Length : 0;
            // Keep the handle open: File.AppendAllText re-opened and closed the file for
            // every single line, which is a syscall storm under verbose logging.
            _writer = new StreamWriter(new FileStream(_path, FileMode.Append, FileAccess.Write,
                                                      FileShare.ReadWrite))
            { AutoFlush = true };
        }
        catch { _writer = null; }
    }

    public void Log(LogLevel level, string category, string message)
    {
        if (level < MinLevel) return;

        var line = new LogLine
        {
            Time = DateTimeOffset.Now,
            Level = level.ToString().ToLowerInvariant(),
            Category = category,
            Message = message
        };
        _ring.Enqueue(line);
        while (_ring.Count > RingCapacity) _ring.TryDequeue(out _);

        var text = $"{line.Time:HH:mm:ss.fff}\t{line.Level}\t{category}\t{message}";
        if (EchoConsole) Console.Error.WriteLine(text);
        try
        {
            lock (_gate)
            {
                if (_writer is null) return;
                _writer.WriteLine(text);
                _written += text.Length + 1;
                if (_written > MaxBytes) TrimLocked();
            }
        }
        catch { /* best effort */ }
    }

    /// <summary>Keep the tail of the file (last 80% of the budget) and drop the rest.</summary>
    private void TrimLocked()
    {
        try
        {
            _writer?.Dispose();
            _writer = null;
            const long keep = MaxBytes * 4 / 5;
            byte[] tail;
            using (var fs = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                var start = Math.Max(0, fs.Length - keep);
                fs.Seek(start, SeekOrigin.Begin);
                tail = new byte[fs.Length - start];
                _ = fs.Read(tail, 0, tail.Length);
            }
            // Start on a record boundary so the first line isn't a fragment.
            var nl = Array.IndexOf(tail, (byte)'\n');
            var offset = nl >= 0 ? nl + 1 : 0;
            using (var fs = new FileStream(_path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite))
                fs.Write(tail, offset, tail.Length - offset);
        }
        catch { /* best effort */ }
        finally { OpenWriter(); }
    }

    public void Trace(string c, string m) => Log(LogLevel.Trace, c, m);
    public void Debug(string c, string m) => Log(LogLevel.Debug, c, m);
    public void Info(string c, string m) => Log(LogLevel.Info, c, m);
    public void Warn(string c, string m) => Log(LogLevel.Warn, c, m);
    public void Error(string c, string m) => Log(LogLevel.Error, c, m);

    /// <summary>
    /// Newest <paramref name="maxLines"/> entries. Walks the ring once instead of
    /// materialising the whole buffer twice per call (this is polled by the log window).
    /// </summary>
    public IReadOnlyList<LogLine> Tail(int maxLines)
    {
        if (maxLines <= 0) return Array.Empty<LogLine>();
        var all = _ring.ToArray();
        var start = Math.Max(0, all.Length - maxLines);
        var take = all.Length - start;
        var outp = new LogLine[take];
        Array.Copy(all, start, outp, 0, take);
        return outp;
    }

    public void Dispose()
    {
        lock (_gate) { _writer?.Dispose(); _writer = null; }
    }
}
