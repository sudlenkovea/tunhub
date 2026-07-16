#!/usr/bin/env bash
# Generate Resources/AppIcon.icns — a bright blue rounded-square with a white shield
# (same shield style as the menu-bar icon, just brighter/filled). Run once; the .icns is
# committed and bundled by build.sh.
set -euo pipefail
cd "$(dirname "$0")"

TMP="$(mktemp -d)"
SWIFT="$TMP/draw.swift"
cat > "$SWIFT" <<'SW'
import AppKit

let size: CGFloat = 1024
let img = NSImage(size: NSSize(width: size, height: size))
img.lockFocus()

// Rounded-square background with a vivid blue gradient (macOS icon corner radius ≈ 22.37%).
let rect = NSRect(x: 0, y: 0, width: size, height: size)
let clip = NSBezierPath(roundedRect: rect, xRadius: size * 0.2237, yRadius: size * 0.2237)
clip.addClip()
let grad = NSGradient(colors: [
    NSColor(srgbRed: 0.26, green: 0.62, blue: 1.00, alpha: 1),   // bright top
    NSColor(srgbRed: 0.06, green: 0.34, blue: 0.90, alpha: 1)    // deep bottom
])!
grad.draw(in: rect, angle: -90)

// White shield (SF Symbol shield.fill), centered.
let conf = NSImage.SymbolConfiguration(pointSize: size * 0.5, weight: .semibold)
    .applying(NSImage.SymbolConfiguration(paletteColors: [.white]))
if let shield = NSImage(systemSymbolName: "shield.fill", accessibilityDescription: nil)?
    .withSymbolConfiguration(conf) {
    let s = shield.size
    let scale = (size * 0.56) / max(s.width, s.height)
    let dw = s.width * scale, dh = s.height * scale
    shield.draw(in: NSRect(x: (size - dw) / 2, y: (size - dh) / 2, width: dw, height: dh))
}

img.unlockFocus()

guard let tiff = img.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else { exit(1) }
try! png.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
SW

BASE="$TMP/icon_1024.png"
swift "$SWIFT" "$BASE"

ICONSET="$TMP/AppIcon.iconset"
mkdir -p "$ICONSET"
for spec in "16 16x16" "32 16x16@2x" "32 32x32" "64 32x32@2x" \
            "128 128x128" "256 128x128@2x" "256 256x256" "512 256x256@2x" \
            "512 512x512" "1024 512x512@2x"; do
    set -- $spec
    sips -z "$1" "$1" "$BASE" --out "$ICONSET/icon_$2.png" >/dev/null
done

mkdir -p Resources
iconutil -c icns "$ICONSET" -o Resources/AppIcon.icns
rm -rf "$TMP"
echo "wrote Resources/AppIcon.icns"
