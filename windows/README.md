# TunHub for Windows (native WinUI 3)

A native Windows client for running multiple AmneziaWG / WireGuard / **OpenVPN** tunnels,
with a native Fluent (WinUI 3) UI. It **reuses the shared C# logic** — no duplication of the
models, parsers or engine:

```
TunHub.WinUI            WinUI 3 (Fluent) UI + tray — the only Windows-specific code
   │  IPC — newline-JSON over a Unix domain socket (AF_UNIX; Windows 10 1809+)
TunHub.Helper           Windows Service (LocalSystem)
   └── TunHub.Engine    supervisor, UAPI, OpenVPN management client, WindowsPlatform
         ├── amneziawg-go / wireguard-go (Wintun)
         └── openvpn.exe (driven via its management interface)
TunHub.Core             models, wg-quick + .ovpn parsers, conflict checker, keys
```

The app-side services (`DaemonClient`, `AppStore`, `ImportService`, localization) live under
`src/TunHub.WinUI/Services` and contain no UI-framework code — they only depend on
`TunHub.Core` / `TunHub.Engine`, the exact same libraries the helper uses.

macOS stays on the native Swift app (`../..` → `TunHub/`). This folder is Windows-only.

## UI parity with macOS

The window mirrors the macOS app: a sidebar of tunnels (status dot + kind badge WG/AWG/OVPN),
a detail pane with **Overview / Editor / Status** tabs (endpoint, server-pushed routes,
external IP, live traffic), an accent **Start**, an `InfoBar` with **Retry / re-enter
credentials** on failure, a connect-time **username / password / OTP** dialog for OpenVPN,
**Settings** with launch-at-login, import of `.conf` / `.ovpn` / `.zip`, export, conflict
check and logs. A **system-tray icon** (the menu-bar equivalent) offers Open / Stop all /
Quit; closing the window hides it to the tray, and Quit asks whether to disconnect first.

## Build (on Windows)

Prerequisites: **.NET 8 SDK**, the **Windows App SDK** workload, **Go 1.21+**, git.
WinUI 3 builds only on Windows.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The script builds the Go cores for windows/amd64, fetches `wintun.dll`, downloads and
extracts the OpenVPN core, stamps the build, publishes the WinUI app + helper (self-contained)
into `dist\TunHub`, bundles third-party license texts into `dist\TunHub\licenses\`, and builds
an **MSI** (`dist\TunHub-0.8.1-win-x64.msi`) via WiX that installs the app, registers the
`TunHubHelper` service and adds a Start-menu shortcut. Set `SKIP_MSI=1` for just the portable
folder.

OpenVPN core: the official community MSI (`openvpn-latest-stable-amd64.msi`) is downloaded and
administratively extracted (`msiexec /a`, no install) to stage `openvpn.exe` + its DLLs. Override
with `OPENVPN_MSI` (a different MSI URL) or `OPENVPN_ZIP` (a portable zip), or pre-populate
`.cores\openvpn\`. OpenVPN is GPLv2 and shipped unmodified as a separate process — see
[`../THIRD-PARTY-NOTICES.md`](../THIRD-PARTY-NOTICES.md).

The shared libraries (`TunHub.Core`, `TunHub.Engine`) also build and unit-test on macOS/Linux
(`dotnet test tests/TunHub.Core.Tests`) — only the WinUI UI layer requires Windows.

## Status

Done: shared Core + Engine (incl. OpenVPN parser/models, management-interface client and
session), Windows platform layer (Wintun/UAPI, netsh routes, **fail-closed kill switch** via
Windows Firewall, **split-DNS via NRPT**), the full WinUI UI + tray, and the WiX MSI.

Needs a Windows machine to finalize: first WinUI compile/run, real tunnel bring-up, verifying
the Wintun adapter name / UAPI named-pipe path against the actual Go Windows cores, tightening
the IPC socket ACL, and Authenticode signing of the MSI.

## Layout

```
TunHub.Windows.sln
build.ps1
installer/TunHub.wxs            WiX MSI authoring
src/TunHub.Core/                models, parsers (wg-quick + .ovpn), conflict checker, keys
src/TunHub.Engine/              supervisor, UAPI, OpenVpn/, Platforms/WindowsPlatform.cs
src/TunHub.Helper/              Windows Service host
src/TunHub.WinUI/               App + MainWindow + tray + Services/ (+ Assets/TunHub.ico)
tests/TunHub.Core.Tests/        xUnit (parsers, conflict checker) — 24 tests
```
