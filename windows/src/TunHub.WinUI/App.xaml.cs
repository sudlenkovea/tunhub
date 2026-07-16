using Microsoft.UI.Xaml;
using TunHub.App.Services;

namespace TunHub.WinUI;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        // Apply the saved interface language before any UI is built.
        Loc.Apply(AppSettings.Load().Language);
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow();
        _window.Activate();
    }
}
