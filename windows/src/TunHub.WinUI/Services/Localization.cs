using TunHub.Core;

namespace TunHub.App.Services;

/// <summary>
/// Minimal runtime localization: English base, keyed by English text; a per-language
/// override table. The chosen language is persisted and applied on startup (like the
/// Swift app). Add a language by extending <see cref="Tables"/>.
/// </summary>
public static class Loc
{
    private static Dictionary<string, string> _active = new();

    public static IReadOnlyList<(string Code, string Native)> Available { get; } = new[]
    {
        ("system", "System default"),
        ("en", "English"),
        ("ru", "Русский")
    };

    public static string CurrentCode { get; private set; } = "system";

    public static void Apply(string code)
    {
        CurrentCode = code;
        var resolved = code == "system" ? SystemLanguage() : code;
        _active = Tables.TryGetValue(resolved, out var t) ? t : new Dictionary<string, string>();
    }

    /// <summary>Localize an English base string.</summary>
    public static string T(string english) => _active.TryGetValue(english, out var v) ? v : english;

    private static string SystemLanguage() =>
        System.Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;

    private static readonly Dictionary<string, Dictionary<string, string>> Tables = new()
    {
        ["ru"] = new()
        {
            ["Tunnels"] = "Туннели",
            ["Start"] = "Запуск",
            ["Stop"] = "Стоп",
            ["Import…"] = "Импорт…",
            ["Delete"] = "Удалить",
            ["Stop all"] = "Остановить все",
            ["Settings"] = "Настройки",
            ["Interface language"] = "Язык интерфейса",
            ["Restart to apply the language."] = "Перезапустите для смены языка.",
            ["Kill switch (global)"] = "Kill switch (глобально)",
            ["Helper: connected"] = "Хелпер: подключён",
            ["Helper: not reachable"] = "Хелпер: недоступен",
            ["No tunnels — import a .conf or ZIP"] = "Нет туннелей — импортируйте .conf или ZIP",
            ["Status"] = "Статус",
            ["External IP"] = "Внешний IP",
            ["running"] = "работает",
            ["stopped"] = "остановлен",
            ["starting…"] = "запускается…",
            ["stopping…"] = "останавливается…",
            ["degraded"] = "деградирован",
            ["failed"] = "ошибка"
        }
    };
}

/// <summary>Persisted app settings (language, global kill switch).</summary>
public sealed class AppSettings
{
    public string Language { get; set; } = "system";
    public bool KillSwitchGlobal { get; set; } = true;
    public bool LaunchAtLogin { get; set; }

    private static string Path => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "TunHub", "settings.json");

    public static AppSettings Load()
    {
        try { return TunJson.Decode<AppSettings>(File.ReadAllText(Path)) ?? new(); }
        catch { return new(); }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
            File.WriteAllText(Path, TunJson.Encode(this));
        }
        catch { /* best effort */ }
    }
}
