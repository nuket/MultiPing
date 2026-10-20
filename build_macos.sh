#!/usr/bin/env bash
# MIT License
#
# MultiPing
# Copyright (c) 2026 Max Vilimpoc
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Builds MultiPing.app with the Xcode command line tools: one universal executable (arm64 and
# x86_64; macOS 11 and later, the first for Apple silicon and the oldest the SDK's libc++ still
# supports) from MultiPingMac.mm, the icon made from MultiPing.ico's largest image, and an ad-hoc
# signature, which Apple silicon needs to run it.

set -euo pipefail
cd "$(dirname "$0")"

# The revision under the copyright: MULTIPING_GIT_REVISION when the caller knows it
# (systems-build.py, building a tree synced without its .git), else this checkout's.
GIT_REV=${MULTIPING_GIT_REVISION:-$(git rev-parse --short HEAD 2>/dev/null || echo unknown)}

APP=MultiPing.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

clang++ -std=c++17 -fobjc-arc -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 \
    -DGIT_REV="\"$GIT_REV\"" MultiPingMac.mm -o "$APP/Contents/MacOS/MultiPing" -framework Cocoa

# The icon: the .ico's largest PNG image, scaled to an iconset, made into an .icns.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
python3 - MultiPing.ico "$SCRATCH/icon.png" <<'EOF'
import struct, sys
data = open(sys.argv[1], "rb").read()
count = struct.unpack("<H", data[4:6])[0]
best = None
for i in range(count):
    w, _, _, _, _, _, size, offset = struct.unpack("<BBBBHHII", data[6 + 16 * i:22 + 16 * i])
    if data[offset:offset + 4] == b"\x89PNG" and (best is None or (w or 256) > best[0]):
        best = (w or 256, data[offset:offset + size])
open(sys.argv[2], "wb").write(best[1])
EOF
ICONSET="$SCRATCH/MultiPing.iconset"
mkdir "$ICONSET"
for size in 16 32 128 256; do
    sips -z $size $size "$SCRATCH/icon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    [ $double -le 256 ] && sips -z $double $double "$SCRATCH/icon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/MultiPing.icns"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>               <string>MultiPing</string>
    <key>CFBundleDisplayName</key>        <string>MultiPing</string>
    <key>CFBundleIdentifier</key>         <string>org.vilimpoc.MultiPing</string>
    <key>CFBundleExecutable</key>         <string>MultiPing</string>
    <key>CFBundleIconFile</key>           <string>MultiPing</string>
    <key>CFBundlePackageType</key>        <string>APPL</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>NSHumanReadableCopyright</key>   <string>Copyright © 2026 Max Vilimpoc. Build from git $GIT_REV.</string>
    <key>LSMinimumSystemVersion</key>     <string>11.0</string>
    <key>NSHighResolutionCapable</key>    <true/>
    <key>NSPrincipalClass</key>           <string>NSApplication</string>
</dict>
</plist>
EOF

codesign --force --sign - "$APP"
echo "Built $APP, $(du -sk "$APP" | cut -f1) KB, $(lipo -archs "$APP/Contents/MacOS/MultiPing")"
