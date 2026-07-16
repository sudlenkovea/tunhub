# TunHub for Windows (native WinUI 3)

A native Windows client for running multiple AmneziaWG/WireGuard tunnels, with a native
Fluent (WinUI 3) UI. It **reuses the shared C# logic** already written for the
cross-platform tree — no duplication of the model/parser/engine:

```
TunHub.WinUI            WinUI 3 (Fluent) UI — the only Windows-specific code here
   │  IPC — newline-JSON over a Unix domain socket (AF_UNIX; Windows 10 1809+)
TunHub.Helper           Windows Service (SYSTEM)          ← reused from ../avalonia
   └── TunHub.Engine    supervisor, UAPI, WindowsPlatform ← reused from ../avalonia
         └── amneziawg-go / wireguard-go (Wintun)
TunHub.Core             models, wg-quick parser, conflict checker, keys ← reused
```

The app-side services (`DaemonClient`, `AppStore`, `ImportService`, localization) are
linked in from `../avalonia/src/TunHub.App/Services` — they contain no UI-framework code,
so both the Avalonia experiment and this native WinUI app share them.

macOS stays on the native Swift app (`../TunHub`). This folder is Windows-only.

## Build (on Windows)

Prerequisites: **.NET 8 SDK**, the **.NET Desktop / Windows App SDK** workload, **Go 1.21+**,
git. WinUI 3 builds only on Windows.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The script builds the Go cores for windows/amd64, fetches `wintun.dll`, stamps the build,
publishes the WinUI app + the helper (self-contained), and bundles everything into
`dist\TunHub`. It prints the `sc.exe create` command to register the privileged helper
service. Then run `dist\TunHub\TunHub.exe`.

In Visual Studio you can also just open `TunHub.Windows.sln` and F5 the `TunHub.WinUI`
project (set the platform to x64).

## Status / TODO

Done: the WinUI 3 UI (tunnel list + status dots + detail pane + import + settings/language),
wired to the shared engine over IPC; the build script.

TODO (Windows platform internals live in `../avalonia/src/TunHub.Engine/Platforms/WindowsPlatform.cs`):

- **WFP kill switch** (currently a no-op with a warning).
- **Split-DNS via NRPT** (currently sets the adapter DNS).
- Verify the **Wintun adapter naming** and the exact **UAPI named-pipe path** against the
  real `wireguard-go`/`amneziawg-go` Windows builds.
- IPC socket **ACL** (currently world-accessible for dev; tighten to an explicit ACL).
- **MSI (WiX)** packaging + Authenticode signing.

## Layout

```
TunHub.Windows.sln
build.ps1
src/TunHub.WinUI/        App.xaml(.cs), MainWindow.xaml(.cs), app.manifest
                        (+ linked shared services from ../avalonia)
```
