#!/usr/bin/env bash
# The web UI stand-in's prompt file (scripts/vxrt_helper.swift, webui-replay): Half-Life: Alyx's messages to its
# loading page around a level load, in the order the game sends them (logs/webui-standin.log), and what the file
# says after each: nothing while loading, the plain-words prompt once the game shows "Press Trigger To Start", nothing
# again at end_loading (the game goes on), a new load, or the game quitting. Other messages show as they are.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/webui-prompt-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
swiftc -O "$ROOT/scripts/vxrt_helper.swift" -o "$T/vxrt-helper"
fail() { echo "webui prompt test failed: $*" >&2; exit 1; }

got="$("$T/vxrt-helper" webui-replay "$T/prompt.txt" <<'MESSAGES'
{"type": "set_paths", "webroot": "Z:\\game\\hlvr\\webui", "models": "Z:\\game\\hlvr\\models\\ui\\"}
{"type": "begin_loading", "mapName": "a1_intro_world_2", "chapter": "", "tipText": "", "description": ""}
{"type": "show_message", "text": "Press Trigger To Start"}
{"type": "end_loading"}
{"type": "begin_loading", "mapName": "a2_pistol", "chapter": "", "tipText": "", "description": ""}
{"type": "show_message", "text": "Press Trigger To Start"}
{"type": "begin_loading", "mapName": "a2_headcrabs_tunnel", "chapter": "", "tipText": "", "description": ""}
{"type": "show_message", "text": "Saving..."}
{"type": "show_message", "text": ""}
{"type": "show_message", "text": "press trigger to start "}
gone
not json
MESSAGES
)"
want="(none)
(none)
Hold the trigger to start
(none)
(none)
Hold the trigger to start
(none)
Saving...
(none)
Hold the trigger to start
(none)
(none)"
[[ "$got" == "$want" ]] || fail "$(diff <(echo "$want") <(echo "$got"))"
[[ ! -e "$T/prompt.txt" ]] || fail "the file is left after the game quit"
echo "webui prompt test: show_message, end_loading, begin_loading, the game quitting -> ok"
