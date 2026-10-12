#!/bin/bash
# Build the Emulsion OFX plugin for macOS.
#   ./build_mac.sh                      -> builds for this Mac (Apple Silicon or Intel)
#   ARCHS="arm64 x86_64" ./build_mac.sh -> universal build for both
set -euo pipefail
cd "$(dirname "$0")"

if ! xcrun --find clang++ >/dev/null 2>&1; then
    echo "Xcode Command Line Tools are needed to build the plugin."
    echo "Install them with:   xcode-select --install"
    echo "then run: bash build_mac.sh"
    exit 1
fi

# Apple Silicon is detected directly (uname -m says x86_64 if Terminal runs under Rosetta).
if [ -z "${ARCHS:-}" ]; then
    if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = "1" ]; then ARCHS=arm64; else ARCHS=x86_64; fi
fi
ARCHFLAGS=""
for a in $ARCHS; do ARCHFLAGS="$ARCHFLAGS -arch $a"; done

OFX=third_party/openfx
OUT=build/mac
BUNDLE="$OUT/Emulsion.ofx.bundle"
MINOS="-mmacosx-version-min=11.0"
CXXFLAGS="-std=c++17 -O3 -DNDEBUG -fvisibility=hidden -fvisibility-inlines-hidden $MINOS $ARCHFLAGS \
 -I$OFX/include -I$OFX/Support/include -I$OFX/Support/Library -Isrc"

rm -rf "$OUT"
mkdir -p "$OUT/obj"

echo "Building Emulsion for: $ARCHS"
echo "Compiling (takes about a minute; the film looks are built in)..."
SOURCES=(src/EmulsionPlugin.cpp src/emulsion_core.cpp "$OFX"/Support/Library/ofxs*.cpp)
pids=()
for f in "${SOURCES[@]}"; do
    o="$OUT/obj/$(basename "${f%.cpp}").o"
    xcrun clang++ $CXXFLAGS -w -c "$f" -o "$o" &
    pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
if [ "$fail" -ne 0 ]; then
    echo "Build failed (see the errors above)."
    exit 1
fi

echo "Linking..."
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
printf '_OfxGetPlugin\n_OfxGetNumberOfPlugins\n' > "$OUT/exports.txt"
xcrun clang++ $ARCHFLAGS $MINOS -bundle -fvisibility=hidden \
    -exported_symbols_list "$OUT/exports.txt" \
    "$OUT"/obj/*.o -o "$BUNDLE/Contents/MacOS/Emulsion.ofx"

cat > "$BUNDLE/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>Emulsion.ofx</string>
    <key>CFBundleIdentifier</key>
    <string>com.emulsionfilm.Emulsion</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>Emulsion</string>
    <key>CFBundlePackageType</key>
    <string>BNDL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleSignature</key>
    <string>????</string>
</dict>
</plist>
PLIST

# Ad-hoc signature (required for code on Apple Silicon; no Apple developer account needed).
codesign --force --sign - "$BUNDLE"

echo ""
echo "Built: $BUNDLE"
file "$BUNDLE/Contents/MacOS/Emulsion.ofx" | sed 's/^/  /'
echo ""
echo "Next: bash install_mac.sh   (copies it into /Library/OFX/Plugins; asks for your Mac password)"
