# TunHub

Run several **AmneziaWG**, **WireGuard** and **OpenVPN** tunnels at the same time, with a
native app on each platform:

- **macOS** (Apple Silicon) — a SwiftUI menu-bar app + a root `LaunchDaemon`.
- **Windows** — a native WinUI 3 (Fluent) app + a `TunHubHelper` service, sharing a
  common C# core (see [`windows/`](windows/)).

Import `.conf`, `.ovpn` and ZIP archives, catch DNS/route conflicts before they break your
network, keep secrets in the OS secret store, and watch per-tunnel statistics, health
checks, failover and a kill switch.

A single userspace core, `amneziawg-go` (v0.2.x), handles both AmneziaWG 2.0 and the older
1.5 protocol; plain WireGuard uses `wireguard-go`; OpenVPN uses the community `openvpn`
binary driven over its management interface.

## Tunnel types

| Type | Core | Auth |
|------|------|------|
| WireGuard | `wireguard-go` | keys |
| AmneziaWG | `amneziawg-go` (I1–I5 / S1–S4 / H1–H4 obfuscation) | keys |
| OpenVPN | `openvpn` (community 2.6/2.7) | certificate, user/password, **static-challenge OTP** |

OpenVPN tunnels are driven entirely through the management interface: credentials and
one-time codes are entered at connect time (or saved), routes and DNS pushed by the server
are shown live, and the profile's script directives (`up`/`down`/`route-up`/`tls-verify`/…)
are parsed but **never executed** — the same safety policy as wg-quick `PostUp`/`PreDown`.

## macOS

### Requirements

- Apple Silicon Mac, macOS 13+
- Xcode or Command Line Tools with Swift 5.9+
- Go 1.21+ (`brew install go`) — only to build the cores
- OpenVPN core is staged from Homebrew (`brew install openvpn`) and made self-contained with
  `dylibbundler`; both are fetched automatically by `build.sh` if missing

### Build

```bash
./build.sh                                        # dev build (ad-hoc signature)
IDENTITY="Developer ID Application: …" ./build.sh # signed build
./make-dmg.sh                                     # → dist/TunHub.dmg (drag-to-Applications)
```

`build.sh` clones and builds `amneziawg-go` and `wireguard-go` for darwin/arm64, stages and
bundles the `openvpn` core into `Contents/Frameworks`, compiles the Swift targets, assembles
`dist/TunHub.app`, and signs every Mach-O.

### First run

1. Move `TunHub.app` to `/Applications`.
2. Launch TunHub. On first start it offers to install the privileged helper (a classic
   LaunchDaemon in `/Library/LaunchDaemons`); approve the single administrator-password prompt.
3. Import configs: drag & drop a `.conf`, `.ovpn` or `.zip` onto the window, or use the
   toolbar Import button.

Left-click the menu-bar icon to open the main window; right-click for the quick menu. The
window shows a Dock icon while open and hides it when closed. **Settings → Launch at login**
starts TunHub with the system; tunnels marked *Connect on app launch* connect automatically.
On quit, if any tunnel is still connected, TunHub asks whether to disconnect first.

### Architecture

```
TunHub.app (SwiftUI menu bar + windows)
   │  XPC (com.tunhub.daemon.xpc)
tunhubd (LaunchDaemon, root)
   ├── amneziawg-go / wireguard-go processes (one per tunnel, on utun)
   ├── openvpn process (per tunnel; driven via its management socket)
   ├── RouteManager   (/sbin/route; default route via 0.0.0.0/1 + 128.0.0.0/1; endpoint pinning)
   ├── DNSManager     (split → SCDynamicStore match domains; global → networksetup)
   ├── FirewallManager (pf anchor com.tunhub — kill switch)
   └── stats loop     (UAPI get=1 / management bytecount every 0.5s)
```

- Secrets (WireGuard PrivateKey/PSK, OpenVPN inline key material and username/password) live
  only in the Keychain, in a **single shared vault item** (service `com.tunhub.secrets`,
  account `vault`) that holds every tunnel's secrets. One item means macOS asks to authorize
  access at most once per app build, not once per tunnel. JSON configs on disk contain no
  secrets; `.ovpn` secret blocks are redacted to `##SECRET:tag##` placeholders and re-inlined
  only in memory at connect time.
- The conflict checker blocks a start on ERROR findings (two default routes, overlapping
  addresses, two global-DNS tunnels, etc.) and surfaces WARNINGs.
- All app identity (bundle IDs, mach service, on-disk paths, core binary names) lives in
  `Sources/TunHubShared/Constants.swift`.

#### Interface ownership

macOS assigns `utunN` names itself and doesn't allow renaming, so every core process is
stamped with a `TUNHUB_OWNER` env var and recorded in an ownership registry
(`/var/db/tunhub/owned.json`) mapping utun ↔ tunnel ↔ pid ↔ core. Crash recovery only ever
touches processes it can positively verify as ours.

### Production signing

For a non-ad-hoc build, set your Team ID in `Sources/TunHubShared/Constants.swift`
(`TunHub.teamID`) — a non-empty value enables the XPC code-signing requirement so the daemon
only accepts connections from your signed app. Then notarize:

```bash
IDENTITY="Developer ID Application: …" ./build.sh
ditto -c -k --keepParent dist/TunHub.app dist/TunHub.zip
xcrun notarytool submit dist/TunHub.zip --keychain-profile … --wait
xcrun stapler staple dist/TunHub.app
```

## Windows

A native WinUI 3 app that reuses the shared C# core (`TunHub.Core` / `TunHub.Engine`). It
mirrors the macOS UI (sidebar with status dots + kind badges, Overview/Editor/Status tabs,
OpenVPN OTP dialog, tray icon, launch-at-login) and ships as an MSI that registers the
`TunHubHelper` service. Build on Windows:

```powershell
cd windows
powershell -ExecutionPolicy Bypass -File .\build.ps1   # → dist\TunHub-0.8.1-win-x64.msi
```

See [`windows/README.md`](windows/README.md) for details. The shared `TunHub.Core` /
`TunHub.Engine` libraries also build and unit-test on macOS/Linux
(`dotnet test windows/tests/TunHub.Core.Tests`).

## Continuous integration

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds both platforms on every
push to `main` and every `v*` tag: macOS produces `TunHub.dmg`, Windows runs the Core/Engine
unit tests and produces the MSI. Both are uploaded as workflow artifacts.

## Localization

Base language is English; strings are keyed by their English text in `Resources/en.lproj` /
`Resources/ru.lproj` (macOS) and `Services/Localization.cs` (Windows). The in-app language
picker persists the choice. Add a language by dropping a new `.lproj/Localizable.strings`.

## Debugging (macOS)

- Daemon log stream: `log stream --predicate 'subsystem == "com.tunhub.daemon"'`
- Daemon log file: `/var/log/tunhub-daemon.log`; state in `/var/db/tunhub/`
- UAPI sockets: `/var/run/wireguard/*.sock`, `/var/run/amneziawg/*.sock`
- Manual teardown after a crash:
  `sudo pkill -f amneziawg-go; sudo pkill -f wireguard-go; sudo pkill -f openvpn; sudo pfctl -f /etc/pf.conf`

## Project layout

```
Package.swift            SwiftPM: TunHubShared, TunHubApp, tunhubd (macOS)
build.sh / make-dmg.sh   macOS build + .dmg packaging
make-icon.sh             generate the app icon
Resources/               Info.plist, launchd plists, *.lproj strings, AppIcon.icns
Sources/TunHubShared/    Constants, models, CIDR math, .conf + .ovpn parsers, ConflictChecker, XPC
Sources/tunhubd/         supervisor, UAPI, OpenVPN management, route/DNS/pf, XPC service
Sources/TunHubApp/       AppState, Keychain, import/export, health/failover, SwiftUI views
windows/                 native WinUI 3 app + shared C# core/engine + WiX MSI (Windows)
.github/workflows/       CI: macOS DMG + Windows MSI
TunHub-design.md         design document (milestones M0–M7)
```

## License

MIT — see [LICENSE](LICENSE).
