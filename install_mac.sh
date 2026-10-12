#!/bin/bash
# Install the built plugin where DaVinci Resolve looks for OpenFX plugins.
set -euo pipefail
cd "$(dirname "$0")"

BUNDLE=build/mac/Emulsion.ofx.bundle
DEST=/Library/OFX/Plugins

if [ ! -f "$BUNDLE/Contents/MacOS/Emulsion.ofx" ]; then
    echo "The plugin is not built yet. Run: bash build_mac.sh"
    exit 1
fi

echo "Installing Emulsion into $DEST"
echo "(macOS will ask for your login password; nothing is shown while you type it.)"
sudo mkdir -p "$DEST"
sudo rm -rf "$DEST/Emulsion.ofx.bundle"
sudo cp -R "$BUNDLE" "$DEST/"
sudo xattr -dr com.apple.quarantine "$DEST/Emulsion.ofx.bundle" 2>/dev/null || true

echo ""
echo "Installed. Now:"
echo "  1. Quit DaVinci Resolve completely and open it again."
echo "  2. On the Color page, open Effects (OpenFX) and look for Emulsion > Emulsion Film."
