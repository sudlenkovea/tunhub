using System.IO.Compression;
using System.Text;
using TunHub.Core;
using TunHub.Engine.Platform;

namespace TunHub.App.Services;

public sealed record ImportResult(List<TunnelConfig> Imported, List<string> Errors);

/// <summary>Import .conf files and ZIP archives; persists config + secrets via AppStore.</summary>
public static class ImportService
{
    private const long MaxEntrySize = 1_000_000;
    private const int MaxEntries = 1000;

    public static ImportResult ImportPaths(IEnumerable<string> paths, AppStore store)
    {
        var imported = new List<TunnelConfig>();
        var errors = new List<string>();
        var existing = store.LoadAll();

        foreach (var path in paths)
        {
            var ext = Path.GetExtension(path).ToLowerInvariant();
            try
            {
                if (ext == ".zip") ImportZip(path, store, existing, imported, errors);
                else
                {
                    var name = Path.GetFileNameWithoutExtension(path);
                    var cfg = ImportText(File.ReadAllText(path), name, store, existing.Concat(imported).ToList());
                    imported.Add(cfg);
                }
            }
            catch (Exception ex) { errors.Add($"{Path.GetFileName(path)}: {ex.Message}"); }
        }
        return new ImportResult(imported, errors);
    }

    /// <summary>Heuristic: an OpenVPN profile has `remote …` lines and no `[Interface]` section.</summary>
    private static bool LooksLikeOpenVpn(string text) =>
        !text.Contains("[Interface]", StringComparison.OrdinalIgnoreCase) &&
        text.Split('\n').Any(l => l.TrimStart().StartsWith("remote ", StringComparison.OrdinalIgnoreCase));

    public static TunnelConfig ImportText(string text, string name, AppStore store, IReadOnlyList<TunnelConfig> existing)
    {
        if (LooksLikeOpenVpn(text)) return ImportOpenVpn(text, name, store, existing);

        var unique = store.UniqueName(name, existing);
        var parsed = WgQuickParser.Parse(unique, text);
        parsed.Config.Name = unique;

        var secrets = new TunnelSecrets { PrivateKey = parsed.PrivateKey };
        parsed.Config.Interface.PrivateKeyRef = new SecretRef($"{parsed.Config.Id}/if");
        foreach (var p in parsed.Config.Peers)
            if (parsed.PresharedKeys.TryGetValue(p.Id, out var psk))
            {
                secrets.Psks[p.Id.ToString()] = psk;
                p.PresharedKeyRef = new SecretRef($"{parsed.Config.Id}/psk/{p.Id}");
            }

        store.SaveSecrets(parsed.Config.Id, secrets);
        store.Save(parsed.Config);
        return parsed.Config;
    }

    /// <summary>Import a .ovpn profile: secret blocks are redacted to placeholders and stored in
    /// the secret vault; the config keeps only the sanitized text (mirrors the macOS import).</summary>
    public static TunnelConfig ImportOpenVpn(string text, string name, AppStore store, IReadOnlyList<TunnelConfig> existing)
    {
        var unique = store.UniqueName(name, existing);
        var parsed = OVPNParser.Parse(unique, text);
        var cfg = new TunnelConfig
        {
            Name = unique,
            Kind = TunnelKind.OpenVpn,
            OpenVpn = parsed.Profile
        };
        var secrets = new TunnelSecrets();
        foreach (var (tag, material) in parsed.Secrets) secrets.OpenVpn[tag] = material;
        store.SaveSecrets(cfg.Id, secrets);
        store.Save(cfg);
        return cfg;
    }

    /// <summary>Export every stored tunnel to a ZIP (.conf for WireGuard-family, .ovpn for OpenVPN).</summary>
    public static void ExportAllZip(AppStore store, string zipPath)
    {
        if (File.Exists(zipPath)) File.Delete(zipPath);
        using var zip = ZipFile.Open(zipPath, ZipArchiveMode.Create);
        var used = new HashSet<string>();
        foreach (var cfg in store.LoadAll())
        {
            var baseName = string.Concat(cfg.Name.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c));
            string entryName;
            string content;
            if (cfg.Kind == TunnelKind.OpenVpn)
            {
                entryName = Dedupe($"{baseName}.ovpn", used);
                content = cfg.OpenVpn?.ConfigText ?? "";
            }
            else
            {
                var secrets = store.LoadSecrets(cfg.Id);
                var psks = cfg.Peers.ToDictionary(p => p.Id,
                    p => secrets is not null && secrets.Psks.TryGetValue(p.Id.ToString(), out var v) ? v : "");
                entryName = Dedupe($"{baseName}.conf", used);
                content = WgQuickParser.Serialize(cfg, secrets?.PrivateKey, psks, redactSecrets: false);
            }
            var entry = zip.CreateEntry(entryName);
            using var w = new StreamWriter(entry.Open(), Encoding.UTF8);
            w.Write(content);
        }
    }

    private static string Dedupe(string name, HashSet<string> used)
    {
        if (used.Add(name)) return name;
        var stem = Path.GetFileNameWithoutExtension(name);
        var ext = Path.GetExtension(name);
        var i = 2;
        while (!used.Add($"{stem} ({i}){ext}")) i++;
        return $"{stem} ({i}){ext}";
    }

    private static void ImportZip(string path, AppStore store, List<TunnelConfig> existing,
        List<TunnelConfig> imported, List<string> errors)
    {
        using var zip = ZipFile.OpenRead(path);
        var count = 0;
        foreach (var entry in zip.Entries)
        {
            if (++count > MaxEntries) { errors.Add("too many files in ZIP"); break; }
            var entryPath = entry.FullName;
            if (entryPath.Contains("..") || entryPath.Contains("__MACOSX") || entryPath.EndsWith(".DS_Store")) continue;
            var lower = entryPath.ToLowerInvariant();
            if (!lower.EndsWith(".conf") && !lower.EndsWith(".ovpn")) continue;
            if (entry.Length > MaxEntrySize) { errors.Add($"{entryPath}: file too large"); continue; }

            try
            {
                using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
                var text = reader.ReadToEnd();
                var name = Path.GetFileNameWithoutExtension(entry.Name);
                var cfg = ImportText(text, name, store, existing.Concat(imported).ToList());
                imported.Add(cfg);
            }
            catch (Exception ex) { errors.Add($"{entryPath}: {ex.Message}"); }
        }
    }
}
