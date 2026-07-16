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
    private Dictionary<Guid, TunnelRuntimeState> _runtime = new();

    public MainWindow()
    {
        InitializeComponent();
        Title = "TunHub";

        TunnelList.ItemsSource = _items;
        Localize();

        ImportBtn.Click += async (_, _) => await ImportAsync();
        StopAllBtn.Click += async (_, _) => await SafeAsync(() => _daemon.StopAllAsync());
        SettingsBtn.Click += async (_, _) => await ShowSettingsAsync();
        StartBtn.Click += async (_, _) => await StartSelectedAsync();
        StopBtn.Click += async (_, _) => await StopSelectedAsync();
        DeleteBtn.Click += (_, _) => DeleteSelected();
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
        ImportBtn.Content = Loc.T("Import…");
        StopAllBtn.Content = Loc.T("Stop all");
        SettingsBtn.Content = Loc.T("Settings");
        StartBtn.Content = Loc.T("Start");
        StopBtn.Content = Loc.T("Stop");
        DeleteBtn.Content = Loc.T("Delete");
        EmptyHint.Text = Loc.T("No tunnels — import a .conf or ZIP");
    }

    private void LoadTunnels()
    {
        _items.Clear();
        foreach (var t in _store.LoadAll()) _items.Add(new TunnelItem(t));
        EmptyHint.Visibility = _items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (_items.Count > 0 && TunnelList.SelectedItem is null) TunnelList.SelectedIndex = 0;
        UpdateDetail();
    }

    private void UpdateDetail()
    {
        var item = Selected;
        var visible = item is not null ? Visibility.Visible : Visibility.Collapsed;
        DetailName.Visibility = DetailKind.Visibility = DetailStatus.Visibility =
            DetailTraffic.Visibility = DetailDot.Visibility = StartBtn.Visibility =
            StopBtn.Visibility = DeleteBtn.Visibility = visible;
        if (item is null) return;

        var cfg = item.Config;
        DetailName.Text = cfg.Name;
        DetailKind.Text = cfg.Kind.Label();
        _runtime.TryGetValue(cfg.Id, out var s);
        var phase = s?.Phase ?? TunnelPhase.Stopped;
        DetailStatus.Text = $"{Loc.T("Status")}: {PhaseText(phase)}";
        DetailDot.Fill = PhaseBrush(phase);
        DetailTraffic.Text = s is null ? "" :
            $"rx {ByteFormat.Human(s.RxTotal)}   tx {ByteFormat.Human(s.TxTotal)}";
        var running = phase is TunnelPhase.Up or TunnelPhase.Degraded or TunnelPhase.Starting;
        StartBtn.IsEnabled = !running;
        StopBtn.IsEnabled = running;
    }

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
            item.StatusText = PhaseText(phase);
            item.StatusBrush = PhaseBrush(phase);
        }
        UpdateDetail();
    }

    private async Task StartSelectedAsync()
    {
        if (Selected is null) return;
        try
        {
            var spec = _store.ResolveSpec(Selected.Config);
            await _daemon.StartTunnelAsync(spec);
        }
        catch (Exception ex) { DetailStatus.Text = ex.Message; }
        await PollAsync();
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

    private async Task ImportAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".conf");
        picker.FileTypeFilter.Add(".zip");
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);

        var files = await picker.PickMultipleFilesAsync();
        if (files is null || files.Count == 0) return;
        var result = ImportService.ImportPaths(files.Select(f => f.Path), _store);
        LoadTunnels();
        if (result.Errors.Count > 0) DetailStatus.Text = string.Join("; ", result.Errors);
    }

    private async Task ShowSettingsAsync()
    {
        var settings = AppSettings.Load();
        var lang = new ComboBox { Header = Loc.T("Interface language") };
        foreach (var (_, native) in Loc.Available) lang.Items.Add(native);
        var codes = Loc.Available.Select(a => a.Code).ToList();
        lang.SelectedIndex = Math.Max(0, codes.IndexOf(settings.Language));

        var kill = new ToggleSwitch { Header = Loc.T("Kill switch (global)"), IsOn = settings.KillSwitchGlobal };
        var note = new TextBlock
        {
            Text = Loc.T("Restart to apply the language."),
            Foreground = new SolidColorBrush(Colors.Gray),
            Visibility = Visibility.Collapsed
        };

        var panel = new StackPanel { Spacing = 12 };
        panel.Children.Add(lang);
        panel.Children.Add(note);
        panel.Children.Add(kill);

        lang.SelectionChanged += (_, _) =>
        {
            if (lang.SelectedIndex >= 0) { settings.Language = codes[lang.SelectedIndex]; note.Visibility = Visibility.Visible; }
        };

        var dialog = new ContentDialog
        {
            Title = Loc.T("Settings"),
            Content = panel,
            PrimaryButtonText = "OK",
            XamlRoot = Root.XamlRoot
        };
        await dialog.ShowAsync();

        settings.KillSwitchGlobal = kill.IsOn;
        settings.Save();
        await SafeAsync(() => _daemon.SetKillSwitchAsync(settings.KillSwitchGlobal));
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

/// <summary>List row: tunnel + live status (bindable).</summary>
public sealed class TunnelItem : INotifyPropertyChanged
{
    public TunnelConfig Config { get; }
    public string Name { get; }

    public TunnelItem(TunnelConfig config)
    {
        Config = config;
        Name = config.Name;
    }

    private string _statusText = "";
    public string StatusText
    {
        get => _statusText;
        set { if (_statusText != value) { _statusText = value; Changed(nameof(StatusText)); } }
    }

    private Brush _statusBrush = new SolidColorBrush(Colors.Gray);
    public Brush StatusBrush
    {
        get => _statusBrush;
        set { _statusBrush = value; Changed(nameof(StatusBrush)); }
    }

    public event PropertyChangedEventHandler? PropertyChanged;
    private void Changed(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}
