# TunHub

A native macOS menu-bar app (Apple Silicon) for running multiple **AmneziaWG** and
**WireGuard** tunnels at the same time. Import `.conf` files and ZIP archives, catch
DNS/route conflicts before they break your network, keep secrets in the Keychain, and
watch per-tunnel statistics, health checks, failover and a kill switch.

A single userspace core, `amneziawg-go` (v0.2.x), handles both AmneziaWG 2.0 and the
older 1.5 protocol; plain WireGuard tunnels use `wireguard-go`.

## Requirements

- Apple Silicon Mac, macOS 13+
- Xcode or Command Line Tools with Swift 5.9+
- Go 1.21+ (`brew install go`) — only needed to build the cores

## Build

```bash
./build.sh                                        # dev build (ad-hoc signature)
IDENTITY="Developer ID Application: …" ./build.sh # signed build
```

The script clones and builds `amneziawg-go` and `wireguard-go` for darwin/arm64,
compiles the Swift targets, assembles `dist/TunHub.app`, and signs every Mach-O.

To produce an installable disk image:

```bash
./make-dmg.sh    # → dist/TunHub.dmg (drag-to-Applications installer)
```

## First run

1. Move `TunHub.app` to `/Applications`.
2. Launch TunHub. On first start it offers to install the privileged helper
   (a classic LaunchDaemon in `/Library/LaunchDaemons`); approve the single
   administrator-password prompt.
3. Import configs: drag & drop a `.conf` or `.zip` onto the window, or use the
   toolbar Import button.

Left-click the menu-bar icon to open the main window; right-click for the quick menu.

## Architecture

```
TunHub.app (SwiftUI menu bar + windows)
   │  XPC (com.tunhub.daemon.xpc)
tunhubd (LaunchDaemon, root)
   ├── amneziawg-go / wireguard-go processes (one per tunnel, on utun)
   ├── RouteManager   (/sbin/route; default route via the 0.0.0.0/1 + 128.0.0.0/1 pair; endpoint pinning)
   ├── DNSManager     (split → SCDynamicStore match domains; global → networksetup on the primary service)
   ├── FirewallManager (pf anchor com.tunhub — kill switch)
   └── stats loop     (UAPI get=1 every 0.5s)
```

- Secrets (PrivateKey/PSK) live only in the Keychain (one combined item per tunnel,
  service `com.tunhub.secrets`); the JSON configs on disk contain no secrets.
- The conflict checker blocks a start on ERROR findings (two default routes,
  overlapping addresses, two global-DNS tunnels, etc.) and surfaces WARNINGs.
- `PostUp`/`PreDown` scripts from imported configs are parsed and stored but **never
  executed** — by design, for safety.
- All app identity (bundle IDs, mach service, on-disk paths, core binary names) lives
  in one place: `Sources/TunHubShared/Constants.swift`.

### Interface ownership

macOS assigns `utunN` names itself and does not allow renaming them, so TunHub cannot
give its interfaces a custom label. Instead every core process is stamped with a
`TUNHUB_OWNER` environment variable and recorded in an ownership registry
(`/var/db/tunhub/owned.json`) mapping utun ↔ tunnel ↔ pid ↔ core binary. Crash
recovery only ever touches processes it can positively verify as ours — it never
signals a reused PID or another app's WireGuard/Amnezia process.

## Production signing

For a non-ad-hoc build, set your Team ID in `Sources/TunHubShared/Constants.swift`
(`TunHub.teamID`). A non-empty value enables the XPC code-signing requirement so the
daemon only accepts connections from your signed app. Then notarize:

```bash
IDENTITY="Developer ID Application: …" ./build.sh
ditto -c -k --keepParent dist/TunHub.app dist/TunHub.zip
xcrun notarytool submit dist/TunHub.zip --keychain-profile … --wait
xcrun stapler staple dist/TunHub.app
```

## Localization

The base language is English; strings are keyed by their English text in
`Resources/en.lproj` / `Resources/ru.lproj`. The in-app language picker (Settings)
lists whatever `.lproj` bundles are present and persists the choice across restarts.
Add a language by dropping a new `.lproj/Localizable.strings` into `Resources/`.

## Debugging

- Daemon log stream: `log stream --predicate 'subsystem == "com.tunhub.daemon"'`
- Daemon log file: `/var/log/tunhub-daemon.log`; state in `/var/db/tunhub/`
- UAPI sockets: `/var/run/wireguard/*.sock`, `/var/run/amneziawg/*.sock`
- Manual teardown after a crash:
  `sudo pkill -f amneziawg-go; sudo pkill -f wireguard-go; sudo pfctl -f /etc/pf.conf`

## Project layout

```
Package.swift            SwiftPM: TunHubShared, TunHubApp, tunhubd
build.sh                 full arm64 build (cores + app bundle)
make-dmg.sh              package the .app into dist/TunHub.dmg
Resources/               Info.plist, launchd plists, *.lproj strings
Sources/TunHubShared/    Constants, models, CIDR math, .conf parser, ConflictChecker, XPC
Sources/tunhubd/         supervisor, UAPI, route/DNS/pf, XPC service
Sources/TunHubApp/       AppState, Keychain, import/export, health/failover, SwiftUI views
TunHub-design.md         design document (milestones M0–M7)
```

## License

MIT — see [LICENSE](LICENSE).
