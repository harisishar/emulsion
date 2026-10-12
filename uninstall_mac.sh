#!/bin/bash
# Remove the plugin from /Library/OFX/Plugins.
set -euo pipefail
if [ -d /Library/OFX/Plugins/Emulsion.ofx.bundle ]; then
    sudo rm -rf /Library/OFX/Plugins/Emulsion.ofx.bundle
    echo "Emulsion removed. Restart DaVinci Resolve."
else
    echo "Emulsion isn't installed in /Library/OFX/Plugins."
fi
