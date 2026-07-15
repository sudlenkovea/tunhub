#!/usr/bin/env bash
# Installs tunhubd as a classic LaunchDaemon (bypassing SMAppService).
# Reliable for dev: stable across rebuilds, no approval in System Settings.
# Run:  sudo ./install-daemon.sh
set -euo pipefail

APP="/Applications/TunHub.app"
LABEL="com.tunhub.daemon"
SRC_PLIST="$(cd "$(dirname "$0")" && pwd)/Resources/com.tunhub.daemon.system.plist"
DST_PLIST="/Library/LaunchDaemons/$LABEL.plist"

[[ $EUID -eq 0 ]] || { echo "root required: sudo ./install-daemon.sh"; exit 1; }
[[ -x "$APP/Contents/MacOS/tunhubd" ]] || { echo "no $APP/Contents/MacOS/tunhubd — run ./build.sh first and copy the .app to /Applications"; exit 1; }

# Remove any SMAppService registration and an old classic daemon.
launchctl bootout system/"$LABEL" 2>/dev/null || true
launchctl bootout system "$DST_PLIST" 2>/dev/null || true

install -m 0644 -o root -g wheel "$SRC_PLIST" "$DST_PLIST"
launchctl bootstrap system "$DST_PLIST"
launchctl enable system/"$LABEL"
launchctl kickstart -k system/"$LABEL"

sleep 1
if launchctl print system/"$LABEL" >/dev/null 2>&1 && pgrep -x tunhubd >/dev/null; then
    echo "✔︎ daemon installed and running (pid $(pgrep -x tunhubd))"
    echo "  log: /var/log/tunhub-daemon.log"
else
    echo "⚠︎ daemon did not come up — check: sudo launchctl print system/$LABEL"
fi
