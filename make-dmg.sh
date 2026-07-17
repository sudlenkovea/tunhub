#!/usr/bin/env bash
# Build TunHub.app and package it into a ready-to-install .dmg (Apple Silicon).
#
#   ./make-dmg.sh                       # dev-signed build → dist/TunHub.dmg
#   IDENTITY="Developer ID Application: …" ./make-dmg.sh   # release build
#
# The .dmg contains TunHub.app plus an /Applications shortcut: the user just drags
# the app onto Applications. Note: the daemon still needs a one-time install on first
# launch (onboarding "Install system component", or sudo ./install-daemon.sh).
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="TunHub"
DIST="dist"
DMG="$DIST/$APP_NAME.dmg"
STAGE="$DIST/dmg-stage"
VOLNAME="$APP_NAME"

echo "==> Building app…"
./build.sh

[[ -d "$DIST/$APP_NAME.app" ]] || { echo "error: $DIST/$APP_NAME.app not found"; exit 1; }

echo "==> Staging DMG contents…"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$DIST/$APP_NAME.app" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

echo "==> Creating compressed .dmg…"
# Detach any stale volume left mounted from a previous run — a mounted UDRW image is the
# usual cause of hdiutil's "Resource busy".
for v in /Volumes/"$VOLNAME"*; do
    [[ -d "$v" ]] && hdiutil detach "$v" -force >/dev/null 2>&1 || true
done
rm -f "$DMG"
# One-step compressed (UDZO) image straight from the staging folder — no intermediate
# read-write image is created or mounted, which avoids the "Resource busy" flakiness.
tries=0
until hdiutil create -volname "$VOLNAME" -srcfolder "$STAGE" \
        -fs HFS+ -format UDZO -ov "$DMG" >/dev/null 2>&1; do
    tries=$((tries + 1))
    [[ $tries -ge 3 ]] && { echo "error: hdiutil create failed after $tries attempts"; exit 1; }
    echo "    hdiutil busy — retrying in 3s ($tries/3)…"
    sleep 3
done
rm -rf "$STAGE"

# Sign the disk image if a real identity is provided.
if [[ "${IDENTITY:-}" != "" && "${IDENTITY:-}" != "-" && "${IDENTITY}" != "TunHub Dev" ]]; then
    codesign --force --sign "$IDENTITY" "$DMG" && echo "dmg signed"
fi

SIZE="$(du -h "$DMG" | cut -f1)"
echo ""
echo "Done: $DMG ($SIZE)"
echo "Install: open the .dmg, drag TunHub into Applications, launch it, and finish the"
echo "one-time system-component setup shown in the onboarding screen."
