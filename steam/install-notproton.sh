#!/bin/bash
set -euo pipefail
if (( $# != 1 )); then echo 'Usage: bash steam/install-notproton.sh "/path/to/Half-Life Alyx"' >&2; exit 1; fi
game_dir="$(cd "$1" && pwd)"
if [[ ! -f "$game_dir/start.command" || ! -f "$game_dir/verantyx/alyx_host" || ! -f "$game_dir/verantyx/bridge" ]]; then
 echo 'Install the MoltenVR native runtime and start.command first.' >&2; exit 1
fi
case "$game_dir" in *$'\n'*) echo 'Newlines in installation paths are unsupported.' >&2; exit 1 ;; esac
source_dir="$(cd "$(dirname "$0")" && pwd)/alyx-native-moltenvr"
dest="$HOME/Library/Application Support/Steam/compatibilitytools.d/alyx-native-moltenvr"
if [[ -e "$dest" ]]; then
 backup="$dest.bak.$(date +%Y%m%d-%H%M%S).$$"
 cp -R "$dest" "$backup"
 echo "Previous integration backed up: $backup"
fi
mkdir -p "$dest"
cp "$source_dir/compatibilitytool.vdf" "$source_dir/toolmanifest.vdf" "$source_dir/run" "$dest/"
printf '%s\n' "$game_dir" > "$dest/game-path.txt"
chmod +x "$dest/run"
echo 'Installed Alyx Native - MoltenVR. Restart macOS Steam, then select it in Alyx > Properties > Compatibility.'
echo 'NotProton must already be installed to expose Steam Play on macOS.'
