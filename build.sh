#!/usr/bin/env bash
# TunHub build script — Apple Silicon (arm64) only.
#
# Usage:
#   ./build.sh                       # STABLE self-signed identity (dev) — doesn't break the LaunchDaemon
#   IDENTITY="-" ./build.sh          # ad-hoc (NOT recommended: breaks daemon registration on rebuild)
#   IDENTITY="Developer ID Application: ..." ./build.sh   # production signing (hardened runtime)
#   CONFIG=debug ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="TunHub"
ARCH="arm64"
CONFIG="${CONFIG:-release}"
DEV_IDENTITY_NAME="TunHub Dev"
# Defaults to ad-hoc. The app auto re-registers the daemon when the signature changes
# (see AppState.healDaemonIfNeeded), so a rebuild needs no manual steps.
# For a fully stable signature (no re-registration): ./make-dev-cert.sh, then
# IDENTITY="TunHub Dev" ./build.sh
IDENTITY="${IDENTITY:--}"
DIST="dist"
CORES=".build/cores"
AWG_REPO="${AWG_REPO:-https://github.com/amnezia-vpn/amneziawg-go}"
WG_REPO="${WG_REPO:-https://git.zx2c4.com/wireguard-go}"

command -v swift >/dev/null || { echo "error: swift not found (install Xcode or CLT)"; exit 1; }
[[ "$(uname -m)" == "arm64" ]] || echo "warning: host is not arm64, cross-building"

# --- Stable self-signed code-signing identity (dev) -----------------------
# An ad-hoc signature changes the cdhash on every build → launchd rejects the daemon
# (LWCR mismatch, "spawn failed"). A persistent certificate fixes this: the designated
# requirement = leaf certificate, stable across rebuilds.
ensure_dev_identity() {
    security find-identity -v -p codesigning 2>/dev/null | grep -q "$DEV_IDENTITY_NAME" && return 0
    echo "    creating self-signed certificate “$DEV_IDENTITY_NAME”…"
    local TMP; TMP="$(mktemp -d)"
    cat > "$TMP/cfg" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = $DEV_IDENTITY_NAME
[v3]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
        -days 3650 -config "$TMP/cfg" >/dev/null 2>&1
    openssl pkcs12 -export -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -out "$TMP/id.p12" \
        -passout pass:tunhub -name "$DEV_IDENTITY_NAME" >/dev/null 2>&1
    # -T /usr/bin/codesign: pre-authorize codesign access to the key (no GUI prompt).
    # No -A, so the keychain password dialog doesn't pop up.
    security import "$TMP/id.p12" -k "$HOME/Library/Keychains/login.keychain-db" \
        -P tunhub -T /usr/bin/codesign >/dev/null 2>&1 || true
    rm -rf "$TMP"
}

# Identity choice: the dev certificate if present/creatable; otherwise ad-hoc with a warning.
if [[ "$IDENTITY" == "$DEV_IDENTITY_NAME" ]]; then
    ensure_dev_identity
    if ! security find-identity -v -p codesigning 2>/dev/null | grep -q "$DEV_IDENTITY_NAME"; then
        echo "    ⚠︎ could not create “$DEV_IDENTITY_NAME” (keychain password needed)."
        echo "      Create it once manually:  ./make-dev-cert.sh"
        echo "      Signing ad-hoc for now (the daemon may need reinstalling on rebuild)."
        IDENTITY="-"
    fi
fi

echo "==> [1/4] Building tunnel cores (darwin/$ARCH)"
# Two cores: plain WireGuard + AmneziaWG (amneziawg-go v0.2.18 — the same version the
# official Amnezia client ships; supports ranged magic headers, S3/S4, I1-I5, and is
# backward-compatible with the AmneziaWG 1.5 protocol, so one core covers everything).
AWG_REF="${AWG_REF:-v0.2.18}"
mkdir -p "$CORES"
[[ -d "$CORES/amneziawg-go" ]] || git clone "$AWG_REPO" "$CORES/amneziawg-go"
[[ -d "$CORES/wireguard-go"  ]] || git clone --depth 1 "$WG_REPO"  "$CORES/wireguard-go"
( cd "$CORES/amneziawg-go" && git fetch --tags origin >/dev/null 2>&1 || true
  echo "    AmneziaWG core ($AWG_REF)…"; git checkout -q "$AWG_REF"
  GOOS=darwin GOARCH=$ARCH CGO_ENABLED=0 go build -buildvcs=false -trimpath -ldflags "-s -w" -o ../amneziawg-go.bin . )
( cd "$CORES/wireguard-go"  && GOOS=darwin GOARCH=$ARCH CGO_ENABLED=0 \
    go build -buildvcs=false -trimpath -ldflags "-s -w" -o ../wireguard-go.bin . )

# OpenVPN core (community 2.6.x). We stage the binary and later bundle its non-system
# dylibs (OpenSSL/lzo/lz4/pkcs11-helper) into the .app so it's self-contained. Uses the
# Homebrew build of openvpn as the source binary; dylibbundler makes it relocatable.
if [[ ! -x "$CORES/openvpn" ]]; then
    echo "    OpenVPN core…"
    if ! command -v openvpn >/dev/null && command -v brew >/dev/null; then
        brew list openvpn >/dev/null 2>&1 || brew install openvpn >/dev/null 2>&1 || true
    fi
    OVPN_BIN="$(command -v openvpn || true)"
    [[ -z "$OVPN_BIN" && -x "$(brew --prefix 2>/dev/null)/sbin/openvpn" ]] && OVPN_BIN="$(brew --prefix)/sbin/openvpn"
    if [[ -n "$OVPN_BIN" && -x "$OVPN_BIN" ]]; then
        cp "$OVPN_BIN" "$CORES/openvpn"
    else
        echo "    warning: openvpn binary not found — OpenVPN tunnels won't run (install: brew install openvpn)"
    fi
fi
# dylibbundler makes the openvpn binary self-contained inside the .app.
command -v dylibbundler >/dev/null || { command -v brew >/dev/null && brew install dylibbundler >/dev/null 2>&1 || true; }

echo "==> [2/4] Building Swift targets ($CONFIG, $ARCH)"
# Unique build stamp → app and daemon can tell whether their binaries match.
STAMP="$(date +%Y%m%d%H%M%S)-$(git -C . rev-parse --short HEAD 2>/dev/null || echo nogit)"
cat > Sources/TunHubShared/BuildStamp.swift <<EOF
import Foundation

// AUTO-GENERATED by build.sh — do not edit by hand.
public let kBuildStamp = "$STAMP"

/// Full daemon version: protocol + build stamp. Both parts must match.
public var kDaemonFullVersion: String { "\\(kDaemonProtocolVersion)+\\(kBuildStamp)" }
EOF
echo "    build stamp: $STAMP"
swift build -c "$CONFIG" --arch "$ARCH"
BIN=".build/${ARCH}-apple-macosx/${CONFIG}"

echo "==> [3/4] Assembling ${APP_NAME}.app"
APP="$DIST/$APP_NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Library/LaunchDaemons"
cp "$BIN/TunHubApp"          "$APP/Contents/MacOS/$APP_NAME"
cp "$BIN/tunhubd"            "$APP/Contents/MacOS/tunhubd"
cp "$CORES/amneziawg-go.bin" "$APP/Contents/MacOS/amneziawg-go"
cp "$CORES/wireguard-go.bin" "$APP/Contents/MacOS/wireguard-go"
# OpenVPN core + self-contained dylibs.
if [[ -x "$CORES/openvpn" ]]; then
    cp "$CORES/openvpn" "$APP/Contents/MacOS/openvpn"
    mkdir -p "$APP/Contents/Frameworks"
    if command -v dylibbundler >/dev/null; then
        dylibbundler -of -cd -b -x "$APP/Contents/MacOS/openvpn" \
            -d "$APP/Contents/Frameworks" -p "@executable_path/../Frameworks/" >/dev/null
    else
        echo "    warning: dylibbundler missing — openvpn may not be self-contained"
    fi
fi
cp Resources/App-Info.plist    "$APP/Contents/Info.plist"
[[ -f Resources/AppIcon.icns ]] && cp Resources/AppIcon.icns "$APP/Contents/Resources/"
cp Resources/com.tunhub.daemon.plist        "$APP/Contents/Library/LaunchDaemons/"
cp Resources/com.tunhub.daemon.system.plist  "$APP/Contents/Library/LaunchDaemons/"
# Localizations (en base + ru). SwiftUI Text auto-localizes via these .lproj bundles.
for lproj in Resources/*.lproj; do
    [[ -d "$lproj" ]] || continue
    cp -R "$lproj" "$APP/Contents/Resources/"
done

echo "==> [4/4] Codesigning (identity: $IDENTITY)"
# hardened runtime + timestamp only for a real Developer ID signature.
RUNTIME=()
if [[ "$IDENTITY" != "-" && "$IDENTITY" != "$DEV_IDENTITY_NAME" ]]; then
    RUNTIME=(--options runtime --timestamp)
fi
# Sign bundled dylibs first (inner-out), then the Mach-O executables, then the bundle.
if [[ -d "$APP/Contents/Frameworks" ]]; then
    for dylib in "$APP/Contents/Frameworks"/*.dylib; do
        [[ -e "$dylib" ]] || continue
        codesign --force ${RUNTIME[@]+"${RUNTIME[@]}"} --sign "$IDENTITY" "$dylib"
    done
fi
BINARIES=(amneziawg-go wireguard-go tunhubd "$APP_NAME")
[[ -x "$APP/Contents/MacOS/openvpn" ]] && BINARIES+=(openvpn)
for b in "${BINARIES[@]}"; do
    codesign --force ${RUNTIME[@]+"${RUNTIME[@]}"} --sign "$IDENTITY" "$APP/Contents/MacOS/$b"
done
codesign --force ${RUNTIME[@]+"${RUNTIME[@]}"} --sign "$IDENTITY" "$APP"
codesign --verify --deep "$APP" && echo "codesign: OK"

echo ""
echo "Done: $APP"
echo "Next steps:"
echo "  1. mv \"$APP\" /Applications/          (SMAppService prefers /Applications)"
echo "  2. open /Applications/$APP_NAME.app"
echo "  3. In onboarding click 'Install system component' and approve it in System Settings."
if [[ "$IDENTITY" == "-" ]]; then
    echo "note: ad-hoc signature — local development only; the XPC codesign check is disabled."
fi
