#!/usr/bin/env bash
# Compiles every pipeline in Half-Life: Alyx's shipped Fossilize database
# (game/hlvr/vulkan_shader_cache.foz — the one Steam replays on Linux) through
# MoltenVK once, so the Metal shader cache already holds them when the game
# asks. Without it each new material compiles on the game's render thread the
# first time it is seen (250-500 ms per pipeline was measured: the hitches
# when characters or new areas appear). Metal's cache is per user, so the
# game process reuses what this fills.
#
#   NativeARM/scripts/prewarm_shaders.sh          # skips if already done
#   NativeARM/scripts/prewarm_shaders.sh --force
#
# Done-marker: <game root>/.shader_prewarm (database size/mtime + MoltenVK).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
GAME_ROOT="${ALYX_GAME_ROOT:-$HOME/Verantyx_VR_Drive/alyx_linux}"
DB="${ALYX_FOSSILIZE_DB:-$GAME_ROOT/game/hlvr/vulkan_shader_cache.foz}"
REPLAY="$NATIVE_DIR/build/Fossilize/build-mac/cli/fossilize-replay"
MVK_DIR="${KL_MVK_DIR:-$NATIVE_DIR/build/Klepton/vendor-moltenvk/out/macos}"
MARKER="$GAME_ROOT/.shader_prewarm"
THREADS="${PREWARM_THREADS:-$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || sysctl -n hw.ncpu)}"

[[ -f "$DB" ]] || { echo "No pipeline database at $DB; nothing to prewarm."; exit 0; }
[[ -x "$REPLAY" ]] || "$SCRIPT_DIR/build_fossilize.sh"
key="$(stat -L -f '%z %m' "$DB") $(stat -L -f '%z %m' "$MVK_DIR/libMoltenVK.dylib")"
if [[ "${1:-}" != "--force" && -f "$MARKER" && "$(cat "$MARKER")" == "$key" ]]; then
    exit 0
fi

# fossilize-replay loads "libvulkan.1.dylib"; point it at MoltenVK, the same
# library and settings the game runs on, so Metal sees identical shaders.
LIBDIR="$(mktemp -d)"
trap 'rm -rf "$LIBDIR"' EXIT
ln -s "$MVK_DIR/libMoltenVK.dylib" "$LIBDIR/libvulkan.1.dylib"
ln -s "$MVK_DIR/libMoltenVK.dylib" "$LIBDIR/libvulkan.dylib"
echo "Preparing shaders once (all pipelines of the game, $THREADS threads; a few minutes)..."
start=$(date +%s)
DYLD_LIBRARY_PATH="$LIBDIR" MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION=1 MVK_CONFIG_LOG_LEVEL=1 \
    "$REPLAY" --num-threads "$THREADS" "$DB" 2>&1 \
    | grep --line-buffered -E "Replayed|graphics pipelines:|compute pipelines:|rror" || true
echo "$key" > "$MARKER"
echo "Shaders prepared in $(( $(date +%s) - start )) s."
