#!/usr/bin/env bash
# Clones Klepton (MIT, https://github.com/shinyquagsire23/Klepton) at a pinned
# commit into NativeARM/build/Klepton, applies the one-line link fix its
# Makefile needs on this Xcode (-framework CoreAudio), and builds klepton-ld.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${1:-$SCRIPT_DIR/../build/Klepton}"
COMMIT="${KLEPTON_COMMIT:-ce74582}"
command -v brew >/dev/null && { brew list --versions pkg-config sdl3 >/dev/null 2>&1 || brew install -q pkg-config sdl3; }
if [[ ! -d "$DEST/.git" ]]; then
    git clone -q https://github.com/shinyquagsire23/Klepton.git "$DEST"
fi
git -C "$DEST" fetch -q origin
git -C "$DEST" checkout -q "$COMMIT"
if ! grep -q 'framework CoreAudio' "$DEST/Makefile"; then
    sed -i '' 's/^LDLIBS  := -lz -framework AudioToolbox \\$/LDLIBS  := -lz -framework AudioToolbox -framework CoreAudio \\/' "$DEST/Makefile"
fi
make -C "$DEST" mvk >/dev/null 2>&1 || true
make -C "$DEST" build/klepton-ld
echo "klepton-ld: $DEST/build/klepton-ld"
