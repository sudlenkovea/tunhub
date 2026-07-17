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

    /// <summary>Shared daemon client used by the tray menu (the window has its own instance).</summary>
    public static DaemonClient Daemon { get; } = new();

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
        var open = new MenuFlyoutItem { Text = Loc.T("Open window") };
        open.Click += (_, _) => ShowWindow();
        var stop = new MenuFlyoutItem { Text = Loc.T("Stop all") };
        stop.Click += async (_, _) => { try { await Daemon.StopAllAsync(); } catch { } };
        var quit = new MenuFlyoutItem { Text = Loc.T("Quit TunHub") };
        quit.Click += async (_, _) => await QuitAsync();

        var menu = new MenuFlyout();
        menu.Items.Add(open);
        menu.Items.Add(stop);
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(quit);

        _tray = new TaskbarIcon
        {
            ToolTipText = "TunHub",
            ContextFlyout = menu,
            LeftClickCommand = new RelayCommand(ShowWindow)
        };
        // Icon is best-effort: a bad URI must not throw out of ForceCreate.
        try { _tray.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/TunHub.ico")); }
        catch (Exception ex) { Log("tray-icon", ex); }
        _tray.ForceCreate();
    }

    private void ShowWindow()
    {
        if (_window is null) return;
        _window.AppWindowShow();
        _window.Activate();
    }

    /// <summary>Quit; if any tunnel is connected, ask whether to disconnect first (macOS parity).</summary>
    private async Task QuitAsync()
    {
        try
        {
            var states = await Daemon.RuntimeStatesAsync();
            var anyRunning = states.Any(s => s.Phase is TunnelPhase.Up or TunnelPhase.Degraded or TunnelPhase.Starting);
            if (anyRunning && _window?.Content?.XamlRoot is { } root)
            {
                ShowWindow();
                var dialog = new ContentDialog
                {
                    Title = Loc.T("Disconnect all tunnels before quitting?"),
                    Content = Loc.T("Some tunnels are still connected. You can disconnect them now or leave them running."),
                    PrimaryButtonText = Loc.T("Disconnect and quit"),
                    SecondaryButtonText = Loc.T("Quit, keep running"),
                    CloseButtonText = Loc.T("Cancel"),
                    DefaultButton = ContentDialogButton.Primary,
                    XamlRoot = root
                };
                var result = await dialog.ShowAsync();
                if (result == ContentDialogResult.None) return;                 // Cancel
                if (result == ContentDialogResult.Primary)
                    try { await Daemon.StopAllAsync(); } catch { }
            }
        }
        catch { /* fall through to exit */ }
        _tray?.Dispose();
        Exit();
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
