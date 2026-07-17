using System.Collections.ObjectModel;
using System.ComponentModel;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using TunHub.App.Services;
using TunHub.Core;
using TunHub.Engine.Platform;
using Windows.Storage.Pickers;
using Windows.UI;

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
    private readonly AppWindow _appWindow;
    private Guid _editorForId;               // the tunnel the Editor tab is currently built for
    private bool _helperReachable;
    private readonly Dictionary<Guid, string> _externalIp = new();   // per-tunnel checked-at-start IP

    /// <summary>Raised when the user closes the window — the app hides it to the tray.</summary>
    public event Action? HideToTrayRequested;

    public MainWindow()
    {
        InitializeComponent();
        Title = "TunHub";

        _appWindow = GetAppWindow();
        try { _appWindow.SetIcon("Assets/TunHub.ico"); } catch { }
        _appWindow.Closing += (_, e) => { e.Cancel = true; HideToTrayRequested?.Invoke(); };

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
        RetryBtn.Click += async (_, _) => await StartSelectedAsync();
        InstallHelperBtn.Click += async (_, _) => await InstallHelperAsync();
        DetailTabs.SelectionChanged += (_, _) => UpdateTabVisibility();
        TunnelList.SelectionChanged += (_, _) => UpdateDetail();

        LoadTunnels();

        _timer = DispatcherQueue.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += async (_, _) => await PollAsync();
        _timer.Start();
        _ = PollAsync();
    }

    private TunnelItem? Selected => TunnelList.SelectedItem as TunnelItem;

    private AppWindow GetAppWindow()
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        var id = Win32Interop.GetWindowIdFromWindow(hwnd);
        return AppWindow.GetFromWindowId(id);
    }

    public void AppWindowShow() => _appWindow.Show();
    public void AppWindowHide() => _appWindow.Hide();

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
        EmptyHint.Text = Loc.T("Select a tunnel or import configs");
        TabOverview.Text = Loc.T("Overview");
        TabEditor.Text = Loc.T("Editor");
        TabStatus.Text = Loc.T("Status");
        HelperBar.Title = Loc.T("System component not running");
        HelperBar.Message = Loc.T("TunHub needs a background service to manage tunnels.");
        InstallHelperBtn.Content = Loc.T("Install system component");
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
        BuildRoutes(cfg, s);
        OvTraffic.Text = s is null ? "—"
            : $"rx {ByteFormat.Human(s.RxTotal)}   tx {ByteFormat.Human(s.TxTotal)}";

        var running = phase is TunnelPhase.Up or TunnelPhase.Degraded or TunnelPhase.Starting;
        StartBtn.IsEnabled = !running;
        StopBtn.IsEnabled = running;

        // External IP: no manual button — check once automatically when the tunnel is up.
        if (phase is TunnelPhase.Up or TunnelPhase.Degraded)
        {
            if (_externalIp.TryGetValue(cfg.Id, out var ip)) OvExternalIp.Text = ip;
            else { OvExternalIp.Text = Loc.T("checking…"); _externalIp[cfg.Id] = Loc.T("checking…"); _ = CheckExternalIpAsync(cfg); }
        }
        else { OvExternalIp.Text = "—"; _externalIp.Remove(cfg.Id); }

        // Failure banner + retry (mirrors macOS connectionFailure alert).
        if (phase == TunnelPhase.Failed && s?.ErrorMessage is { Length: > 0 } err)
        {
            FailureBar.Message = err;
            var isAuth = err.Contains("auth", StringComparison.OrdinalIgnoreCase);
            RetryBtn.Content = Loc.T(isAuth ? "Re-enter credentials" : "Retry");
            FailureBar.IsOpen = true;
        }
        else FailureBar.IsOpen = false;

        // Rebuild the editor form only when the selected tunnel changes — otherwise the 1 Hz
        // poll would wipe whatever the user is typing.
        if (_editorForId != cfg.Id)
        {
            _editorForId = cfg.Id;
            BuildEditor(cfg);
            SelectTab(TabOverview);
        }
        BuildStatus(cfg, s);
    }

    // Routes: config routes for WG; server-pushed runtime routes for OpenVPN. One per line,
    // collapsed into an expander when there are many (mirrors the macOS RoutesBox).
    private void BuildRoutes(TunnelConfig cfg, TunnelRuntimeState? s)
    {
        OvRoutesHost.Children.Clear();
        List<string> routes = s?.Routes is { Count: > 0 } r
            ? r
            : cfg.EffectiveRoutes().Select(x => x.Canonical).ToList();

        if (routes.Count == 0)
        {
            OvRoutesHost.Children.Add(new TextBlock { Text = cfg.HasDefaultRoute ? Loc.T("all traffic (default route)") : "—" });
            return;
        }
        if (routes.Count == 1)
        {
            OvRoutesHost.Children.Add(new TextBlock { Text = routes[0], IsTextSelectionEnabled = true });
            return;
        }
        var lines = new StackPanel { Spacing = 1 };
        foreach (var route in routes)
            lines.Children.Add(new TextBlock { Text = route, FontFamily = new FontFamily("Consolas"), FontSize = 12, IsTextSelectionEnabled = true });
        var exp = new Expander
        {
            Header = string.Format(Loc.T("{0} routes"), routes.Count),
            Content = lines,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch
        };
        OvRoutesHost.Children.Add(exp);
    }

    private void SelectTab(SelectorBarItem item)
    {
        if (DetailTabs.SelectedItem != item) DetailTabs.SelectedItem = item;
        UpdateTabVisibility();
    }

    private void UpdateTabVisibility()
    {
        OverviewPane.Visibility = DetailTabs.SelectedItem == TabOverview ? Visibility.Visible : Visibility.Collapsed;
        EditorPane.Visibility   = DetailTabs.SelectedItem == TabEditor   ? Visibility.Visible : Visibility.Collapsed;
        StatusPane.Visibility   = DetailTabs.SelectedItem == TabStatus   ? Visibility.Visible : Visibility.Collapsed;
    }

    // MARK: - Editor / Status hosts

    private static TextBlock SectionHeader(string text) => new()
    {
        Text = text, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, Margin = new Thickness(0, 6, 0, 0)
    };

    private static Brush Secondary => (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"];

    private void BuildEditor(TunnelConfig cfg)
    {
        EditorHost.Children.Clear();

        if (cfg.Kind == TunnelKind.OpenVpn) { BuildOpenVpnEditor(cfg); return; }

        var name = new TextBox { Header = Loc.T("Name"), Text = cfg.Name };
        var priv = new TextBox { Header = Loc.T("Private key"), Text = _store.LoadSecrets(cfg.Id)?.PrivateKey ?? "" };
        var pub = new TextBox { Header = Loc.T("Public key"), Text = cfg.Interface.PublicKey, IsReadOnly = true };
        var gen = new Button { Content = Loc.T("Generate") };
        gen.Click += (_, _) => { var k = WgKey.GeneratePrivateKey(); priv.Text = k; pub.Text = WgKey.PublicKey(k) ?? ""; };
        priv.TextChanged += (_, _) => pub.Text = WgKey.PublicKey(priv.Text.Trim()) ?? "";

        var addresses = new TextBox { Header = Loc.T("Address"), Text = string.Join(", ", cfg.Interface.Addresses.Select(Fmt)) };
        var dns = new TextBox { Header = "DNS", Text = string.Join(", ", cfg.Interface.Dns) };
        var mtu = new TextBox { Header = "MTU", Text = cfg.Interface.Mtu?.ToString() ?? "" };
        var listen = new TextBox { Header = "ListenPort", Text = cfg.Interface.ListenPort?.ToString() ?? "" };

        EditorHost.Children.Add(name);
        EditorHost.Children.Add(SectionHeader(Loc.T("Interface")));
        EditorHost.Children.Add(priv);
        EditorHost.Children.Add(gen);
        EditorHost.Children.Add(pub);
        EditorHost.Children.Add(addresses);
        EditorHost.Children.Add(dns);
        var row = new Grid { ColumnSpacing = 10 };
        row.ColumnDefinitions.Add(new ColumnDefinition());
        row.ColumnDefinitions.Add(new ColumnDefinition());
        Grid.SetColumn(mtu, 0); Grid.SetColumn(listen, 1);
        row.Children.Add(mtu); row.Children.Add(listen);
        EditorHost.Children.Add(row);

        // Peers.
        EditorHost.Children.Add(SectionHeader(Loc.T("Peers")));
        var peerBoxes = new List<(TextBox Pub, TextBox End, TextBox Allowed, TextBox Keep)>();
        var peersHost = new StackPanel { Spacing = 8 };
        EditorHost.Children.Add(peersHost);
        void AddPeerCard(PeerConfig? p)
        {
            var pk = new TextBox { Header = Loc.T("Public key"), Text = p?.PublicKey ?? "" };
            var ep = new TextBox { Header = Loc.T("Endpoint"), Text = p?.Endpoint ?? "" };
            var al = new TextBox { Header = "AllowedIPs", Text = p is null ? "0.0.0.0/0, ::/0" : string.Join(", ", p.AllowedIPs.Select(Fmt)) };
            var ka = new TextBox { Header = "Keepalive", Text = p?.PersistentKeepalive?.ToString() ?? "" };
            var box = (pk, ep, al, ka);
            peerBoxes.Add(box);
            var card = new StackPanel { Spacing = 6, Padding = new Thickness(10),
                BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
                BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8) };
            card.Children.Add(pk); card.Children.Add(ep); card.Children.Add(al); card.Children.Add(ka);
            var rm = new Button { Content = Loc.T("Remove peer") };
            rm.Click += (_, _) => { peersHost.Children.Remove(card); peerBoxes.Remove(box); };
            card.Children.Add(rm);
            peersHost.Children.Add(card);
        }
        foreach (var p in cfg.Peers) AddPeerCard(p);
        var addPeer = new Button { Content = Loc.T("Add peer") };
        addPeer.Click += (_, _) => AddPeerCard(null);
        EditorHost.Children.Add(addPeer);

        // AmneziaWG obfuscation (compact grid of the common knobs).
        TextBox? jc = null, jmin = null, jmax = null, s1 = null, s2 = null, s3 = null, s4 = null;
        if (cfg.Kind == TunnelKind.AmneziaWg)
        {
            EditorHost.Children.Add(SectionHeader(Loc.T("AmneziaWG obfuscation")));
            var a = cfg.Awg ?? new AwgParams();
            jc = new TextBox { Header = "Jc", Text = a.Jc?.ToString() ?? "" };
            jmin = new TextBox { Header = "Jmin", Text = a.Jmin?.ToString() ?? "" };
            jmax = new TextBox { Header = "Jmax", Text = a.Jmax?.ToString() ?? "" };
            s1 = new TextBox { Header = "S1", Text = a.S1?.ToString() ?? "" };
            s2 = new TextBox { Header = "S2", Text = a.S2?.ToString() ?? "" };
            s3 = new TextBox { Header = "S3", Text = a.S3?.ToString() ?? "" };
            s4 = new TextBox { Header = "S4", Text = a.S4?.ToString() ?? "" };
            var g = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
            for (int i = 0; i < 4; i++) g.ColumnDefinitions.Add(new ColumnDefinition());
            g.RowDefinitions.Add(new RowDefinition()); g.RowDefinitions.Add(new RowDefinition());
            void place(TextBox t, int c, int r) { Grid.SetColumn(t, c); Grid.SetRow(t, r); g.Children.Add(t); }
            place(jc!, 0, 0); place(jmin!, 1, 0); place(jmax!, 2, 0); place(s1!, 3, 0);
            place(s2!, 0, 1); place(s3!, 1, 1); place(s4!, 2, 1);
            EditorHost.Children.Add(g);
        }

        // Options.
        EditorHost.Children.Add(SectionHeader(Loc.T("Options")));
        var kill = new ToggleSwitch { Header = Loc.T("Kill switch (block traffic outside the tunnel)"), IsOn = cfg.Options.KillSwitch };
        var autoc = new ToggleSwitch { Header = Loc.T("Connect on app launch"), IsOn = cfg.Options.AutoConnectOnLaunch };
        EditorHost.Children.Add(kill);
        EditorHost.Children.Add(autoc);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 6, 0, 0) };
        var cancel = new Button { Content = Loc.T("Cancel") };
        cancel.Click += (_, _) => { _editorForId = Guid.Empty; UpdateDetail(); SelectTab(TabOverview); };
        var save = new Button { Content = Loc.T("Save"), Style = (Style)Application.Current.Resources["AccentButtonStyle"] };
        save.Click += (_, _) =>
        {
            cfg.Name = name.Text.Trim();
            cfg.Interface.PublicKey = pub.Text.Trim();
            cfg.Interface.Addresses = ParseRanges(addresses.Text);
            cfg.Interface.Dns = SplitList(dns.Text);
            cfg.Interface.Mtu = int.TryParse(mtu.Text.Trim(), out var m) ? m : null;
            cfg.Interface.ListenPort = ushort.TryParse(listen.Text.Trim(), out var lp) ? lp : null;
            cfg.Peers = peerBoxes.Select(b => new PeerConfig
            {
                PublicKey = b.Pub.Text.Trim(),
                Endpoint = string.IsNullOrWhiteSpace(b.End.Text) ? null : b.End.Text.Trim(),
                AllowedIPs = ParseRanges(b.Allowed.Text),
                PersistentKeepalive = ushort.TryParse(b.Keep.Text.Trim(), out var k) ? k : null
            }).ToList();
            if (cfg.Kind == TunnelKind.AmneziaWg)
            {
                cfg.Awg = new AwgParams
                {
                    Jc = ParseInt(jc), Jmin = ParseInt(jmin), Jmax = ParseInt(jmax),
                    S1 = ParseInt(s1), S2 = ParseInt(s2), S3 = ParseInt(s3), S4 = ParseInt(s4)
                };
            }
            cfg.Options.KillSwitch = kill.IsOn;
            cfg.Options.AutoConnectOnLaunch = autoc.IsOn;

            var secrets = _store.LoadSecrets(cfg.Id) ?? new TunnelSecrets();
            secrets.PrivateKey = priv.Text.Trim();
            _store.SaveSecrets(cfg.Id, secrets);
            _store.Save(cfg);
            _editorForId = Guid.Empty;
            LoadTunnels();
            SelectTab(TabOverview);
        };
        buttons.Children.Add(save);
        buttons.Children.Add(cancel);
        EditorHost.Children.Add(buttons);
    }

    private void BuildOpenVpnEditor(TunnelConfig cfg)
    {
        var ov = cfg.OpenVpn!;
        var name = new TextBox { Header = Loc.T("Name"), Text = cfg.Name };
        EditorHost.Children.Add(name);

        EditorHost.Children.Add(SectionHeader(Loc.T("OpenVPN profile")));
        var summary = new TextBlock { Foreground = Secondary, TextWrapping = TextWrapping.Wrap, Text =
            $"{Loc.T("Remotes")}: {string.Join(", ", ov.Remotes.Select(r => $"{r.Host}:{r.Port}/{r.Proto}"))}\n" +
            $"{Loc.T("Auth")}: {ov.AuthMode}\n" +
            (ov.Cipher is { } c ? $"{Loc.T("Cipher")}: {c}\n" : "") +
            (ov.RedirectGateway ? Loc.T("Redirect gateway") + "\n" : "") +
            Loc.T("The .ovpn profile is read-only — re-import the file to change it.") };
        EditorHost.Children.Add(summary);

        EditorHost.Children.Add(SectionHeader(Loc.T("Credentials")));
        var stored = _store.LoadSecrets(cfg.Id);
        var user = new TextBox { Header = Loc.T("Username"), Text = stored?.OpenVpn.GetValueOrDefault("username") ?? "" };
        var pass = new PasswordBox { Header = Loc.T("Password") };
        EditorHost.Children.Add(user);
        EditorHost.Children.Add(pass);
        EditorHost.Children.Add(new TextBlock { Text = Loc.T("OTP is asked at connect"), FontSize = 12, Foreground = Secondary });

        EditorHost.Children.Add(SectionHeader(Loc.T("Options")));
        var kill = new ToggleSwitch { Header = Loc.T("Kill switch (block traffic outside the tunnel)"), IsOn = cfg.Options.KillSwitch };
        var autoc = new ToggleSwitch { Header = Loc.T("Connect on app launch"), IsOn = cfg.Options.AutoConnectOnLaunch };
        EditorHost.Children.Add(kill);
        EditorHost.Children.Add(autoc);

        var save = new Button { Content = Loc.T("Save"), Style = (Style)Application.Current.Resources["AccentButtonStyle"], Margin = new Thickness(0, 6, 0, 0) };
        save.Click += (_, _) =>
        {
            cfg.Name = name.Text.Trim();
            cfg.Options.KillSwitch = kill.IsOn;
            cfg.Options.AutoConnectOnLaunch = autoc.IsOn;
            var secrets = _store.LoadSecrets(cfg.Id) ?? new TunnelSecrets();
            if (!string.IsNullOrWhiteSpace(user.Text)) secrets.OpenVpn["username"] = user.Text.Trim();
            if (!string.IsNullOrEmpty(pass.Password)) secrets.OpenVpn["password"] = pass.Password;
            _store.SaveSecrets(cfg.Id, secrets);
            _store.Save(cfg);
            _editorForId = Guid.Empty;
            LoadTunnels();
            SelectTab(TabOverview);
        };
        EditorHost.Children.Add(save);
    }

    private static int? ParseInt(TextBox? t) => t is not null && int.TryParse(t.Text.Trim(), out var v) ? v : null;
    private static List<string> SplitList(string s) =>
        s.Split(new[] { ',', ' ' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();
    private static List<IpAddressRange> ParseRanges(string s) =>
        SplitList(s).Select(IpAddressRange.Parse).Where(r => r is not null).Select(r => r!.Value).ToList();
    private static string Fmt(IpAddressRange a) => $"{a.AddressString}/{a.Prefix}";

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
        _helperReachable = reachable;
        HelperStatus.Text = reachable ? Loc.T("Helper: connected") : Loc.T("Helper: not reachable");
        HelperDot.Fill = new SolidColorBrush(reachable ? Colors.MediumSeaGreen : Colors.Gray);
        HelperBar.IsOpen = !reachable;   // offer to install/start the system component

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
        SelectTab(TabEditor); // open the editor for the new tunnel
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

    // Auto-check the public IP once the tunnel is up. A split tunnel (no default route) doesn't
    // carry general traffic, so a generic IP-check can't run through it — show a hint instead of
    // a misleading value (mirrors the macOS "not routed" behaviour).
    private async Task CheckExternalIpAsync(TunnelConfig cfg)
    {
        string result;
        if (!cfg.HasDefaultRoute)
        {
            result = Loc.T("split tunnel — not routed");
        }
        else
        {
            try { result = (await _http.GetStringAsync("https://api.ipify.org")).Trim(); }
            catch { result = Loc.T("check failed (blocked or no route)"); }
        }
        _externalIp[cfg.Id] = result;
        if (Selected?.Config.Id == cfg.Id) OvExternalIp.Text = result;
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

    private Task CheckConflictsAsync()
    {
        var findings = _store.CheckAllConflicts();
        var body = new StackPanel { Spacing = 8, Padding = new Thickness(16) };

        if (findings.Count == 0)
        {
            body.Children.Add(new TextBlock { Text = Loc.T("No conflicts found"), Foreground = Secondary });
        }
        else
        {
            // Group by code, most severe first (mirrors the macOS ConflictsSheet grouping).
            foreach (var group in findings.GroupBy(f => f.Code)
                                          .OrderByDescending(g => g.Max(f => f.Severity)))
            {
                var items = group.ToList();
                var sev = items.Max(f => f.Severity);

                var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
                header.Children.Add(new FontIcon { Glyph = SeverityGlyph(sev), FontSize = 15, Foreground = SeverityBrush(sev) });
                header.Children.Add(new TextBlock { Text = group.Key, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
                if (items.Count > 1)
                    header.Children.Add(new Border
                    {
                        Background = new SolidColorBrush(Color.FromArgb(40, 128, 128, 128)),
                        CornerRadius = new CornerRadius(8), Padding = new Thickness(6, 0, 6, 0),
                        Child = new TextBlock { Text = $"×{items.Count}", FontSize = 11 }
                    });

                var content = new StackPanel { Spacing = 6 };
                foreach (var f in items)
                {
                    var block = new StackPanel { Spacing = 1 };
                    block.Children.Add(new TextBlock { Text = f.Message, TextWrapping = TextWrapping.Wrap });
                    if (f.TunnelNames.Count > 0)
                        block.Children.Add(new TextBlock { Text = string.Join(" ↔ ", f.TunnelNames), FontSize = 12, Foreground = Secondary });
                    if (f.FixHint is { Length: > 0 })
                        block.Children.Add(new TextBlock { Text = "→ " + f.FixHint, FontSize = 12, Foreground = (Brush)Application.Current.Resources["AccentTextFillColorPrimaryBrush"], TextWrapping = TextWrapping.Wrap });
                    content.Children.Add(block);
                }

                body.Children.Add(new Expander
                {
                    Header = header,
                    Content = content,
                    IsExpanded = items.Count == 1,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    HorizontalContentAlignment = HorizontalAlignment.Stretch
                });
            }
        }
        OpenContentWindow(Loc.T("Check all tunnels"), body, 580, 560);
        return Task.CompletedTask;
    }

    private async Task ShowLogsAsync()
    {
        var lines = await _daemon.RecentLogAsync(500);
        var list = new StackPanel { Padding = new Thickness(12) };
        if (lines.Count == 0)
            list.Children.Add(new TextBlock { Text = "—", Foreground = Secondary });
        foreach (var l in lines)
        {
            var row = new TextBlock
            {
                FontFamily = new FontFamily("Consolas"), FontSize = 12, TextWrapping = TextWrapping.Wrap,
                Text = $"{l.Time.LocalDateTime:HH:mm:ss} [{l.Level}] {l.Category}: {l.Message}",
                Foreground = l.Level.Equals("error", StringComparison.OrdinalIgnoreCase) ? SeverityBrush(FindingSeverity.Error)
                           : l.Level.Equals("warn", StringComparison.OrdinalIgnoreCase) ? SeverityBrush(FindingSeverity.Warning)
                           : (Brush)Application.Current.Resources["TextFillColorPrimaryBrush"]
            };
            list.Children.Add(row);
        }
        var sv = new ScrollViewer { Content = list };
        OpenContentWindow(Loc.T("Logs"), sv, 760, 520);
    }

    private void OpenContentWindow(string title, FrameworkElement content, int width, int height)
    {
        var win = new Window { Title = title };
        var host = content is ScrollViewer ? content : new ScrollViewer { Content = content };
        host.SetValue(Grid.RowProperty, 0);
        win.Content = new Grid { Children = { host } };
        try
        {
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(win);
            var id = Win32Interop.GetWindowIdFromWindow(hwnd);
            AppWindow.GetFromWindowId(id).Resize(new Windows.Graphics.SizeInt32(width, height));
        }
        catch { }
        win.Activate();
    }

    private static string SeverityGlyph(FindingSeverity s) => s switch
    {
        FindingSeverity.Error => "", // ErrorBadge
        FindingSeverity.Warning => "", // Warning
        _ => "" // Info
    };
    private static Brush SeverityBrush(FindingSeverity s) => new SolidColorBrush(s switch
    {
        FindingSeverity.Error => Colors.IndianRed,
        FindingSeverity.Warning => Colors.Orange,
        _ => Colors.SteelBlue
    });

    private async Task InstallHelperAsync()
    {
        // Register + start the TunHubHelper Windows service. Creating a service needs
        // elevation, so we launch an elevated sc.exe (UAC prompt) — the macOS equivalent of the
        // one-time administrator-password prompt.
        var exe = Path.Combine(AppContext.BaseDirectory, "tunhub-helper.exe");
        if (!File.Exists(exe)) { await MessageAsync(Loc.T("Errors"), $"tunhub-helper.exe not found next to the app ({exe})"); return; }
        var script = $"sc.exe create TunHubHelper binPath= \"\\\"{exe}\\\"\" start= auto & sc.exe start TunHubHelper";
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = "cmd.exe", Arguments = $"/c {script}",
                UseShellExecute = true, Verb = "runas", CreateNoWindow = true
            };
            System.Diagnostics.Process.Start(psi);
        }
        catch (Exception ex) { await MessageAsync(Loc.T("Errors"), ex.Message); }
        await Task.Delay(1500);
        await PollAsync();
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
        (string label, Color bg, Color fg) = config.Kind switch
        {
            TunnelKind.WireGuard => ("WG", Color.FromArgb(255, 0xE6, 0xF1, 0xFB), Color.FromArgb(255, 0x18, 0x5F, 0xA5)),
            TunnelKind.AmneziaWg => ("AWG", Color.FromArgb(255, 0xFA, 0xEE, 0xDA), Color.FromArgb(255, 0x85, 0x4F, 0x0B)),
            TunnelKind.OpenVpn   => ("OVPN", Color.FromArgb(255, 0xE1, 0xF5, 0xEE), Color.FromArgb(255, 0x0F, 0x6E, 0x56)),
            _ => ("?", Colors.LightGray, Colors.Black)
        };
        KindLabel = label;
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
