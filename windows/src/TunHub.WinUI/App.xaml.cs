using System.Runtime.InteropServices;
using System.Windows.Input;
using H.NotifyIcon;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using TunHub.App.Services;
using TunHub.Core;

namespace TunHub.WinUI;

public partial class App : Application
{
    private MainWindow? _window;
    private TaskbarIcon? _tray;

    /// <summary>Shared daemon client + store used by the tray menu (the window has its own too).</summary>
    public static DaemonClient Daemon { get; } = new();
    public static AppStore Store { get; } = new();
    /// <summary>Latest per-tunnel phase, pushed by the main window's poll — the tray menu reads it.</summary>
    internal static Dictionary<Guid, TunnelPhase> States = new();

    private MenuFlyout? _trayMenu;
    private string _traySig = "";

    /// <summary>Rebuild the tray menu when the tunnel set/statuses change (called from the poll).
    /// Doing it proactively — not on the flyout's Opening event — avoids an empty menu.</summary>
    public void RefreshTray()
    {
        if (_trayMenu is null) return;
        var sig = string.Join(",", SafeTunnels().Select(t =>
            $"{t.Id}:{(States.TryGetValue(t.Id, out var p) ? p : TunnelPhase.Stopped)}"));
        if (sig == _traySig) return;
        _traySig = sig;
        try { RebuildTrayMenu(); } catch (Exception ex) { Log("tray-refresh", ex); }
    }

    public App()
    {
        // Surface any unhandled exception in a visible dialog (and log it) so a silent
        // startup crash is immediately obvious instead of hidden in a log file.
        UnhandledException += (_, e) => { ShowError("UI", e.Exception); e.Handled = true; };
        AppDomain.CurrentDomain.UnhandledException += (_, e) => ShowError("domain", e.ExceptionObject as Exception);
        TaskScheduler.UnobservedTaskException += (_, e) => { Log("task", e.Exception); e.SetObserved(); };

        try { Loc.Apply(AppSettings.Load().Language); } catch (Exception ex) { ShowError("loc", ex); }
        InitializeComponent();
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);

    /// <summary>Show a native error dialog (works even before/without a XAML root) and log it.</summary>
    public static void ShowError(string where, Exception? ex)
    {
        Log(where, ex);
        try
        {
            var msg = ex?.ToString() ?? "unknown error";
            if (msg.Length > 3000) msg = msg.Substring(0, 3000) + "…";
            MessageBoxW(IntPtr.Zero, msg, $"TunHub — error ({where})", 0x10 /* MB_ICONERROR */);
        }
        catch { /* nothing more we can do */ }
    }

    /// <summary>Append a diagnostic line to %LOCALAPPDATA%\TunHub\app.log.</summary>
    public static void Log(string where, Exception? ex)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TunHub");
            Directory.CreateDirectory(dir);
            File.AppendAllText(Path.Combine(dir, "app.log"),
                $"{DateTimeOffset.Now:u} [{where}] {ex}\n");
        }
        catch { /* best effort */ }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            _window = new MainWindow();
            // Closing the window hides it to the tray instead of exiting (menu-bar-style app).
            _window.HideToTrayRequested += () => _window?.AppWindowHide();
            _window.Activate();            // show the window FIRST — never gate it on the tray
        }
        catch (Exception ex) { ShowError("launch (main window)", ex); return; }

        // The tray icon is a nice-to-have; a failure here must not take the app down.
        try { InitTray(); } catch (Exception ex) { ShowError("tray", ex); }
    }

    private void InitTray()
    {
        _trayMenu = new MenuFlyout();
        _tray = new TaskbarIcon
        {
            ToolTipText = "TunHub",
            ContextFlyout = _trayMenu,
            // Classic Win32 popup menu built from the flyout items — reliable for a tray icon
            // (the XAML SecondWindow flyout rendered empty). It invokes each item's Command.
            ContextMenuMode = ContextMenuMode.PopupMenu,
            LeftClickCommand = new RelayCommand(ShowWindow)
        };
        try { _tray.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/TunHub.ico")); }
        catch (Exception ex) { Log("tray-icon", ex); }
        _tray.ForceCreate();
        RebuildTrayMenu();
    }

    /// <summary>Tray menu: Open, the tunnel list (click toggles start/stop, like the macOS menu bar),
    /// Stop all, Quit.</summary>
    private void RebuildTrayMenu()
    {
        if (_trayMenu is null) return;
        _trayMenu.Items.Clear();

        // In PopupMenu mode H.NotifyIcon invokes each item's Command (not Click).
        _trayMenu.Items.Add(new MenuFlyoutItem { Text = Loc.T("Open window"), Command = new RelayCommand(ShowWindow) });
        _trayMenu.Items.Add(new MenuFlyoutSeparator());

        var tunnels = SafeTunnels();
        var anyRunning = false;
        foreach (var t in tunnels)
        {
            var phase = States.TryGetValue(t.Id, out var p) ? p : TunnelPhase.Stopped;
            var running = phase is TunnelPhase.Up or TunnelPhase.Degraded or TunnelPhase.Starting;
            anyRunning |= running;
            var cfg = t; var isRunning = running;
            _trayMenu.Items.Add(new MenuFlyoutItem
            {
                Text = (running ? "● " : "○ ") + t.Name,
                Command = new RelayCommand(() => _ = ToggleTunnelAsync(cfg, isRunning))
            });
        }
        if (tunnels.Count > 0) _trayMenu.Items.Add(new MenuFlyoutSeparator());

        // "Stop all" only when something is actually running (like the main window).
        if (anyRunning)
            _trayMenu.Items.Add(new MenuFlyoutItem
            {
                Text = Loc.T("Stop all"),
                Command = new RelayCommand(() => { try { _ = Daemon.StopAllAsync(); } catch (Exception ex) { Log("tray-stopall", ex); } })
            });
        _trayMenu.Items.Add(new MenuFlyoutItem { Text = Loc.T("Quit TunHub"), Command = new RelayCommand(QuitNow) });
    }

    private static List<TunnelConfig> SafeTunnels()
    {
        try { return Store.LoadAll(); } catch { return new List<TunnelConfig>(); }
    }

    private async Task ToggleTunnelAsync(TunnelConfig t, bool running)
    {
        try
        {
            if (running) { await Daemon.StopTunnelAsync(t.Id); return; }
            // OpenVPN may need credentials/OTP — that flow lives in the window.
            if (t.Kind == TunnelKind.OpenVpn) { ShowWindow(); return; }
            await Daemon.StartTunnelAsync(Store.ResolveSpec(t));
        }
        catch (Exception ex) { Log("tray-toggle", ex); }
    }

    private void ShowWindow()
    {
        if (_window is null) return;
        _window.AppWindowShow();
        _window.Activate();
    }

    /// <summary>Quit for certain. Application.Exit() alone is unreliable while the tray icon and
    /// poll timer keep the message loop alive, so terminate the process.</summary>
    private void QuitNow()
    {
        try { _tray?.Dispose(); } catch { }
        try { _window?.Close(); } catch { }
        Environment.Exit(0);
    }
}

/// <summary>Minimal ICommand for tray click wiring (avoids a full MVVM dependency).</summary>
public sealed class RelayCommand : ICommand
{
    private readonly Action _action;
    public RelayCommand(Action action) => _action = action;
    // Always executable — no state changes to raise, so the event is a no-op stub.
    public event EventHandler? CanExecuteChanged { add { } remove { } }
    public bool CanExecute(object? parameter) => true;
    public void Execute(object? parameter) => _action();
}
