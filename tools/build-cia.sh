#!/bin/sh
# Builds nDJess.cia (installable with FBI) from the compiled nDJess.elf.
# Needs makerom and bannertool on PATH, or set MAKEROM / BANNERTOOL to their paths.
set -e
cd "$(dirname "$0")/.."
MAKEROM="${MAKEROM:-makerom}"
BANNERTOOL="${BANNERTOOL:-bannertool}"
[ -f nDJess.elf ] || { echo "Run make first (nDJess.elf is missing)"; exit 1; }
mkdir -p build
"$BANNERTOOL" makebanner -i assets/banner.png -a assets/banner.wav -o build/banner.bnr
"$BANNERTOOL" makesmdh -s "nDJess" -l "3DS DJ Mixer" -p "Nova White" -i icon.png -o build/icon.icn
"$MAKEROM" -f cia -o nDJess.cia -elf nDJess.elf -rsf tools/ndjess.rsf \
    -icon build/icon.icn -banner build/banner.bnr -exefslogo -target t
echo "Built nDJess.cia"
