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

## Status

Done:

- `CMakeLists.txt` — MSVC toolchain, static CRT, LTCG/OPT:REF/ICF for size, warnings at `/W4`.
- `core/str` — UTF-8 ↔ UTF-16, trimming/splitting, base64, hex, human-readable sizes.
- `core/json` — dependency-free JSON DOM. Sorted keys when pretty-printing so config diffs
  stay stable; compact output for IPC.
- `core/ipaddr` — CIDR parsing/normalisation via the system resolver, masking,
  `contains`/`overlaps` (needed by the conflict checker and route logic).
- `core/wgkey` — base64 ↔ hex for UAPI, plus vendored X25519 for keypair generation and
  public-key derivation. CSPRNG via BCrypt.
- `core/models.h` — full model layer including AmneziaWG H1–H4 / S3 / S4 / I1–I5, split-DNS
  semantics and the resolved-spec/runtime-state contracts.

Next, in order:

1. `core/models.cpp`, `wgquick_parser.cpp`, `ovpn_parser.cpp`, `conflict_checker.cpp`
   (+ unit tests mirroring the existing 24 C# tests).
2. `engine/` — UAPI client over the AmneziaWG/WireGuard named pipes, tunnel supervisor with
   the stats loop and startup orphan reaping, routes/NRPT DNS/firewall kill switch, OpenVPN
   management-interface client, bounded `FileLog`.
3. `helper/` — LocalSystem service, named-pipe IPC with a security descriptor.
4. `app/` — tray, tunnel list, WG/AWG and OpenVPN editors, live log window, settings,
   import, traffic graph.
5. Packaging (WiX MSI) + CI job, then retire `windows/` (.NET).

## Note on verification

This code is written on macOS and cannot be compiled here — it needs MSVC. The CI job on
`windows-2022` is what will first compile it, so expect an initial round of compiler fixes
before the tree builds clean.
