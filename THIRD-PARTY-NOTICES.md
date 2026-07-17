# Third-party notices

TunHub itself is MIT-licensed. It ships several third-party tunnel cores as **separate,
unmodified executables** that it launches as child processes (no static/dynamic linking into
TunHub) — this is "mere aggregation", so bundling them does not change TunHub's own license.

The full license **text** of each component is included with every build under `licenses/`
(inside `TunHub.app/Contents/Resources/licenses/` on macOS; next to the app on Windows) —
this is required by both the MIT and GPLv2 licenses. Corresponding **source code** for the
GPLv2 components is the unmodified upstream release, available at the links below; on request
we will also provide it per GPLv2 §3.

| Component | Role | License | Source |
|-----------|------|---------|--------|
| wireguard-go | WireGuard userspace core | MIT | https://git.zx2c4.com/wireguard-go |
| amneziawg-go | AmneziaWG userspace core (wireguard-go fork) | MIT | https://github.com/amnezia-vpn/amneziawg-go |
| Wintun (Windows) | Windows TUN adapter (`wintun.dll`) | GPLv2 | https://git.zx2c4.com/wintun / https://www.wintun.net/ |
| OpenVPN (community) | OpenVPN core (`openvpn` / `openvpn.exe` + libs) | GPLv2 | https://github.com/OpenVPN/openvpn / https://openvpn.net/community-downloads/ |

The OpenVPN build additionally depends on OpenSSL (Apache-2.0), LZO (GPLv2), lz4 (BSD) and
pkcs11-helper (BSD/GPL); their notices ship with the OpenVPN license files under
`licenses/openvpn-*`.

Bundled binaries are downloaded from the official upstream release channels by the build
scripts (`build.sh` on macOS, `windows/build.ps1` on Windows) and are not modified.
