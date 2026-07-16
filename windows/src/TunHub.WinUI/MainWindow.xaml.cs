using System.Collections.ObjectModel;
using System.ComponentModel;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using TunHub.App.Services;
using TunHub.Core;
using Windows.Storage.Pickers;

namespace TunHub.WinUI;

public sealed partial class MainWindow : Window
{
    private readonly AppStore _store = new();
    private readonly DaemonClient _daemon = new();
    private readonly ObservableCollection<TunnelItem> _items = new();
    private readonly DispatcherQueueTimer _timer;
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(6) };
    private Dictionary<Guid, TunnelRuntimeState> _runtime = new();
    private readonly HashSet<Guid> _reportedFailures = new();

    public MainWindow()
    {
        InitializeComponent();
        Title = "TunHub";

        TunnelList.ItemsSource = _items;
        Localize();

        NewBtn.Click += (_, _) => NewTunnel();
        ImportBtn.Click += async (_, _) => await ImportAsync();
        ExportBtn.Click += async (_, _) => await ExportAllAsync();
        ConflictsBtn.Click += async (_, _) => await CheckConflictsAsync();
        LogsBtn.Click += async (_, _) => await ShowLogsAsync();
        SettingsBtn.Click += async (_, _) => await ShowSettingsAsync();
        StopAllBtn.Click += async (_, _) => await SafeAsync(() => _daemon.StopAllAsync());

        StartBtn.Click += async (_, _) => await StartSelectedAsync();
        StopBtn.Click += async (_, _) => await StopSelectedAsync();
        DeleteBtn.Click += (_, _) => DeleteSelected();
        CheckIpBtn.Click += async (_, _) => await CheckExternalIpAsync();
        RetryBtn.Click += async (_, _) => await StartSelectedAsync();
        TunnelList.SelectionChanged += (_, _) => UpdateDetail();

        LoadTunnels();

        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await PollAsync();
        _timer.Start();
        _ = PollAsync();
    }

    private TunnelItem? Selected => TunnelList.SelectedItem as TunnelItem;

    private void Localize()
    {
        NewBtn.Label = Loc.T("New");
        ImportBtn.Label = Loc.T("Import");
        ExportBtn.Label = Loc.T("Export all (ZIP)");
        ConflictsBtn.Label = Loc.T("Check conflicts");
        LogsBtn.Label = Loc.T("Logs");
        SettingsBtn.Label = Loc.T("Settings");
        StopAllBtn.Content = Loc.T("Stop all");
        StartBtn.Content = Loc.T("Start");
        StopBtn.Content = Loc.T("Stop");
        DeleteBtn.Content = Loc.T("Delete");
        CheckIpBtn.Content = Loc.T("Check");
        EmptyHint.Text = Loc.T("Select a tunnel or import configs");
    }

    private void LoadTunnels()
    {
        var keep = Selected?.Config.Id;
        _items.Clear();
        foreach (var t in _store.LoadAll()) _items.Add(new TunnelItem(t));
        if (_items.Count > 0 && TunnelList.SelectedItem is null)
            TunnelList.SelectedItem = _items.FirstOrDefault(i => i.Config.Id == keep) ?? _items[0];
        UpdateDetail();
    }

    private void UpdateDetail()
    {
        var item = Selected;
        EmptyHint.Visibility = item is null ? Visibility.Visible : Visibility.Collapsed;
        DetailPanel.Visibility = item is null ? Visibility.Collapsed : Visibility.Visible;
        if (item is null) return;

        var cfg = item.Config;
        DetailName.Text = cfg.Name;
        DetailKind.Text = cfg.Kind.Label();
        _runtime.TryGetValue(cfg.Id, out var s);
        var phase = s?.Phase ?? TunnelPhase.Stopped;

        DetailStatus.Text = $"{Loc.T("Status")}: {PhaseText(phase)}";
        DetailDot.Fill = PhaseBrush(phase);

        OvEndpoint.Text = s?.Peers.FirstOrDefault()?.Endpoint ?? "—";
        OvRoutes.Text = (s?.Routes is { Count: > 0 } r)
            ? string.Join(", ", r)
            : (cfg.HasDefaultRoute ? Loc.T("all traffic (default route)")
                                   : string.Join(", ", cfg.EffectiveRoutes().Select(x => x.Canonical)));
        OvTraffic.Text = s is null ? "—"
            : $"rx {ByteFormat.Human(s.RxTotal)}   tx {ByteFormat.Human(s.TxTotal)}";

        var running = phase is TunnelPhase.Up or TunnelPhase.Degraded or TunnelPhase.Starting;
        StartBtn.IsEnabled = !running;
        StopBtn.IsEnabled = running;

        // Failure banner + retry (mirrors macOS connectionFailure alert).
        if (phase == TunnelPhase.Failed && s?.ErrorMessage is { Length: > 0 } err)
        {
            FailureBar.Message = err;
            var isAuth = err.Contains("auth", StringComparison.OrdinalIgnoreCase);
            RetryBtn.Content = Loc.T(isAuth ? "Re-enter credentials" : "Retry");
            FailureBar.IsOpen = true;
        }
        else FailureBar.IsOpen = false;

        BuildEditor(cfg);
        BuildStatus(cfg, s);
    }

    // MARK: - Editor / Status hosts

    private void BuildEditor(TunnelConfig cfg)
    {
        EditorHost.Children.Clear();
        var name = new TextBox { Header = Loc.T("Name"), Text = cfg.Name };
        EditorHost.Children.Add(name);

        var summary = new TextBlock
        {
            Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
            TextWrapping = TextWrapping.Wrap
        };
        if (cfg.Kind == TunnelKind.OpenVpn && cfg.OpenVpn is { } ov)
        {
            summary.Text =
                $"{Loc.T("Remotes")}: {string.Join(", ", ov.Remotes.Select(r => $"{r.Host}:{r.Port}/{r.Proto}"))}\n" +
                $"{Loc.T("Auth")}: {ov.AuthMode}\n" +
                (ov.Cipher is { } c ? $"{Loc.T("Cipher")}: {c}\n" : "") +
                (ov.RedirectGateway ? Loc.T("Redirect gateway") + "\n" : "") +
                Loc.T("The .ovpn profile is read-only — re-import the file to change it.");
        }
        else
        {
            summary.Text =
                $"{Loc.T("Address")}: {string.Join(", ", cfg.Interface.Addresses.Select(a => a.ToString()))}\n" +
                $"{Loc.T("Peers")}: {cfg.Peers.Count}\n" +
                (cfg.Interface.Dns.Count > 0 ? $"DNS: {string.Join(", ", cfg.Interface.Dns)}\n" : "") +
                (cfg.Awg is { IsEmpty: false } ? Loc.T("AmneziaWG obfuscation") : "");
        }
        EditorHost.Children.Add(summary);

        var save = new Button { Content = Loc.T("Save"), Style = (Style)Application.Current.Resources["AccentButtonStyle"] };
        save.Click += (_, _) =>
        {
            var c = cfg;
            c.Name = name.Text.Trim();
            _store.Save(c);
            LoadTunnels();
            DetailPivot.SelectedIndex = 0; // jump back to Overview (matches macOS "Cancel/Save → status")
        };
        EditorHost.Children.Add(save);
    }

    private void BuildStatus(TunnelConfig cfg, TunnelRuntimeState? s)
    {
        StatusHost.Children.Clear();
        if (s is null || s.Peers.Count == 0)
        {
            StatusHost.Children.Add(new TextBlock
            {
                Text = Loc.T("tunnel not running"),
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
            });
            return;
        }
        foreach (var p in s.Peers)
        {
            var card = new StackPanel { Spacing = 2 };
            if (p.Endpoint is { Length: > 0 })
                card.Children.Add(new TextBlock { Text = $"{Loc.T("Endpoint")}: {p.Endpoint}" });
            card.Children.Add(new TextBlock
            {
                Text = $"rx {ByteFormat.Human(p.RxBytes)}   tx {ByteFormat.Human(p.TxBytes)}",
                Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
            });
            if (p.LastHandshake is { } hs)
                card.Children.Add(new TextBlock
                {
                    Text = $"handshake: {hs.LocalDateTime:HH:mm:ss}",
                    Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]
                });
            StatusHost.Children.Add(card);
        }
    }

    // MARK: - Poll

    private async Task PollAsync()
    {
        var reachable = await _daemon.PingAsync();
        HelperStatus.Text = reachable ? Loc.T("Helper: connected") : Loc.T("Helper: not reachable");
        HelperDot.Fill = new SolidColorBrush(reachable ? Colors.MediumSeaGreen : Colors.Gray);

        var states = await _daemon.RuntimeStatesAsync();
        _runtime = states.GroupBy(s => s.Id).ToDictionary(g => g.Key, g => g.First());
        foreach (var item in _items)
        {
            var phase = _runtime.TryGetValue(item.Config.Id, out var s) ? s.Phase : TunnelPhase.Stopped;
            item.SetPhase(phase);
        }
        UpdateDetail();
    }

    // MARK: - Actions

    private void NewTunnel()
    {
        var cfg = new TunnelConfig { Name = _store.UniqueName("New tunnel", _store.LoadAll()) };
        var priv = WgKey.GeneratePrivateKey();
        cfg.Interface.PrivateKeyRef = new SecretRef($"{cfg.Id}/if");
        cfg.Interface.PublicKey = WgKey.PublicKey(priv) ?? "";
        _store.SaveSecrets(cfg.Id, new TunnelSecrets { PrivateKey = priv });
        _store.Save(cfg);
        LoadTunnels();
        TunnelList.SelectedItem = _items.FirstOrDefault(i => i.Config.Id == cfg.Id);
        DetailPivot.SelectedIndex = 1; // open the editor
    }

    private async Task StartSelectedAsync()
    {
        if (Selected is null) return;
        var cfg = Selected.Config;
        try
        {
            var spec = _store.ResolveSpec(cfg);

            // OpenVPN: ask for credentials/OTP at connect if they aren't stored or a static
            // challenge is configured (mirrors the macOS OVPNCredentialSheet).
            if (cfg.Kind == TunnelKind.OpenVpn && cfg.OpenVpn is { } prof && spec.OpenVpn is { } rov)
            {
                var needsAsk = prof.StaticChallenge is not null
                               || (prof.NeedsUsername && string.IsNullOrEmpty(rov.Password));
                if (needsAsk)
                {
                    var creds = await PromptOvpnCredentialsAsync(cfg, rov.Username);
                    if (creds is null) return; // cancelled
                    rov.Username = creds.Value.User;
                    rov.Password = creds.Value.Pass;
                    rov.Otp = creds.Value.Otp;
                    if (creds.Value.Remember)
                    {
                        var s = _store.LoadSecrets(cfg.Id) ?? new TunnelSecrets();
                        s.OpenVpn["username"] = creds.Value.User;
                        s.OpenVpn["password"] = creds.Value.Pass;
                        _store.SaveSecrets(cfg.Id, s);
                    }
                }
            }

            _reportedFailures.Remove(cfg.Id);
            await _daemon.StartTunnelAsync(spec);
        }
        catch (Exception ex) { DetailStatus.Text = ex.Message; }
        await PollAsync();
    }

    private async Task<(string User, string Pass, string Otp, bool Remember)?> PromptOvpnCredentialsAsync(
        TunnelConfig cfg, string? presetUser)
    {
        var user = new TextBox { Header = Loc.T("Username"), Text = presetUser ?? "" };
        var pass = new PasswordBox { Header = Loc.T("Password") };
        var otp = new TextBox { Header = Loc.T("One-time code (OTP)") };
        var remember = new CheckBox { Content = Loc.T("Save login and password") };
        var panel = new StackPanel { Spacing = 10 };
        panel.Children.Add(user);
        panel.Children.Add(pass);
        if (cfg.OpenVpn?.StaticChallenge is not null) panel.Children.Add(otp);
        panel.Children.Add(remember);

        var dialog = new ContentDialog
        {
            Title = string.Format(Loc.T("Connect “{0}”"), cfg.Name),
            Content = panel,
            PrimaryButtonText = Loc.T("Connect"),
            CloseButtonText = Loc.T("Cancel"),
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = Root.XamlRoot
        };
        var result = await dialog.ShowAsync();
        if (result != ContentDialogResult.Primary) return null;
        return (user.Text.Trim(), pass.Password, otp.Text.Trim(), remember.IsChecked == true);
    }

    private async Task StopSelectedAsync()
    {
        if (Selected is null) return;
        await SafeAsync(() => _daemon.StopTunnelAsync(Selected.Config.Id));
        await PollAsync();
    }

    private void DeleteSelected()
    {
        if (Selected is null) return;
        _store.Delete(Selected.Config);
        LoadTunnels();
    }

    private async Task CheckExternalIpAsync()
    {
        OvExternalIp.Text = Loc.T("checking…");
        try
        {
            var ip = await _http.GetStringAsync("https://api.ipify.org");
            OvExternalIp.Text = ip.Trim();
        }
        catch { OvExternalIp.Text = Loc.T("failed"); }
    }

    private async Task ImportAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".conf");
        picker.FileTypeFilter.Add(".ovpn");
        picker.FileTypeFilter.Add(".zip");
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

        var files = await picker.PickMultipleFilesAsync();
        if (files is null || files.Count == 0) return;
        var result = ImportService.ImportPaths(files.Select(f => f.Path), _store);
        LoadTunnels();
        if (result.Errors.Count > 0) await MessageAsync(Loc.T("Errors"), string.Join("\n", result.Errors));
    }

    private async Task ExportAllAsync()
    {
        if (_items.Count == 0) return;
        var picker = new FileSavePicker { SuggestedFileName = "tunhub-tunnels" };
        picker.FileTypeChoices.Add("ZIP", new List<string> { ".zip" });
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
        var file = await picker.PickSaveFileAsync();
        if (file is null) return;
        try { ImportService.ExportAllZip(_store, file.Path); }
        catch (Exception ex) { await MessageAsync(Loc.T("Errors"), ex.Message); }
    }

    private async Task CheckConflictsAsync()
    {
        var findings = _store.CheckAllConflicts();
        var text = findings.Count == 0
            ? Loc.T("No conflicts found")
            : string.Join("\n", findings.Select(f => $"{f.Code}: {f.Message}"));
        await MessageAsync(Loc.T("Check all tunnels"), text);
    }

    private async Task ShowLogsAsync()
    {
        var lines = await _daemon.RecentLogAsync(400);
        var text = string.Join("\n", lines.Select(l => $"{l.Time.LocalDateTime:HH:mm:ss} [{l.Level}] {l.Category}: {l.Message}"));
        await MessageAsync(Loc.T("Logs"), string.IsNullOrEmpty(text) ? "—" : text, scroll: true);
    }

    private async Task ShowSettingsAsync()
    {
        var settings = AppSettings.Load();
        var lang = new ComboBox { Header = Loc.T("Interface language") };
        foreach (var (_, native) in Loc.Available) lang.Items.Add(native);
        var codes = Loc.Available.Select(a => a.Code).ToList();
        lang.SelectedIndex = Math.Max(0, codes.IndexOf(settings.Language));

        var launch = new ToggleSwitch { Header = Loc.T("Launch TunHub at login"), IsOn = settings.LaunchAtLogin };
        var kill = new ToggleSwitch { Header = Loc.T("Kill switch (global)"), IsOn = settings.KillSwitchGlobal };

        var panel = new StackPanel { Spacing = 12 };
        panel.Children.Add(lang);
        panel.Children.Add(launch);
        panel.Children.Add(kill);
        lang.SelectionChanged += (_, _) => { if (lang.SelectedIndex >= 0) settings.Language = codes[lang.SelectedIndex]; };

        var dialog = new ContentDialog
        {
            Title = Loc.T("Settings"), Content = panel,
            PrimaryButtonText = "OK", XamlRoot = Root.XamlRoot
        };
        await dialog.ShowAsync();

        settings.LaunchAtLogin = launch.IsOn;
        settings.KillSwitchGlobal = kill.IsOn;
        settings.Save();
        LoginItem.Apply(settings.LaunchAtLogin);
        await SafeAsync(() => _daemon.SetKillSwitchAsync(settings.KillSwitchGlobal));
    }

    private async Task MessageAsync(string title, string body, bool scroll = false)
    {
        FrameworkElement content = scroll
            ? new ScrollViewer { Content = new TextBlock { Text = body, FontFamily = new FontFamily("Consolas"), TextWrapping = TextWrapping.Wrap }, MaxHeight = 360 }
            : new TextBlock { Text = body, TextWrapping = TextWrapping.Wrap };
        var dialog = new ContentDialog { Title = title, Content = content, CloseButtonText = Loc.T("Close"), XamlRoot = Root.XamlRoot };
        await dialog.ShowAsync();
    }

    private static string PhaseText(TunnelPhase p) => p switch
    {
        TunnelPhase.Up => Loc.T("running"),
        TunnelPhase.Degraded => Loc.T("degraded"),
        TunnelPhase.Starting => Loc.T("starting…"),
        TunnelPhase.Stopping => Loc.T("stopping…"),
        TunnelPhase.Failed => Loc.T("failed"),
        _ => Loc.T("stopped")
    };

    private static Brush PhaseBrush(TunnelPhase p) => new SolidColorBrush(p switch
    {
        TunnelPhase.Up => Colors.MediumSeaGreen,
        TunnelPhase.Degraded or TunnelPhase.Starting or TunnelPhase.Stopping => Colors.Orange,
        TunnelPhase.Failed => Colors.IndianRed,
        _ => Colors.Gray
    });

    private static async Task SafeAsync(Func<Task> action)
    {
        try { await action(); } catch { /* surfaced elsewhere */ }
    }
}

/// <summary>List row: tunnel + live status + kind badge (bindable).</summary>
public sealed class TunnelItem : INotifyPropertyChanged
{
    public TunnelConfig Config { get; }
    public string Name { get; }
    public string KindLabel { get; }
    public Brush KindBg { get; }
    public Brush KindFg { get; }

    public TunnelItem(TunnelConfig config)
    {
        Config = config;
        Name = config.Name;
        (KindLabel, var bg, var fg) = config.Kind switch
        {
            TunnelKind.WireGuard => ("WG", Color.FromArgb(255, 0xE6, 0xF1, 0xFB), Color.FromArgb(255, 0x18, 0x5F, 0xA5)),
            TunnelKind.AmneziaWg => ("AWG", Color.FromArgb(255, 0xFA, 0xEE, 0xDA), Color.FromArgb(255, 0x85, 0x4F, 0x0B)),
            TunnelKind.OpenVpn   => ("OVPN", Color.FromArgb(255, 0xE1, 0xF5, 0xEE), Color.FromArgb(255, 0x0F, 0x6E, 0x56)),
            _ => ("?", Colors.LightGray, Colors.Black)
        };
        KindBg = new SolidColorBrush(bg);
        KindFg = new SolidColorBrush(fg);
    }

    public void SetPhase(TunnelPhase phase)
    {
        StatusBrush = new SolidColorBrush(phase switch
        {
            TunnelPhase.Up => Colors.MediumSeaGreen,
            TunnelPhase.Degraded or TunnelPhase.Starting or TunnelPhase.Stopping => Colors.Orange,
            TunnelPhase.Failed => Colors.IndianRed,
            _ => Colors.Gray
        });
        Busy = phase is TunnelPhase.Starting or TunnelPhase.Stopping;
        BusyVisibility = Busy ? Visibility.Visible : Visibility.Collapsed;
    }

    private Brush _statusBrush = new SolidColorBrush(Colors.Gray);
    public Brush StatusBrush { get => _statusBrush; set { _statusBrush = value; Changed(nameof(StatusBrush)); } }

    private bool _busy;
    public bool Busy { get => _busy; set { _busy = value; Changed(nameof(Busy)); } }

    private Visibility _busyVisibility = Visibility.Collapsed;
    public Visibility BusyVisibility { get => _busyVisibility; set { _busyVisibility = value; Changed(nameof(BusyVisibility)); } }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}
