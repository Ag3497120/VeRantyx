#!/usr/bin/env bash
# Builds Valve's Fossilize (MIT, https://github.com/ValveSoftware/Fossilize)
# for macOS: fossilize-replay plays the game's pipeline database through
# MoltenVK so Metal's shader cache is warm before playing
# (scripts/prewarm_shaders.sh). Output: build/Fossilize/build-mac/cli/.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="$SCRIPT_DIR/../build/Fossilize"
COMMIT="${FOSSILIZE_COMMIT:-556da9ff7d7616704dda515756219ab1dab693ec}"
if [[ ! -d "$DIR/.git" ]]; then
    git clone https://github.com/ValveSoftware/Fossilize.git "$DIR"
fi
git -C "$DIR" fetch --depth 1 origin "$COMMIT" 2>/dev/null || true
git -C "$DIR" checkout -q "$COMMIT"
git -C "$DIR" submodule update --init --recursive --depth 1
# macOS has no prctl; the parent-death signal only matters to the Linux
# multi-process replayer, which this build does not use.
python3 - "$DIR/fossilize_external_replayer_linux.hpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if 'VERANTYX_APPLE_PRCTL' not in s:
    s = s.replace('#include <sys/prctl.h>', '''#ifdef __APPLE__
// VERANTYX_APPLE_PRCTL
#define PR_SET_PDEATHSIG 1
static inline int prctl(int, ...) { return 0; }
#else
#include <sys/prctl.h>
#endif''', 1)
    open(p, 'w').write(s)
PY
cmake -S "$DIR" -B "$DIR/build-mac" -DCMAKE_BUILD_TYPE=Release -DFOSSILIZE_VULKAN_LAYER=OFF -DFOSSILIZE_TESTS=OFF -DFOSSILIZE_CLI=ON >/dev/null
cmake --build "$DIR/build-mac" -j "$(sysctl -n hw.ncpu)" --target fossilize-replay fossilize-list >/dev/null
echo "fossilize-replay: $DIR/build-mac/cli/fossilize-replay"
