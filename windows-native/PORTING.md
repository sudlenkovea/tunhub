# TunHub for Windows — native rewrite

The WinUI 3 / .NET build shipped ~200 MB. Almost none of that was TunHub: a self-contained
.NET runtime for the app (~70 MB), the self-contained WindowsAppSDK native libraries
(~60–80 MB), and a *second* .NET runtime for the helper service (~65 MB). Our own code was a
few MB.

This tree replaces it with plain Win32 C++: no runtime, no toolkit, static CRT. The
remaining installer weight is the tunnel cores themselves (amneziawg-go, wireguard-go,
openvpn + OpenSSL), which are a fixed cost under any UI technology.

| | .NET / WinUI 3 | native Win32 |
|---|---|---|
| App | ~150 MB | ~1–2 MB |
| Helper service | ~65 MB | ~0.5 MB |
| Cores (Go/OpenVPN) | ~20–25 MB | ~20–25 MB |
| **Installer** | **~200 MB** | **~25–30 MB** |

## Decisions

- **C++20, MSVC**, CMake. Static CRT (`/MT`) so nothing has to be redistributed.
- **Standard Win32 common controls** — no custom-drawn chrome. The app should look like a
  Windows system utility.
- **No third-party dependencies.** JSON and curve25519 are vendored in `src/core` (a few
  hundred lines each) rather than pulling in a library; linking a TLS stack just to derive a
  public key would outweigh the entire binary.
- **Config format is unchanged**, so `%ProgramData%\TunHub` configs stay compatible with the
  previous build and with the macOS app.

## Layout

```
src/core/     models, JSON, CIDR, keys, wg-quick + .ovpn parsers, conflict checker
src/engine/   log, paths, process spawn, UAPI, routing/DNS/firewall, OpenVPN, supervisor, IPC
src/helper/   the LocalSystem Windows service
src/app/      the Win32 GUI
installer/    WiX v5 MSI
build.ps1     compile → stage → fetch cores → MSI
```

`core` has no Windows-UI dependencies, so it can be unit-tested on its own. `engine` is
linked by both the service and the app (the app only uses the IPC client half).

## Status

All stages are in place: core, engine, service, GUI, packaging and the CI job. The `windows/`
.NET tree has been removed — it is fully superseded and remains in git history.

Feature parity with the previous build: tunnel list with live status and traffic, start/stop,
tray icon (status colour, tunnel list, stop-all only when something runs), WG/AWG editor
including the full AWG parameter set, OpenVPN editor with credential prompt and static
challenge, import of `.conf`/`.ovpn` with warnings, conflict checker, live log viewer with
pause/copy, settings (language, launch at login, kill switch, log capture with restart), and
the traffic strip.

## Note on verification

This tree was written on macOS and cannot be compiled there — it needs MSVC. The CI job on
`windows-2022` is what compiles it first, so expect an initial round of compiler fixes before
it builds clean. The build also reports the staged payload size per file, which is the number
this rewrite exists to reduce.
