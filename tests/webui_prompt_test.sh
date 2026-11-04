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
python3 - "$T" <<'PY'
import json, pathlib, subprocess, sys
root = pathlib.Path(sys.argv[1])
files = [root / "prompt.txt", root / "state.json", root / "карта с пробелом.json"]
startup = dict(type="begin_loading", mapName="startup")
level = dict(type="begin_loading", mapName="a2_pistol", chapter="Глава", facingAngle=42.5, loadingText="ЗАГРУЗКА")
end = dict(type="end_loading")
def replay(messages):
    for file in files:
        file.unlink(missing_ok=True)
    data = "\n".join(m if isinstance(m, str) else json.dumps(m, ensure_ascii=False) for m in messages) + "\n"
    subprocess.run([str(root / "vxrt-helper"), "webui-replay", *map(str, files)], input=data, text=True,
                   check=True, stdout=subprocess.PIPE)
    return json.loads(files[2].read_text()) if files[2].exists() else None, json.loads(files[1].read_text())
assert replay([startup])[0] is None
assert replay([startup, startup])[0] is None
value, state = replay([startup, end, level])
assert value == dict(level, messageText="", waitingForInput=False) and state["phase"] == "loading"
assert replay([level])[0] == value
other = dict(level, mapName="a2_headcrabs_tunnel")
assert replay([level, other])[0]["mapName"] == other["mapName"]
for text in ["Press Trigger To Start", "Чтобы начать, нажмите: Trigger"]:
    value, state = replay([level, dict(type="show_message", text=text)])
    assert value["messageText"] == text and value["waitingForInput"] and state["phase"] == "waiting"
value, state = replay([level, dict(type="show_message", text="Saving...")])
assert value["messageText"] == "Saving..." and not value["waitingForInput"]
assert replay([startup, end, startup])[0]["mapName"] == "startup"
for clear in [end, "gone"]:
    assert replay([level, clear])[0] is None
assert replay([level, "not json", dict(type="unknown")])[0]["mapName"] == level["mapName"]
print("webui map test: startup, later/replaced maps, localized readiness, end/gone and UTF-8 paths -> ok")
PY
