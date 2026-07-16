namespace TunHub.Engine.Platform;

/// <summary>Per-OS on-disk locations for the privileged helper.</summary>
public static class PlatformPaths
{
    private static string MacVar => "/var/db/tunhub";
    private static string MacRun => "/var/run/tunhub";

    private static string WinBase =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "TunHub");

    public static string VarDir => OperatingSystem.IsWindows() ? Path.Combine(WinBase, "state") : MacVar;
    public static string RunDir => OperatingSystem.IsWindows() ? Path.Combine(WinBase, "run") : MacRun;

    public static string LogFile => OperatingSystem.IsWindows()
        ? Path.Combine(WinBase, "logs", "daemon.log")
        : "/var/log/tunhub-daemon.log";

    /// <summary>Unix-domain-socket path for the UI ↔ helper IPC (AF_UNIX on both OSes).</summary>
    public static string IpcSocket => Path.Combine(RunDir, "ipc.sock");

    public static string OwnershipFile => Path.Combine(VarDir, "owned.json");

    public static void EnsureDirectories()
    {
        foreach (var d in new[] { VarDir, RunDir, Path.GetDirectoryName(LogFile)! })
            Directory.CreateDirectory(d);
    }
}
