using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.Engine.Platforms;

/// <summary>macOS user login Keychain via the `security` CLI (one combined item per tunnel).</summary>
public sealed class MacSecretStore : ISecretStore
{
    private const string Service = "com.tunhub.secrets";

    public void Save(Guid tunnelId, TunnelSecrets secrets)
    {
        var b64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(TunJson.Encode(secrets)));
        Shell.Run("/usr/bin/security", "add-generic-password",
            "-a", tunnelId.ToString(), "-s", Service, "-w", b64, "-U");
    }

    public TunnelSecrets? Load(Guid tunnelId)
    {
        var r = Shell.Run("/usr/bin/security", "find-generic-password",
            "-a", tunnelId.ToString(), "-s", Service, "-w");
        if (!r.Ok) return null;
        try
        {
            var json = Encoding.UTF8.GetString(Convert.FromBase64String(r.Stdout.Trim()));
            return TunJson.Decode<TunnelSecrets>(json);
        }
        catch { return null; }
    }

    public void Delete(Guid tunnelId) =>
        Shell.Run("/usr/bin/security", "delete-generic-password", "-a", tunnelId.ToString(), "-s", Service);
}

/// <summary>Windows DPAPI blob per tunnel. Default scope is CurrentUser (the app runs as the user).</summary>
[SupportedOSPlatform("windows")]
public sealed class WindowsSecretStore : ISecretStore
{
    private readonly string _dir;
    private readonly DataProtectionScope _scope;

    public WindowsSecretStore(string dir, DataProtectionScope scope = DataProtectionScope.CurrentUser)
    {
        _dir = dir;
        _scope = scope;
    }

    private string PathFor(Guid id) => Path.Combine(_dir, $"{id:N}.bin");

    public void Save(Guid tunnelId, TunnelSecrets secrets)
    {
        Directory.CreateDirectory(_dir);
        var plain = Encoding.UTF8.GetBytes(TunJson.Encode(secrets));
        File.WriteAllBytes(PathFor(tunnelId), ProtectedData.Protect(plain, null, _scope));
    }

    public TunnelSecrets? Load(Guid tunnelId)
    {
        var path = PathFor(tunnelId);
        if (!File.Exists(path)) return null;
        try
        {
            var plain = ProtectedData.Unprotect(File.ReadAllBytes(path), null, _scope);
            return TunJson.Decode<TunnelSecrets>(Encoding.UTF8.GetString(plain));
        }
        catch { return null; }
    }

    public void Delete(Guid tunnelId)
    {
        var path = PathFor(tunnelId);
        if (File.Exists(path)) File.Delete(path);
    }
}

public static class PlatformFactory
{
    public static ITunnelPlatform Create(FileLog log)
    {
        if (OperatingSystem.IsWindows()) return new WindowsPlatform(log);
        if (OperatingSystem.IsMacOS()) return new MacPlatform(log);
        throw new PlatformNotSupportedException("TunHub supports Windows and macOS only.");
    }

    /// <summary>User-scoped secret store used by the app (never by the privileged helper).</summary>
    public static ISecretStore CreateAppSecretStore()
    {
        if (OperatingSystem.IsWindows())
        {
            var dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "TunHub", "secrets");
            return new WindowsSecretStore(dir);
        }
        if (OperatingSystem.IsMacOS()) return new MacSecretStore();
        throw new PlatformNotSupportedException("TunHub supports Windows and macOS only.");
    }
}
