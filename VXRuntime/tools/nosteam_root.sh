#!/bin/bash
# nosteam_root.sh <root> [<game folder>]: a VXRT_ROOT that is a Windows PC without Steam (NOSTEAM.md).
#   - makes the root's drive_c folders when the checks pass (the runtime makes the rest, and registry.txt with its defaults, at first start);
#   - checks that nothing in it points steam_api64.dll at a Steam client: no HKCU\Software\Valve\Steam (ActiveProcess:
#     pid, SteamClientDll64) in registry.txt, no steamclient64.dll in the root's system32 or in the game's folders;
#   - prints the environment and the command line to start the game with (no VXRT_STEAM_BRIDGE, no SteamAppId).
# Nothing is written into <game folder>: it is read through VXRT_OVERLAY, whose writes go to <root>/overlay_game.
# Exit status 1 when a check fails (the root then is not Steam-less).
set -u
ROOT=${1:?usage: nosteam_root.sh <root> [<game folder>]}
GAME=${2:-}
fail=0
if [ -f "$ROOT/registry.txt" ]; then
  if grep -qi '^key HKEY_CURRENT_USER\\Software\\Valve\\Steam' "$ROOT/registry.txt"; then
    echo "FAIL: $ROOT/registry.txt has Steam's keys:"; grep -i '^key .*\\Valve\\Steam' "$ROOT/registry.txt" | sed 's/^/  /'; fail=1
  else echo "ok: registry.txt has no HKCU\\Software\\Valve\\Steam"; fi
else
  echo "ok: no registry.txt yet (the runtime writes its defaults at first start, without Steam's keys unless VXRT_STEAM_BRIDGE=1)"
fi
for f in "$ROOT/drive_c/windows/system32/steamclient64.dll" "$ROOT/drive_c/windows/system32/steamclient.dll"; do
  [ -e "$f" ] && { echo "FAIL: $f exists"; fail=1; }
done
[ $fail = 0 ] && mkdir -p "$ROOT/drive_c/windows/system32" "$ROOT/drive_c/windows/temp" "$ROOT/drive_c/users/vxrt/AppData/Local" \
         "$ROOT/drive_c/users/vxrt/AppData/Roaming" "$ROOT/drive_c/users/vxrt/Saved Games" "$ROOT/drive_c/users/vxrt/Documents"
if [ -n "$GAME" ]; then
  hits=$(find "$GAME" -maxdepth 3 -iname 'steamclient*.dll' 2>/dev/null)
  [ -n "$hits" ] && { echo "FAIL: a Steam client next to the game (steam_api loads it first):"; echo "$hits" | sed 's/^/  /'; fail=1; } \
                 || echo "ok: no steamclient*.dll in the game's folders"
  exe=$(find "$GAME/bin/x64" -maxdepth 1 -iname 'Cyberpunk2077.exe' 2>/dev/null | head -1)
  echo
  echo "start (cwd: the exe's folder):"
  echo "  cd \"$GAME/bin/x64\""
  echo "  env -u VXRT_STEAM_BRIDGE -u SteamAppId -u SteamGameId VXRT_ROOT=\"$ROOT\" VXRT_OVERLAY=\"$GAME:$ROOT/overlay_game\" \\"
  echo "      VXRT_DLL_PATH=<redist> VXRT_AOT_DIR=<modules> VXRT_AOT_STRICT=1 VXRT_NPU_INTERP=race \\"
  echo "      vxrt ${exe:+$(basename "$exe")} --launcher-skip -skipStartScreen"
fi
exit $fail
