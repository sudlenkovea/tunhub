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

    public static TunnelConfig ImportText(string text, string name, AppStore store, IReadOnlyList<TunnelConfig> existing)
    {
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
            if (!entryPath.ToLowerInvariant().EndsWith(".conf")) continue;
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
