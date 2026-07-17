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
        // Apply the saved interface language before any UI is built.
        Loc.Apply(AppSettings.Load().Language);
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        // Closing the window hides it to the tray instead of exiting (menu-bar-style app).
        _window.HideToTrayRequested += () => _window?.AppWindowHide();
        InitTray();
        _window.Activate();
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
            IconSource = new BitmapImage(new Uri("ms-appx:///Assets/TunHub.ico")),
            LeftClickCommand = new RelayCommand(ShowWindow)
        };
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
