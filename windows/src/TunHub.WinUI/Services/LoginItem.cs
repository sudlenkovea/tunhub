using System.Runtime.Versioning;

namespace TunHub.WinUI;

/// <summary>
/// "Launch at login" on Windows via the per-user Run key (HKCU\...\Run). The macOS build
/// uses SMAppService.mainApp; this is the Windows equivalent — no admin rights required.
/// </summary>
[SupportedOSPlatform("windows")]
public static class LoginItem
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "TunHub";

    public static void Apply(bool enabled)
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey, writable: true)
                            ?? Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RunKey);
            if (key is null) return;
            if (enabled)
            {
                var exe = Environment.ProcessPath ?? System.Diagnostics.Process.GetCurrentProcess().MainModule?.FileName;
                if (exe is { Length: > 0 }) key.SetValue(ValueName, $"\"{exe}\"");
            }
            else if (key.GetValue(ValueName) is not null)
            {
                key.DeleteValue(ValueName, throwOnMissingValue: false);
            }
        }
        catch { /* best effort — same posture as the macOS LoginItem */ }
    }

    public static bool IsEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is not null;
        }
        catch { return false; }
    }
}
