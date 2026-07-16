using System.Collections.Concurrent;
using TunHub.Engine.Ipc;

namespace TunHub.Engine.Platform;

public enum LogLevel { Trace, Debug, Info, Warn, Error }

/// <summary>Thread-safe file logger + in-memory ring buffer (served to the UI over IPC).</summary>
public sealed class FileLog
{
    private readonly string _path;
    private readonly object _gate = new();
    private readonly ConcurrentQueue<LogLine> _ring = new();
    private const int RingCapacity = 4000;
    public bool EchoConsole { get; set; } = true;

    public FileLog(string path)
    {
        _path = path;
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    }

    public void Log(LogLevel level, string category, string message)
    {
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
            lock (_gate) File.AppendAllText(_path, text + "\n");
        }
        catch { /* best effort */ }
    }

    public void Trace(string c, string m) => Log(LogLevel.Trace, c, m);
    public void Debug(string c, string m) => Log(LogLevel.Debug, c, m);
    public void Info(string c, string m) => Log(LogLevel.Info, c, m);
    public void Warn(string c, string m) => Log(LogLevel.Warn, c, m);
    public void Error(string c, string m) => Log(LogLevel.Error, c, m);

    public IReadOnlyList<LogLine> Tail(int maxLines) =>
        _ring.Reverse().Take(maxLines).Reverse().ToList();
}
