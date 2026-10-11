#!/bin/bash
# The dress rehearsal before a person's: the real Glovebox app (no -demo) with its real engine, on
# the Mac's real Xcode and Device Hub, in the states a player's Mac can be in. Signed out of Xcode
# (its teams still cached, as signing out leaves them), signed in with a paid or a free team, no
# headset, each Xcode the Mac could pick, and the keychain. CI only
# (.github/workflows/installer-rehearsal.yml): it rewrites Xcode's settings and xcode-select,
# presses keys and clicks, so never run it on a Mac someone is using.
#
#   rehearsal.sh <Glovebox.app (Debug, built with GLOVEBOX_SRC)> <Glovebox source> <out folder> [<Glovebox.app with the fake engine>] [<Glovebox.dmg>]
#   -> <out>/results.md (a PASS, FAIL or NOTE a line), <out>/*.png, <out>/log.txt, <out>/doctor-*.jsonl
set -u
BUNDLE=$1
SRC=$2
OUT=$3
FAKE=${4:-}
DMG=${5:-}
T="$OUT/tour"
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"
rm -f "$T"/next "$T"/status
R="$OUT/results.md"
LOG="$OUT/log.txt"
: > "$R"
: > "$LOG"
FAILS=0
pass() { echo "- PASS $*" | tee -a "$R"; }
fail() { echo "- FAIL $*" | tee -a "$R"; FAILS=$((FAILS + 1)); }
note() { echo "- NOTE $*" | tee -a "$R"; }
section() { printf '\n## %s\n\n' "$*" | tee -a "$R"; echo "$*" > "$T/phase"; }
log() { echo "$*" | tee -a "$LOG"; }

for tool in display windows stillcmp; do xcrun swiftc -O -o "$T/$tool" "$HERE/$tool.swift" 2>&1 | grep -v warning | head -5; done
"$T/display"
# Keep the display awake for the whole run (its pictures).
caffeinate -d -i -w $$ &
defaults write com.apple.dock autohide -bool true && killall Dock
for k in reduceMotion reduceTransparency; do defaults write com.apple.universalaccess $k -bool false 2>/dev/null; done
sleep 2

ax() { osascript -l JavaScript "$HERE/ax.js" "$@" 2>&1 | head -c 4000; echo; }
mouse() { osascript -l JavaScript "$HERE/mouse.js" "$@" 2>&1; }
key() { osascript -e "tell application \"System Events\" to keystroke \"$1\" using {$2}" 2>&1; }
escape() { osascript -e 'tell application "System Events" to key code 53' >/dev/null 2>&1; }
quitapp() { osascript -e "tell application id \"$1\" to quit" >/dev/null 2>&1; sleep 2; pkill -f "$2" 2>/dev/null; sleep 1; }
hubquit() { pkill -f "DeviceHub|Device Hub" 2>/dev/null; sleep 1; pkill -9 -f "DeviceHub|Device Hub" 2>/dev/null; }
# What a process started, all the way down: "pid command;" each.
descendants() { local c; for c in $(pgrep -P "$1"); do printf '%s %s; ' "$c" "$(ps -o command= -p "$c" | cut -c1-90)"; descendants "$c"; done; }
# Each quit, with what Glovebox still had running, so K can tell which one a background check came from.
noteQuit() { # noteQuit <how>
  local g; for g in $(pgrep -f "Glovebox.app/Contents/MacOS/Glovebox"); do
    echo "$(date +%T) [$(cat "$T/phase" 2>/dev/null)] $1; running: $(descendants "$g")" >> "$OUT/quits.txt"
  done
}
quitall() {
  # Quit as a player does (Glovebox stops what it started), then whatever didn't.
  if pgrep -f "Glovebox.app/Contents/MacOS/Glovebox" >/dev/null; then
    noteQuit quit
    osascript -e 'with timeout of 5 seconds' -e 'tell application id "io.github.glovebox-vr.installer" to quit' -e 'end timeout' >/dev/null 2>&1
    for _ in $(seq 1 8); do pgrep -f "Glovebox.app/Contents/MacOS/Glovebox" >/dev/null || break; sleep 0.5; done
    pkill -9 -f "Glovebox.app/Contents/MacOS/Glovebox" 2>/dev/null
  fi
  pkill -f "glovebox (doctor|build|install)" 2>/dev/null
  pgrep -f "/Contents/MacOS/Xcode" >/dev/null && quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"
  hubquit
  sleep 1
}

# Glovebox to the front the way the Dock does it: on CI the app can't bring itself forward over another app.
gbfront() {
  local p; p=$(pgrep -f "Glovebox.app/Contents/MacOS/Glovebox" | head -1)
  [ -n "$p" ] && osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $p) to true" >/dev/null 2>&1
}

# The app takes one command at a time (Tour.swift) and writes where it stands to status.
cmd() {
  log "  > $*"
  printf '%s\n' "$*" > "$T/next.tmp" && mv "$T/next.tmp" "$T/next"
  for _ in $(seq 1 50); do [ -f "$T/next" ] || return 0; sleep 0.1; done
  log "the app didn't take: $*"
}
status() { cat "$T/status" 2>/dev/null; }
field() { status | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2-; }
stepstate() { field steps | tr ',' '\n' | grep "^$1:" | cut -d: -f2; }
check() { field checks | tr ',' '\n' | grep "^$1:" | cut -d: -f2-; }
waitfor() { # waitfor <pattern> <seconds>
  for _ in $(seq 1 $(( $2 * 5 ))); do status | grep -q -- "$1" && return 0; sleep 0.2; done
  return 1
}
clickring() {
  local at; at=$(field ring)
  [[ "$at" =~ ^[0-9]+,[0-9]+$ ]] || { log "no ring ($(status))"; return 1; }
  mouse "${at%,*}" "${at#*,}" click >/dev/null
}
shot() { screencapture -x "$OUT/$1.png"; }
# The Glovebox window alone: the biggest of its windows on screen.
winshot() {
  local r
  r=$("$T/windows" Glovebox | grep 'layer=0' | sed -E 's/.* x=([0-9.-]+) y=([0-9.-]+) w=([0-9.]+) h=([0-9.]+).*/\3 \4 \1 \2/' \
    | awk '{ printf "%d %d,%d,%d,%d\n", $1 * $2, $3, $4, $1, $2 }' | sort -n | tail -1 | cut -d' ' -f2)
  if [ -n "$r" ]; then screencapture -x -R "$r" "$OUT/$1.png"; else shot "$1"; fi
}
# Every text the guide's bubble (and the app's window) shows, from the accessibility tree.
texts() { ax texts Glovebox | tr '\n' ' '; }
expect() { # expect <scenario> <what> <got> <regex for what's right>
  if [[ "$3" =~ ^($4)$ ]]; then pass "$1: $2 is $3"; else fail "$1: $2 is ${3:-nothing} (should be $4)"; fi
}
has() { # has <scenario> <what> <text> <phrase>
  if [[ "$3" == *"$4"* ]]; then pass "$1: $2 says \"$4\""; else fail "$1: $2 doesn't say \"$4\""; fi
}
hasnt() {
  if [[ "$3" != *"$4"* ]]; then pass "$1: $2 doesn't say \"$4\""; else fail "$1: $2 says \"$4\" (it shouldn't)"; fi
}
run_for() { # run_for <seconds> <command...>: stops it if it hangs (a keychain prompt, say)
  local secs=$1; shift
  "$@" > "$T/run.out" 2>&1 &
  local pid=$!
  for _ in $(seq 1 "$secs"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
  if kill -0 $pid 2>/dev/null; then kill $pid; echo "(still running after ${secs}s: stopped)"; fi
  tail -3 "$T/run.out"
}

# Xcode's account settings, as a player's Mac has them. Signing out of Xcode empties its account
# lists and leaves the teams cached; Xcode 16 can leave an entry with no name in them.
xprefs() { # xprefs none|signed-out|nameless|cached-no-list|signed-in-paid|signed-in-free|signed-in-noteams
  python3 - "$1" <<'PY'
import plistlib, subprocess, sys
mode = sys.argv[1]
out = subprocess.run(["defaults", "export", "com.apple.dt.Xcode", "-"], capture_output=True).stdout
try:
    prefs = plistlib.loads(out)
except Exception:
    prefs = {}
for k in ("IDEProvisioningTeamByIdentifier", "DVTDeveloperAccountManagerAppleIDLists"):
    prefs.pop(k, None)
paid = mode != "signed-in-free"
teams = [{"teamID": "A1B2C3D4E5", "teamName": "Jane Appleseed", "teamType": "Individual", "isFreeProvisioningTeam": not paid}]
if mode not in ("none", "signed-in-noteams"):
    prefs["IDEProvisioningTeamByIdentifier"] = {"8F2C0D6E-0000-4000-8000-000000000001": teams}
if mode.startswith("signed-in"):
    prefs["DVTDeveloperAccountManagerAppleIDLists"] = {"IDE.Prod": [{"identifier": "jane@example.com", "username": "jane@example.com"}],
                                                       "IDE.Identifiers.Prod": ["jane@example.com"]}
elif mode == "signed-out":
    prefs["DVTDeveloperAccountManagerAppleIDLists"] = {"IDE.Prod": [], "IDE.Identifiers.Prod": []}
elif mode == "nameless":
    prefs["DVTDeveloperAccountManagerAppleIDLists"] = {"IDE.Prod": [{"identifier": "", "username": ""}], "IDE.Identifiers.Prod": [""]}
# `defaults import` merges: the keys a mode leaves out have to go first.
for k in ("IDEProvisioningTeamByIdentifier", "DVTDeveloperAccountManagerAppleIDLists"):
    subprocess.run(["defaults", "delete", "com.apple.dt.Xcode", k], capture_output=True)
subprocess.run(["defaults", "import", "com.apple.dt.Xcode", "-"], input=plistlib.dumps(prefs), check=True)
PY
  log "  Xcode accounts: $1 ($(defaults read com.apple.dt.Xcode DVTDeveloperAccountManagerAppleIDLists 2>&1 | tr -s ' \n' ' ' | head -c 160))"
}

# Any password or permission prompt, the whole time: Glovebox must never ask for the Mac's password.
# Nor make macOS offer the Command Line Developer Tools (a shim like xcrun run before Xcode's license
# is accepted does).
watch_prompts() {
  local last="" n=0 blast="" bn=0
  while :; do
    # Notification banners (macOS 27 tells the player when an app runs in the background).
    local b; b=$("$T/windows" NotificationCenter "Notification Center" 2>/dev/null)
    if [ -n "$b" ] && [ "$b" != "$blast" ]; then
      bn=$((bn + 1))
      { echo "== $(date +%T) [$(cat "$T/phase" 2>/dev/null)]"; ax texts com.apple.notificationcenterui; } >> "$OUT/banners.txt"
      [ $bn -le 8 ] && screencapture -x "$OUT/banner-$bn.png"
    fi
    blast=$b
    local w; w=$({ "$T/windows" SecurityAgent UserNotificationCenter "Software Update"; "$T/windows" | grep "owner=Install Command Line"; } 2>/dev/null)
    if [ -n "$w" ] && [ "$w" != "$last" ]; then
      local ts ph; ts=$(date +%T); ph=$(cat "$T/phase" 2>/dev/null)
      while IFS= read -r l; do echo "$ts [$ph] $l"; done <<< "$w" >> "$OUT/prompts.txt"
      { echo "== $ts [$ph]"; ps -axo pid,ppid,etime,command | grep -E "devicectl|xcrun|python|lanprobe|Glovebox|glovebox" | grep -v grep | cut -c1-220; } >> "$OUT/prompt-processes.txt"
      # Who macOS says asked and for what (nehelper's and the network stack's own lines, not the
      # alert's window server noise), and where each Glovebox was at that moment.
      if [ $n -le 3 ] && echo "$w" | grep -q UserNotificationCenter; then
        sudo log show --last 40s --style compact --info --debug --predicate '(process == "nehelper") OR (process == "mDNSResponder" AND eventMessage CONTAINS[c] "Glovebox") OR (subsystem == "com.apple.network" AND process CONTAINS[c] "Glovebox") OR (process != "UserNotificationCenter" AND eventMessage CONTAINS[c] "local network")' \
          2>/dev/null | tail -300 > "$OUT/prompt-log-$n.txt"
        for p in $(pgrep -x Glovebox); do sudo sample "$p" 1 -file "$OUT/prompt-sample-$n-$p.txt" >/dev/null 2>&1; done
      fi
      n=$((n + 1)); [ $n -le 6 ] && screencapture -x "$OUT/prompt-$n.png"
    fi
    last=$w
    sleep 1
  done
}

launch() { # launch <scenario>
  quitall
  rm -f "$T/next" "$T/status"
  open -n "$BUNDLE" --args -tour "$T"
  for _ in $(seq 1 60); do [ -f "$T/status" ] && break; sleep 0.5; done
  [ -f "$T/status" ] || { fail "$1: the app didn't open"; return 1; }
  sleep 2
}

# The checks ran and nothing changed for a few seconds.
settle() { # settle <scenario> [seconds]
  waitfor "checked=1" "${2:-180}" || { fail "$1: the checks didn't finish ($(status))"; return 1; }
  local last="" same=0 s
  for _ in $(seq 1 90); do
    # A failing headset check shows only after 10 s (a headset waking up): until then it isn't settled.
    s="$(field checks) $(field steps) $(field hs)"
    [ "$(field hs)" = settling ] && { same=0; last=$s; sleep 1; continue; }
    if [ "$s" = "$last" ]; then same=$((same + 1)); [ $same -ge 8 ] && break; else same=0; last=$s; fi
    sleep 1
  done
  log "  status: $(status)"
}

DEV=""
GB=io.github.glovebox-vr.installer
doctor() { # doctor <scenario>: the engine on its own, the way the app runs it
  local d; d=$(field dev | sed 's/%20/ /g')
  [ "$d" = "-" ] && d=$DEV
  local t0; t0=$(date +%s)
  DEVELOPER_DIR="$d" xcrun python3 "$SRC/engine/glovebox.py" doctor --json > "$OUT/doctor-$1.jsonl" 2>&1
  DOCTOR_SECS=$(( $(date +%s) - t0 ))
  log "  doctor took ${DOCTOR_SECS}s"
  python3 -c '
import json, sys
for l in open(sys.argv[1]):
    try: e = json.loads(l)
    except ValueError: print("   ", l.rstrip()); continue
    if e.get("event") == "check": print("   ", e["check"], e["status"], "-", e.get("detail"))' "$OUT/doctor-$1.jsonl" | tee -a "$LOG"
}
engine_team() { grep '"check": "team"' "$OUT/doctor-$1.jsonl" | head -1 | python3 -c 'import json,sys; print(json.loads(sys.stdin.read() or "{}").get("status", "none"))'; }

# Two pictures of the window 3 s apart are the same: still art behind, no video.
still() { # still <scenario> <picture>
  winshot "$2"; sleep 3; winshot "$2-later"
  local d; d=$("$T/stillcmp" "$OUT/$2.png" "$OUT/$2-later.png")
  if [ $? = 0 ]; then pass "$1: $2 is still (no video behind it; ${d#stillcmp: })"
  else fail "$1: $2 changed within 3 s ($2.png, $2-later.png; ${d#stillcmp: })"; return; fi
  rm -f "$OUT/$2-later.png"
}

pages() { # pages <scenario>: a picture of each step's page
  for s in xcode steam account headset install; do cmd go "$s"; sleep 2; winshot "$1-$s"; done
}

# ---------------------------------------------------------------------------------------------
section "This Mac"
log "macOS $(sw_vers -productVersion), xcode-select: $(xcode-select -p)"
for x in /Applications/Xcode*.app; do
  [ -d "$x" ] || continue
  log "  $x -> $(realpath "$x") $(defaults read "$x/Contents/Info" CFBundleShortVersionString 2>/dev/null)"
done
log "visionOS SDK: $(xcrun --sdk xros --show-sdk-version 2>&1 | head -1)"
log "network: $(route -n get default 2>/dev/null | grep -E 'gateway|interface' | tr -s ' \n' ' '); proxies: $(scutil --proxy | grep -E 'Enable|Proxy |PAC' | tr -s ' \n' ' ')"
note "macOS $(sw_vers -productVersion); xcode-select $(xcode-select -p); visionOS SDK $(xcrun --sdk xros --show-sdk-version 2>&1 | head -1)"
ORIG_SELECT=$(xcode-select -p)
DEV=$ORIG_SELECT
# Background items before anything runs (K compares).
BTM_START=$(date '+%Y-%m-%d %H:%M:%S')
sudo sfltool dumpbtm > "$OUT/btm-start.txt" 2>&1
log "background items at the start: $(grep -c 'UUID:' "$OUT/btm-start.txt") ($(grep -ciE 'glovebox-vr|Glovebox\.app' "$OUT/btm-start.txt") of Glovebox)"
watch_prompts &
WATCHER=$!

# ---------------------------------------------------------------------------------------------
section "A. A fresh Mac: never signed in to Xcode, no headset"
xprefs none
defaults delete io.github.glovebox-vr.installer installed >/dev/null 2>&1
launch A || exit 1
sleep 6
still A A-welcome
cmd start
settle A
if [ "$(check visionos)" != ok ]; then
  note "A: visionOS isn't in Xcode yet ($(check visionos)): waiting for Glovebox to add it"
  waitfor "visionos:ok" 1500 && pass "A: Glovebox added visionOS by itself" || fail "A: visionOS didn't get added ($(status))"
  settle A
fi
expect A "the first page after Let's Go" "$(field page)" "steam"
expect A "Xcode step" "$(stepstate xcode)" "done"
expect A "team check" "$(check team)" "fail\(apple-account-missing\)"
expect A "Apple Account step" "$(stepstate account)" "attention"
expect A "headset check" "$(check headset)" "fail\(headset-not-paired\)"
expect A "Vision Pro step" "$(stepstate headset)" "attention"
expect A "Install step" "$(stepstate install)" "waiting"
note "A: Glovebox picked Xcode $(field xcode) at $(field dev | sed 's/%20/ /g')"
doctor A
expect A "engine's team check" "$(engine_team A)" "missing"
if [ "$DOCTOR_SECS" -le 45 ]; then pass "A: the engine's doctor with no headset took ${DOCTOR_SECS}s"; else fail "A: the engine's doctor with no headset took ${DOCTOR_SECS}s"; fi
pages A

# The sign-in guide on the real Xcode, start to Apple's sheet. Nobody signs in, so it has to stay.
cmd go account; sleep 1
cmd guide signin
for _ in $(seq 1 90); do "$T/windows" Xcode | grep -q 'layer=[08] ' && break; sleep 1; done
sleep 4
for i in 1 2 3 4 5 6 7 8; do
  [ "$(field stage)" = welcome ] || break
  [ "$i" = 1 ] && { shot A-signin-welcome; has A "the bubble over Xcode's first launch" "$(texts)" "Click Continue in Xcode"; }
  out=$(ax press Xcode Continue); log "  first launch $i: $out"
  echo "$out" | grep -q "^pressed" || ax pressat Xcode AXButton -1 >/dev/null
  sleep 4
done
waitfor "stage=open" 20
sleep 2
shot A-signin-open
t=$(texts); log "  bubble: $t"
expect A "sign-in guide's first stage" "$(field stage)" "open"
has A "sign-in bubble" "$t" "Press these keys"
osascript -e 'tell application id "com.apple.dt.Xcode" to activate' >/dev/null 2>&1; sleep 1
key "," "command down"
if waitfor "stage=click ring=[0-9]" 20; then
  sleep 1; shot A-signin-click
  t=$(texts); log "  bubble: $t"
  has A "sign-in bubble at Settings" "$t" "Click Sign In"
  clickring
  waitfor "stage=finish" 8 || { log "  the click didn't open the sign-in: pressing it"; ax press Xcode "Sign In…" >/dev/null; }
else
  fail "A: the sign-in guide didn't find Xcode's Settings ($(status))"
  ax press Xcode "Sign In…" >/dev/null
fi
if waitfor "stage=finish" 15; then
  sleep 3; shot A-signin-finish
  t=$(texts); log "  bubble: $t"
  has A "sign-in bubble at Apple's sheet" "$t" "Sign in with your Apple Account"
  sleep 20
  expect A "sign-in guide 20 s later, still not signed in" "$(field stage)" "finish"
else
  fail "A: the sign-in guide never reached Apple's sheet ($(status))"
fi
escape; cmd guide stop
quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"

# The pairing guide on the real Device Hub: +, Pair Nearby Device, Vision Pro, then the headset.
cmd go headset; sleep 1
cmd front; sleep 1
cmd guide pair
if waitfor "ring=[0-9]" 60; then
  sleep 2; shot A-pair-1-plus
  t=$(texts); log "  bubble: $t"
  expect A "pairing guide's first stage" "$(field stage)" "open"
  has A "first pairing bubble" "$t" "Click +"
  hasnt A "first pairing bubble" "$t" "On your Vision Pro"
  hasnt A "first pairing bubble" "$t" "First, on your Vision Pro"
  plus=$(field ring)
  clickring; sleep 2
  if waitfor "stage=click ring=[0-9]" 8 && [ "$(field ring)" != "$plus" ]; then
    sleep 1; shot A-pair-2-nearby
    t=$(texts); log "  bubble: $t"
    has A "second pairing bubble" "$t" "Click Pair Nearby Device"
    hasnt A "second pairing bubble" "$t" "On your Vision Pro"
    clickring
    if waitfor "stage=device ring=[0-9]" 10; then
      sleep 2; shot A-pair-3-visionpro
      log "  Device Hub's pairing window: $(ax texts com.apple.dt.Devices | tr '\n' ' ')"
      "$T/windows" "Device Hub" >> "$LOG"
      t=$(texts); log "  bubble: $t"
      has A "third pairing bubble (Device Hub's tiles are device types)" "$t" "Click Vision Pro"
      hasnt A "third pairing bubble" "$t" "Remote Devices"
      clickring
      if waitfor "stage=finish" 8; then
        sleep 2; shot A-pair-4-headset
        log "  Device Hub's pairing window after Vision Pro: $(ax texts com.apple.dt.Devices | tr '\n' ' ')"
        "$T/windows" "Device Hub" >> "$LOG"
        t=$(texts); log "  bubble: $t"
        has A "last pairing bubble" "$t" "Find your Vision Pro"
        has A "last pairing bubble" "$t" "open Settings › General › Remote Devices"
        has A "last pairing bubble" "$t" "On this Mac, pick it and click Next"
        hasnt A "last pairing bubble" "$t" "this Mac shows"
        hasnt A "last pairing bubble" "$t" "Now, on your Vision Pro"
        sleep 15
        expect A "pairing guide 15 s later, nothing paired" "$(field stage)" "finish"
      else
        fail "A: clicking Vision Pro didn't move the guide on ($(status))"; shot A-pair-stuck
      fi
    else
      fail "A: clicking Pair Nearby Device didn't move the guide on ($(status))"; shot A-pair-stuck
    fi
  else
    fail "A: clicking + didn't move the guide on ($(status))"; shot A-pair-stuck
  fi
else
  fail "A: the pairing guide didn't find Device Hub's + ($(status))"; shot A-pair-stuck
fi
"$T/windows" "Device Hub" >> "$LOG"
escape; cmd guide stop
hubquit
# Device Hub's code window comes up only with a real headset: the guide on a stand-in of its size
# (a real pairing's screenshot: 470 × 313, the six boxes 163 pt down). The headset shows the code, the player
# types it on this Mac.
cmd guide paircode
if waitfor "stage=finish ring=[0-9]" 10; then
  sleep 2; shot A-pair-5-code
  t=$(texts); log "  bubble: $t"
  has A "the code bubble" "$t" "Type the code your Vision Pro shows"
  has A "the code bubble" "$t" "Type it here, on this Mac"
  hasnt A "the code bubble" "$t" "this Mac shows"
  hasnt A "the code bubble" "$t" "Type it on your Vision Pro"
  scr=$(osascript -l JavaScript -e 'ObjC.import("AppKit"); const f = $.NSScreen.screens.objectAtIndex(0).frame; `${f.size.width},${f.size.height}`' 2>/dev/null)
  at=$(field ring)
  if [[ "$scr" =~ ^([0-9.]+),([0-9.]+)$ ]] && [[ "$at" =~ ^[0-9]+,[0-9]+$ ]]; then
    want="$(( ${scr%%,*} / 2 )),$(( ${scr##*,} / 2 + 30 ))"
    dx=$(( ${at%,*} - ${want%,*} )); dy=$(( ${at#*,} - ${want#*,} ))
    if [ ${dx#-} -le 30 ] && [ ${dy#-} -le 30 ]; then pass "A: the ring is on the code boxes ($at, boxes at $want)"
    else fail "A: the ring is at $at, the code boxes at $want"; fi
  else
    note "A: couldn't place the code boxes (screen $scr, ring $at)"
  fi
else
  fail "A: the guide didn't take the code window ($(status))"; shot A-pair-code-stuck
fi
cmd guide stop

# ---------------------------------------------------------------------------------------------
section "B. Signed out of Xcode, its paid team still cached (seen in a beta rehearsal)"
xprefs signed-out
launch B || exit 1
cmd start
settle B
expect B "team check" "$(check team)" "fail\(apple-account-missing\)"
expect B "Apple Account step" "$(stepstate account)" "attention"
expect B "Xcode step" "$(stepstate xcode)" "done"
doctor B
expect B "engine's team check" "$(engine_team B)" "missing"
cmd go account; sleep 2; winshot B-account
# The sign-in guide must stay up while signed out (it closed at once while the cached team counted).
cmd guide signin
sleep 25
st=$(field stage)
if [ "$st" != off ] && [ "$st" != done ]; then pass "B: the sign-in guide stays up while signed out (stage $st)"; else fail "B: the sign-in guide closed by itself while signed out"; fi
shot B-signin-guide
# Then signing in (the account list fills): the guide sees it, closes, and the step is done.
xprefs signed-in-paid
if waitfor "stage=off" 60; then pass "B: signing in closes the guide by itself"; else fail "B: the guide didn't notice the sign-in ($(status))"; fi
settle B 60
expect B "Apple Account step after signing in" "$(stepstate account)" "done"
expect B "team check after signing in" "$(check team)" "ok"
expect B "page after signing in" "$(field page)" "headset|steam"
winshot B-after-signin
quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"

# ---------------------------------------------------------------------------------------------
section "C. Teams cached with no account list at all"
xprefs cached-no-list
launch C || exit 1
cmd start
settle C
note "C: team check $(check team), Apple Account step $(stepstate account) (Glovebox counts a cached team as signed in when Xcode keeps no account list)"
doctor C

# ---------------------------------------------------------------------------------------------
section "C2. Account entries with no name in them (Xcode 16 can leave those), paid team cached"
xprefs nameless
launch C2 || exit 1
cmd start
settle C2
expect C2 "team check" "$(check team)" "fail\(apple-account-missing\)"
expect C2 "Apple Account step" "$(stepstate account)" "attention"
doctor C2
expect C2 "engine's team check" "$(engine_team C2)" "missing"
cmd go account; sleep 2
cmd guide signin
sleep 20
st=$(field stage)
if [ "$st" != off ] && [ "$st" != done ]; then pass "C2: the sign-in guide stays up (stage $st)"; else fail "C2: the sign-in guide closed by itself"; fi
escape; cmd guide stop
quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"

# ---------------------------------------------------------------------------------------------
section "T. Signed in to Xcode, its team not loaded yet (a tester, 2026-10-10): load the team, not sign in"
xprefs signed-in-noteams
launch T || exit 1
cmd start
settle T
expect T "team check" "$(check team)" "fail\(apple-team-not-loaded\)"
expect T "Apple Account step" "$(stepstate account)" "attention"
doctor T
expect T "engine's team check" "$(engine_team T)" "missing"
cmd go account; sleep 2; winshot T-account
has T "the Apple Account card" "$(texts)" "Load your team in Xcode"
cmd guide refresh
sleep 8
if [ "$(field stage)" = open ]; then
  shot T-guide-open
  osascript -e 'tell application id "com.apple.dt.Xcode" to activate' >/dev/null 2>&1; sleep 1
  key "," "command down"
fi
if waitfor "stage=click ring=[0-9]" 20; then
  sleep 1; shot T-guide-click
  t=$(texts); log "  bubble: $t"
  has T "the bubble at Settings" "$t" "Click your Apple Account"
  clickring
  # This Xcode isn't really signed in: the click on the account opens Apple's sign-in over Settings, where a real
  # account opens its page. What Xcode's windows do meanwhile (on run 38098348919 the guide lost Settings here).
  for i in 1 2 3 4 5 6 7 8; do
    log "  +${i}: $(field stage) ring=$(field ring) | $("$T/windows" Xcode | sed -E 's/^window owner=Xcode //' | tr '\n' ';')"
    sleep 0.5
  done
  # Xcode 27: the account's page lists its teams; the team's page has Download Manual Profiles (rings on both).
  if waitfor "stage=team ring=[0-9]" 8; then
    sleep 1; shot T-guide-team
    t=$(texts); log "  bubble: $t"
    has T "the bubble on the account's page" "$t" "Click your team"
    clickring
    if waitfor "stage=finish ring=[0-9]" 8; then
      sleep 1; shot T-guide-button
      t=$(texts); log "  bubble: $t"
      has T "the bubble on the team's page" "$t" "Click Download Manual Profiles"
    else
      fail "T: a click on the team didn't ring Download Manual Profiles ($(status))"
    fi
  else
    fail "T: a click on the account didn't ring its team ($(status))"
  fi
else
  fail "T: the guide didn't ring the account in Xcode's Settings ($(status))"
fi
st=$(field stage)
if [ "$st" != off ] && [ "$st" != done ]; then pass "T: the guide stays up while the team isn't loaded (stage $st)"; else fail "T: the guide closed by itself before the team loaded"; fi
# Xcode saving the team (what Download Manual Profiles does): the guide sees it, closes, and the step is done.
xprefs signed-in-paid
if waitfor "stage=off" 60; then pass "T: the team loading closes the guide by itself"; else fail "T: the guide didn't notice the team ($(status))"; fi
settle T 60
expect T "Apple Account step once the team loads" "$(stepstate account)" "done"
expect T "team check once the team loads" "$(check team)" "ok"
quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"

# ---------------------------------------------------------------------------------------------
section "D. Signed in with a paid team, then signing out while Glovebox is open"
xprefs signed-in-paid
launch D || exit 1
cmd start
settle D
expect D "team check" "$(check team)" "ok"
expect D "Apple Account step" "$(stepstate account)" "done"
doctor D
expect D "engine's team check" "$(engine_team D)" "ok"
pages D
xprefs signed-out
sleep 5
for _ in $(seq 1 30); do [ "$(check team)" = "fail(apple-account-missing)" ] && break; sleep 1; done
expect D "team check after signing out" "$(check team)" "fail\(apple-account-missing\)"
expect D "Apple Account step after signing out" "$(stepstate account)" "attention"
cmd go account; sleep 2; winshot D-signed-out

# ---------------------------------------------------------------------------------------------
section "E. Signed in with a free account only: it builds, for 7 days at a time"
xprefs signed-in-free
launch E || exit 1
cmd start
settle E
expect E "team check" "$(check team)" "ok"
expect E "Apple Account step" "$(stepstate account)" "done"
doctor E
cmd go account; sleep 2; winshot E-account
has E "the Apple Account card" "$(texts)" "reinstall every 7 days"

# ---------------------------------------------------------------------------------------------
section "Dark. The guide's bubble over dark Xcode and Device Hub windows"
dark() { osascript -e "tell application \"System Events\" to tell appearance preferences to set dark mode to $1" >/dev/null 2>&1; }
dark true
xprefs none
launch Dark || exit 1
cmd start
settle Dark
cmd go account; sleep 1
cmd guide signin
waitfor "stage=open" 60 && sleep 4
shot Dark-signin-open
osascript -e 'tell application id "com.apple.dt.Xcode" to activate' >/dev/null 2>&1; sleep 1
key "," "command down"
waitfor "stage=click ring=[0-9]" 20 && { sleep 2; shot Dark-signin-click; }
escape; cmd guide stop
quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"
cmd go headset; sleep 1; cmd front; sleep 1
cmd guide pair
if waitfor "ring=[0-9]" 60; then
  sleep 2; shot Dark-pair-1-plus
  clickring; sleep 2
  waitfor "stage=click ring=[0-9]" 8 && { sleep 1; shot Dark-pair-2-nearby; clickring; }
  waitfor "stage=device ring=[0-9]" 10 && { sleep 2; shot Dark-pair-3-visionpro; }
  pass "Dark: bubbles over dark windows (Dark-*.png)"
else
  fail "Dark: the pairing guide didn't find Device Hub's + ($(status))"
fi
escape; cmd guide stop
hubquit
winshot Dark-headset
dark false

# ---------------------------------------------------------------------------------------------
section "Menus: Licenses, Help › Glovebox Website, Help › Copy Diagnostics"
menu() { osascript -e "tell application \"System Events\" to tell process \"Glovebox\" to click menu item \"$2\" of menu 1 of menu bar item \"$1\" of menu bar 1" 2>&1; }
cmd front; sleep 1
o=$(menu Glovebox "Licenses…"); log "  Licenses…: $o"
sleep 2
if "$T/windows" Glovebox | grep -q "name=Licenses"; then pass "Licenses opens its window"; else
  w=$(ax windows Glovebox); log "  Glovebox windows: $w"
  echo "$w" | grep -q Licenses && pass "Licenses opens its window" || fail "Licenses didn't open a window"
fi
shot menu-licenses
ax texts Glovebox | head -c 600 >> "$LOG"; echo >> "$LOG"
osascript -e 'tell application "System Events" to tell process "Glovebox" to keystroke "w" using {command down}' >/dev/null 2>&1
o=$(menu Help "Glovebox Website"); log "  Glovebox Website: $o"
# The browser's window title is the page's (asking Safari for its URL is an Apple event, which
# makes macOS ask this runner for permission to control Safari).
for _ in $(seq 1 20); do "$T/windows" Safari | grep -qi "name=[^-].*glovebox" && break; sleep 1; done
page=$("$T/windows" Safari | grep -i "glovebox" | head -1)
log "  Safari: ${page:-$("$T/windows" Safari | head -2)}"
if [ -n "$page" ]; then pass "Help › Glovebox Website opens the site in Safari (${page#*name=})"; else fail "Help › Glovebox Website: no Glovebox page in Safari"; fi
shot menu-website
quitapp com.apple.Safari "/Applications/Safari.app/Contents/MacOS/Safari"
cmd front; sleep 1
printf '' | pbcopy
o=$(menu Help "Copy Diagnostics"); log "  Copy Diagnostics: $o"
sleep 3
pbpaste > "$OUT/diagnostics.txt"
log "  diagnostics: $(wc -c < "$OUT/diagnostics.txt") bytes"
if [ -s "$OUT/diagnostics.txt" ]; then pass "Copy Diagnostics copies $(wc -l < "$OUT/diagnostics.txt" | tr -d ' ') lines (diagnostics.txt)"; else fail "Copy Diagnostics copied nothing"; fi
if grep -qiE 'refresh_?token|password|jane@example' "$OUT/diagnostics.txt"; then fail "Copy Diagnostics has a token, password or the account's email"; else pass "Copy Diagnostics has no token, password or email"; fi

# ---------------------------------------------------------------------------------------------
section "X. Xcode's license not accepted yet (a new Xcode): the setup guide, then Glovebox comes back by itself"
xprefs signed-in-paid
LIC=/Library/Preferences/com.apple.dt.Xcode
sudo cp "$LIC.plist" "$T/license-backup.plist" 2>/dev/null
for k in IDEXcodeVersionForAgreedToGMLicense IDELastGMLicenseAgreedTo IDEXcodeVersionForAgreedToBetaLicense IDELastBetaLicenseAgreedTo; do
  sudo defaults delete "$LIC" "$k" >/dev/null 2>&1
done
"$ORIG_SELECT/usr/bin/xcodebuild" -license check >/dev/null 2>&1; lc=$?
"$ORIG_SELECT/usr/bin/xcodebuild" -checkFirstLaunchStatus >/dev/null 2>&1; fc=$?
log "  license check exit $lc; first-launch status $fc"
if [ $lc = 0 ]; then
  note "X: couldn't unaccept Xcode's license on this runner, so the setup guide wasn't shown"
else
  launch X || exit 1
  cmd start
  settle X
  expect X "xcode check" "$(check xcode)" "fail\(xcode-first-launch\)"
  expect X "Xcode step" "$(stepstate xcode)" "attention"
  cmd go xcode; sleep 2; winshot X-open-xcode
  has X "the Xcode card" "$(texts)" "Finish setting up Xcode"
  echo "X (Xcode's own setup, after Open Xcode)" > "$T/phase"
  cmd fix xcode
  waitfor "stage=click" 60
  sleep 5; shot X-guide-agree
  t=$(texts); log "  bubble: $t"
  "$T/windows" Xcode >> "$LOG"
  ax dump Xcode | grep -E "AXButton" | head -12 >> "$LOG"
  expect X "the setup guide's first stage" "$(field stage)" "click"
  has X "the setup bubble" "$t" "Click Agree"
  expect X "Xcode step while the guide runs" "$(stepstate xcode)" "working"
  # Agree, as the player would: then macOS asks for the Mac's password, for Xcode.
  if [ "$(field ring)" != none ]; then clickring; else log "  Agree: $(ax press Xcode Agree)"; fi
  sleep 5
  shot X-after-agree
  "$T/windows" SecurityAgent >> "$LOG"
  if "$T/windows" SecurityAgent | grep -q .; then
    waitfor "stage=confirm" 10
    t=$(texts); log "  bubble: $t"
    has X "the bubble at macOS's password prompt" "$t" "Type your Mac's password"
    shot X-guide-password
    # On a Mac with Touch ID the same prompt takes a finger, with Use Password beside it.
    cmd touchid on; sleep 2
    t=$(texts); log "  bubble as on a Mac with Touch ID: $t"
    has X "the bubble at the prompt on a Mac with Touch ID" "$t" "Use Touch ID"
    has X "the bubble at the prompt on a Mac with Touch ID" "$t" "click Use Password"
    shot X-guide-touchid
    cmd touchid -; sleep 1
    # The runner has no password to type: cancel (the license is still there, so the bubble goes
    # back to Agree), then accept the license the way the prompt would.
    escape
    if waitfor "stage=click" 20; then t=$(texts); has X "the bubble after the password prompt is cancelled" "$t" "Click Agree"
    else fail "X: the bubble didn't go back to Agree after the password prompt was cancelled ($(status))"; fi
    shot X-guide-cancelled
  else
    note "X: no password prompt after Agree (guide stage $(field stage))"
  fi
  sudo "$ORIG_SELECT/usr/bin/xcodebuild" -license accept
  # The password, typed, would take Xcode on from its license; here Xcode opens again with the
  # license accepted.
  quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"
  open -a "${ORIG_SELECT%/Contents/Developer}"
  for _ in $(seq 1 30); do "$T/windows" Xcode | grep -q . && break; sleep 1; done
  sleep 3
  shot X-xcode-again
  # Xcode's first pages, if this runner shows them: Continue until they're through.
  for i in 1 2 3 4 5 6; do
    [ "$(field stage)" = welcome ] || { sleep 3; [ "$(field stage)" = welcome ] || break; }
    if [ "$i" = 1 ]; then shot X-guide-continue; t=$(texts); log "  bubble: $t"; has X "the bubble on Xcode's first pages" "$t" "Click"; fi
    if [ "$(field ring)" != none ]; then clickring; else ax press Xcode Continue >/dev/null; fi
    sleep 4
  done
  if waitfor "stage=off" 90; then pass "X: the setup guide closes once Xcode is set up"; else fail "X: the setup guide didn't close ($(status))"; fi
  sleep 4
  log "  windows after Xcode's setup: $("$T/windows" Xcode | tr '\n' ';' | head -c 400)"
  expect X "the app in front after Xcode's setup" "$(field front)" "$GB"
  expect X "xcode check once Xcode is set up" "$(check xcode)" "ok"
  expect X "Xcode step once Xcode is set up" "$(stepstate xcode)" "done"
  winshot X-after
  quitapp com.apple.dt.Xcode "/Contents/MacOS/Xcode"
fi
sudo "$ORIG_SELECT/usr/bin/xcodebuild" -license accept >/dev/null 2>&1
echo "X done" > "$T/phase"

# ---------------------------------------------------------------------------------------------
section "F. Which Xcode Glovebox picks"
xprefs none
newest_release() { # the newest Xcode 26+ that isn't a beta, by version
  for x in /Applications/Xcode*.app; do
    [ -L "$x" ] && continue
    v=$(defaults read "$x/Contents/Info" CFBundleShortVersionString 2>/dev/null) || continue
    case "$(basename "$x" | tr A-Z a-z)" in *beta*) continue;; esac
    [ "${v%%.*}" -ge 26 ] 2>/dev/null && echo "$v $x"
  done | sort -V | tail -1 | cut -d' ' -f2-
}
NEWEST=$(newest_release)
log "newest release Xcode: $NEWEST"
BETAS=$(ls -d /Applications/Xcode*eta*.app 2>/dev/null | tr '\n' ' ')
note "F: Xcodes here: $(for x in /Applications/Xcode*.app; do [ -L "$x" ] || printf '%s (%s) ' "$(basename "$x")" "$(defaults read "$x/Contents/Info" CFBundleShortVersionString 2>/dev/null)"; done)"
[ -z "$BETAS" ] && note "F: no beta Xcode on this runner, so 'a beta beside the release is never picked' is covered by the unit tests only"
pick() { # pick <label> <xcode-select path> <expected Xcode.app, or empty to just note it>
  sudo xcode-select -s "$2" || { note "F: couldn't select $2"; return; }
  launch "F $1" || return
  cmd start
  settle "F $1"
  local got; got=$(field dev | sed 's/%20/ /g; s#/Contents/Developer$##')
  if [ -n "$3" ]; then
    if [ "$(realpath "$got" 2>/dev/null)" = "$(realpath "$3")" ]; then pass "F: with xcode-select at $2, Glovebox uses $got"
    else fail "F: with xcode-select at $2, Glovebox uses ${got:-nothing} (should be $3)"; fi
  else
    note "F: with xcode-select at $2, Glovebox uses ${got:-nothing} (Xcode $(field xcode)), Xcode step $(stepstate xcode), xcode check $(check xcode)"
  fi
  winshot "F-$1"
}
SELECTED_APP=${ORIG_SELECT%/Contents/Developer}
pick default "$ORIG_SELECT" "$SELECTED_APP"
for x in /Applications/Xcode*.app; do
  [ -L "$x" ] && continue
  [ "$(realpath "$x")" = "$(realpath "$SELECTED_APP")" ] && continue
  v=$(defaults read "$x/Contents/Info" CFBundleShortVersionString 2>/dev/null) || continue
  if [ "${v%%.*}" -ge 26 ] 2>/dev/null; then pick "xcode-$v" "$x/Contents/Developer" "$x"; break; fi
done
[ -d /Library/Developer/CommandLineTools ] && pick clt /Library/Developer/CommandLineTools "$NEWEST"
sudo xcode-select -s "$ORIG_SELECT"

# ---------------------------------------------------------------------------------------------
section "G. Keychain (Debug build, unsigned: the login keychain; the data protection keychain needs the Developer ID build)"
quitall
K="$BUNDLE/Contents/MacOS/Glovebox"
kc() { local o; o=$(run_for 30 "$K" -keychain-selftest "$1"); log "  $1: $o"; echo "$o"; }
kc clean >/dev/null
o=$(kc write); has G "write" "$o" ": wrote"
o=$(kc read); has G "read after write" "$o" "read: written by build"
kc clean >/dev/null
o=$(kc read); has G "read after clean" "$o" "read: nothing"
o=$(kc legacy-write); has G "login-keychain write" "$o" "wrote to the login keychain"
o=$(kc read); has G "read of the login-keychain item" "$o" "read: written to the login keychain"
kc clean >/dev/null
if security find-generic-password -s "Glovebox Steam session" >/dev/null 2>&1; then fail "G: a Debug build made a \"Glovebox Steam session\" item"
else pass "G: no \"Glovebox Steam session\" item (the release one) after every run"; fi
log "  dev items: $(security find-generic-password -s "Glovebox Steam session (dev)" 2>&1 | grep -c acct) (expected 0: nobody signed in)"

# ---------------------------------------------------------------------------------------------
section "L. Never \"Allow Glovebox to find devices on local networks?\""
# Glovebox starts devicectl and the engine (which starts devicectl too) as its plain children: the
# helper that made them their own responsible process ran in the background, and macOS 27 said so
# (K). The local network alert came from Glovebox looking up the Mac's Bonjour name (run 10).
# lanprobe uses the local network (a connection to the gateway, a Bonjour browse) and says whether
# macOS let it: started the way Glovebox starts tools, then under Python the way the engine starts
# devicectl, then devicectl itself, with and without waiting.
xprefs signed-in-paid
lprompts() { "$T/windows" UserNotificationCenter 2>/dev/null | grep -c . ; }
xcrun swiftc -O -o "$T/lanprobe" "$HERE/lanprobe.swift" 2>&1 | grep -v warning | head -5
printf 'import subprocess, sys\nsys.exit(subprocess.run(sys.argv[1:]).returncode)\n' > "$T/child.py"
if launch L; then
  cmd start
  settle L 60
  for v in "run|$T/lanprobe|lanprobe, started the way Glovebox starts tools|must" \
           "run|/usr/bin/xcrun python3 $T/child.py $T/lanprobe|lanprobe under Python, the way the engine starts devicectl|must" \
           "devicectl|list devices --json-output $T/devices.json|devicectl list, the way Glovebox starts it|must" \
           "devicectl|list devices --timeout 20 --json-output $T/devices.json|devicectl list waiting 20 s (run 3's call)|must"; do
    IFS='|' read -r how args what kind <<< "$v"
    out="$T/run.out"; case $how in devicectl*) out="$T/devicectl.out" ;; esac
    rm -f "$out"
    before=$(lprompts)
    cmd "$how" "$args"
    sleep 3
    p=$(pgrep -nf "lanprobe|devicectl list" | head -1)
    [ -n "$p" ] && log "  $what: pid $p, $(sudo launchctl procinfo "$p" 2>/dev/null | grep -iE 'responsible' | tr -s ' \n' ' ' | head -c 300)"
    for _ in $(seq 1 40); do [ -f "$out" ] && break; sleep 1; done
    sleep 3
    after=$(lprompts)
    result=$(sed -n 2p "$out" 2>/dev/null | head -c 300)
    log "  $what: $(head -1 "$out" 2>/dev/null) $result, prompts $before -> $after"
    shot "L-${how}-$(echo "$what" | tr -cs 'A-Za-z0-9' '-' | head -c 40)"
    asked=$([ "$after" -gt "$before" ] && echo yes || echo no)
    denied=$(echo "$result" | grep -q "denied" && echo yes || echo no)
    case $kind in
      must)
        if [ "$asked" = yes ]; then fail "L: $what makes macOS ask to find devices on local networks (L-*.png)"
        elif [ "$denied" = yes ]; then fail "L: $what is kept off the local network without a prompt ($result)"
        else pass "L: $what asks nothing${result:+ ($result)}"; fi ;;
      *) note "L: $what: prompt $asked${result:+, $result}" ;;
    esac
    hubquit
  done
  # Answer it (Don't Allow, the first button), so it isn't in the way of what comes next.
  [ "$(lprompts)" -gt 0 ] && log "  local network prompt: $(ax pressat UserNotificationCenter AXButton 0)"
fi
quitall

# ---------------------------------------------------------------------------------------------
section "H. Download, build and install with the fake engine (no Steam account, no headset)"
fake() { # fake <name> <env...>: the fake-engine app, with the fake engine's switches
  local name=$1; shift
  quitall
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  rm -f "$T/next" "$T/status"
  env FAKE_STEP=0.05 "$@" "$FAKE/Contents/MacOS/Glovebox" -tour "$T" > "$T/fake-$name.log" 2>&1 &
  for _ in $(seq 1 60); do [ -f "$T/status" ] && break; sleep 0.5; done
  [ -f "$T/status" ] || { fail "$name: the app didn't open"; return 1; }
  sleep 2
}
# Quit as a player does (⌘Q), then: nothing of Glovebox's still running, and 40 s later no background
# item (build 2026100614's helper outlived the app, and macOS 27 registered Glovebox 33 s after a
# tester's quit, with a notification).
leftovers() { ps -axo pid,ppid,command | grep -E "glovebox (doctor|build|install)|glovebox\.py|devicectl|Glovebox\.app/Contents/MacOS/Glovebox|xcodebuild" | grep -v grep | cut -c1-160; }
afterquit() { # afterquit <scenario> <when>
  noteQuit "quit $2"
  osascript -e 'with timeout of 5 seconds' -e "tell application id \"$GB\" to quit" -e 'end timeout' >/dev/null 2>&1
  for _ in $(seq 1 16); do pgrep -f "Glovebox.app/Contents/MacOS/Glovebox" >/dev/null || break; sleep 0.5; done
  if pgrep -f "Glovebox.app/Contents/MacOS/Glovebox" >/dev/null; then fail "$1: Glovebox didn't quit $2"; quitall; return; fi
  sleep 1
  local left; left=$(leftovers)
  if [ -n "$left" ]; then fail "$1: after quitting $2, still running: $(echo "$left" | tr '\n' ';')"
  else pass "$1: quitting $2 leaves nothing of Glovebox's running"; fi
  sleep 40
  if sudo sfltool dumpbtm 2>/dev/null | grep -qiE 'glovebox-vr\.installer'; then fail "$1: 40 s after quitting $2, Glovebox is a background item"
  else pass "$1: 40 s after quitting $2, no background item"; fi
}
if [ -n "$FAKE" ] && [ -x "$FAKE/Contents/MacOS/Glovebox" ]; then
  xprefs signed-in-paid
  # A first install: the stutter-report question not answered yet.
  defaults delete $GB shareStutters >/dev/null 2>&1; defaults delete $GB shareStuttersAt >/dev/null 2>&1
  fake H FAKE_FAIL="team:8,headset:16" FAKE_TEAM=none FAKE_HEADSET=devmode
  cmd start
  waitfor "checked=1" 60
  sleep 1
  cmd go account; sleep 1.5; winshot H-account
  # Shown once it has said so for 10 s (a headset waking up says it for a few seconds).
  for _ in $(seq 1 60); do [ "$(check developer-mode)" = "fail(developer-mode-off)" ] && break; sleep 0.5; done
  cmd go headset; sleep 1.5; winshot H-headset-devmode
  log "  status: $(status)"
  expect H "Developer Mode while it's off" "$(check developer-mode)" "fail\(developer-mode-off\)"
  cmd go install; sleep 1.5; winshot H-install-waiting
  expect H "Install step while steps are open" "$(stepstate install)" "waiting"
  for _ in $(seq 1 90); do [ "$(stepstate headset)" = done ] && [ "$(stepstate account)" = done ] && break; sleep 1; done
  expect H "Apple Account step once the fake team is in" "$(stepstate account)" "done"
  expect H "Vision Pro step once Developer Mode is on" "$(stepstate headset)" "done"
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  expect H "Steam step" "$(stepstate steam)" "done"
  expect H "download" "$(field download)" "100"
  cmd go install; sleep 1.5; winshot H-install-ready
  expect H "Install step" "$(stepstate install)" "todo"
  # The game files are here already, so the build would start at once: it waits for the answer
  # (a beta rehearsal: the answer given on the Mac, and the headset asked again), and carries it.
  touch "$T/h-install"
  cmd install
  sleep 6; winshot H-share-ask
  expect H "the stutter-report question once Install is clicked, game files here" "$(field share)" "ask"
  early=$(find "$HOME/Library/Logs/Glovebox" -name 'build-*.log' -newer "$T/h-install" 2>/dev/null | head -1)
  if [ -z "$early" ]; then pass "H: the build waits for the answer"; else fail "H: the build started before the answer ($early)"; fi
  cmd answer share
  sleep 3; winshot H-installing
  expect H "the answer" "$(field share)" "yes"
  n=$(pgrep -f "$FAKE/Contents/MacOS/Glovebox" | wc -l | tr -d ' ')
  expect H "Glovebox processes while it builds (no helper of its own)" "$n" "1"
  waitfor "install=done" 120 || fail "H: the install didn't finish ($(status))"
  envf="$HOME/Library/Application Support/Glovebox/alyx/KleptonAlyx.app/alyx-demo/alyx-env.txt"
  log "  built app's alyx-env.txt: $(tr '\n' ' ' < "$envf" 2>/dev/null)"
  if grep -qx "GLOVEBOX_SHARE_STUTTERS=1" "$envf" 2>/dev/null; then pass "H: the built app carries the answer (alyx-demo/alyx-env.txt: GLOVEBOX_SHARE_STUTTERS=1)"
  else fail "H: the built app doesn't carry the answer ($envf: $(tr '\n' ' ' < "$envf" 2>/dev/null))"; fi
  sleep 4; still H H-done
  expect H "install" "$(field install)" "done"
  expect H "Install step at the end" "$(stepstate install)" "done"
  afterquit H "on the finished page"
  # Next time it opens on Home.
  quitall; rm -f "$T/next" "$T/status"
  "$FAKE/Contents/MacOS/Glovebox" -tour "$T" > "$T/fake-home.log" 2>&1 &
  for _ in $(seq 1 60); do [ -f "$T/status" ] && break; sleep 0.5; done
  sleep 8; still H H-home
  defaults delete io.github.glovebox-vr.installer installed >/dev/null 2>&1
  log "  fake engine log: $(tail -3 "$T/fake-H.log" | tr '\n' ' ')"

  # A returning player: Glovebox checks the saved Steam session at launch (it's made up, so Steam
  # turns it down). That check must not make macOS ask about the local network (the prompt watcher).
  fake Hs
  cmd start
  # The made-up token can take Steam a while to turn down (the logon retries up to four times).
  for _ in $(seq 1 20); do [ "$(stepstate steam)" = working ] && break; sleep 0.5; done
  t0=$(date +%s)
  for _ in $(seq 1 180); do [ "$(stepstate steam)" != working ] && break; sleep 1; done
  sleep 2; winshot H-saved-session
  log "  saved session at launch, after $(( $(date +%s) - t0 )) s: $(status)"
  expect H "the Steam step after checking a made-up saved session" "$(stepstate steam)" "todo|attention"
  cp "$T/fake-Hs.log" "$OUT/fake-Hs.txt" 2>/dev/null

  # The headset asleep: the build goes through, devicectl's install says the device is locked,
  # Glovebox waits for the headset, and installs once the doctor finds it awake again. Signed out of
  # the made-up session first, so Steam turning it down can't land after fakesteam made a new one.
  touch "$T/h2-start"
  fake H2 FAKE_FAIL="headset:999" FAKE_HEADSET=asleep FAKE_BUILD_FAIL=install
  cmd steamout
  cmd start
  settle H2 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  waitfor "install=headset" 120
  sleep 2; winshot H2-install-asleep
  expect H2 "install with the headset asleep" "$(field install)" "headset"
  # Awake: the fake engine's headset check passes from now on.
  find "$HOME/Library/Application Support/Glovebox" -path '*/.fake' -type d 2>/dev/null | while read -r d; do echo 999 > "$d/runs-headset"; done
  waitfor "install=done" 60
  expect H2 "install once the headset is awake" "$(field install)" "done"
  winshot H2-installed
  cp "$T/fake-H2.log" "$OUT/fake-H2.txt" 2>/dev/null
  find "$HOME/Library/Logs/Glovebox" -name '*.log' -newer "$T/h2-start" 2>/dev/null | while read -r f; do
    cp "$f" "$OUT/engine-H2-$(basename "$f" .log).txt"; done
  ls -laR "$HOME/Library/Application Support/Glovebox/alyx" 2>/dev/null | grep -v linuxsteamrtarm64 | head -60 > "$OUT/H2-work.txt"

  # Quitting mid-build: the engine and what it started stop with Glovebox, so nothing of it runs
  # on in the background.
  fake H3 FAKE_STEP=1
  cmd steamout
  cmd start
  settle H3 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  e=""
  for _ in $(seq 1 40); do e=$(pgrep -f "glovebox build" | head -1); [ -n "$e" ] && break; sleep 0.5; done
  if [ -n "$e" ]; then
    sleep 4
    pg=$(ps -o pgid= -p "$e" | tr -d ' ')
    log "  engine $e, group $pg: $(ps -axo pid,pgid,command | awk -v g="$pg" '$2 == g' | cut -c1-140 | tr '\n' ';')"
    winshot H3-building
    afterquit H3 "mid-build"
    left=$(ps -axo pid,pgid,command | awk -v g="$pg" '$2 == g' | cut -c1-140)
    [ -n "$left" ] && fail "H3: the engine's process group is still there: $left"
  else
    fail "H3: the build didn't start ($(status))"
  fi
  cp "$T/fake-H3.log" "$OUT/fake-H3.txt" 2>/dev/null

  # Quitting while the install waits for an asleep headset (the doctor runs every 3 s then).
  fake H4 FAKE_FAIL="headset:999" FAKE_HEADSET=asleep FAKE_BUILD_FAIL=install
  cmd steamout
  cmd start
  settle H4 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  if waitfor "install=headset" 120; then
    sleep 5; winshot H4-waiting
    afterquit H4 "while the install waits for the headset"
  else
    fail "H4: the install didn't get to waiting for the headset ($(status))"
  fi
  cp "$T/fake-H4.log" "$OUT/fake-H4.txt" 2>/dev/null

  # The news sign-up: it comes up beside the progress once nothing has asked anything for 20 s, stays on
  # the finished page while it has no answer, and No Thanks ends it for good. (No address is ever sent.)
  defaults delete $GB newsAnswered >/dev/null 2>&1; defaults delete $GB newsSignedUp >/dev/null 2>&1
  fake W FAKE_STEP=1.5
  cmd steamout
  cmd start
  settle W 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  if waitfor "news=ask" 90; then
    expect W "the install when the news card comes up" "$(field install)" "running"
    sleep 2; winshot W-news-installing
    has W "the install page" "$(texts)" "Hear from us when more games come to Vision Pro"
  else
    fail "W: no news card while it installs ($(status))"
  fi
  waitfor "install=done" 180 || fail "W: the install didn't finish ($(status))"
  sleep 3; winshot W-news-done
  expect W "the news card on the finished page, not answered yet" "$(field news)" "ask"
  r=$(ax press Glovebox "No Thanks")
  case "$r" in pressed*) log "  No Thanks: $r";; *) log "  No Thanks: $r; by the tour instead"; cmd news no;; esac
  sleep 2; winshot W-after-no-thanks
  expect W "the news card after No Thanks" "$(field news)" "-"
  expect W "the answer, kept" "$(defaults read $GB newsAnswered 2>/dev/null)" "1"
  cp "$T/fake-W.log" "$OUT/fake-W.txt" 2>/dev/null
  quitall
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  security delete-generic-password -s "Glovebox Steam session (dev)" -a current >/dev/null 2>&1
else
  note "H: no fake-engine build given"
fi

# ---------------------------------------------------------------------------------------------
section "S. Signing: a key codesign may use asks nothing; one macOS asks about asks once, alone, before the build"
# A key the App Store Xcode app made gets a partition list codesign isn't on, so macOS asks once, for the
# Mac's password and Always Allow; xcodebuild's build signs ~94 files at once and each would ask
# (research/signing-probe). The stand-in here: an "Apple Development" identity for the rehearsal's team in
# a keychain of its own (password known), codesign on its key's access list or not. Glovebox reads that
# without asking, signs one file alone when macOS will ask, and builds only once codesign may use the key.
if [ -n "$FAKE" ] && [ -x "$FAKE/Contents/MacOS/Glovebox" ]; then
  SKC="$T/gbsign.keychain-db"; SPW=gbsign-pass
  sigkc() { # sigkc <name> [security import flags]: a fresh keychain holding one identity for team A1B2C3D4E5
    local n=$1; shift
    security delete-keychain "$SKC" >/dev/null 2>&1
    security create-keychain -p "$SPW" "$SKC" && security unlock-keychain -p "$SPW" "$SKC" && security set-keychain-settings "$SKC"
    security list-keychains -d user -s $(security list-keychains -d user | tr -d '"' | grep -v gbsign) "$SKC"
    printf '[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=Apple Development: Jane Appleseed (S0S0S0S0S0)\nOU=A1B2C3D4E5\nO=Jane Appleseed\n[ext]\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=critical,codeSigning\nbasicConstraints=critical,CA:false\n' > "$T/$n.cnf"
    /usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout "$T/$n.key" -out "$T/$n.pem" -days 30 -config "$T/$n.cnf" 2>/dev/null
    /usr/bin/openssl pkcs12 -export -inkey "$T/$n.key" -in "$T/$n.pem" -out "$T/$n.p12" -passout pass:p12 2>/dev/null
    log "  $n identity: $(security import "$T/$n.p12" -k "$SKC" -P p12 "$@" 2>&1 | tr '\n' ' ')"
  }
  sa() { "$T/windows" SecurityAgent | head -1; }
  sa_wait() { for _ in $(seq 1 40); do [ -n "$(sa)" ] && return 0; sleep 0.5; done; return 1; }
  # A spot in macOS 27's keychain prompt (432x180): its buttons sit 31 pt above the bottom.
  sa_at() { # sa_at <x from the left, or -x from the right> <y from the top, or -y from the bottom>
    local f x y w h; f=$(sa | sed -E 's/.* x=([0-9.-]+) y=([0-9.-]+) w=([0-9.]+) h=([0-9.]+).*/\1 \2 \3 \4/')
    read -r x y w h <<< "$f"
    python3 -c "import sys; x,y,w,h,dx,dy=map(float,sys.argv[1:]); print(int(x+(dx if dx>=0 else w+dx)), int(y+(dy if dy>=0 else h+dy)))" "$x" "$y" "$w" "$h" "$1" "$2"
  }
  sa_type() { mouse $(sa_at 289 105) click >/dev/null; sleep 0.5; osascript -e "tell application \"System Events\" to keystroke \"$1\"" >/dev/null 2>&1; sleep 0.5; }
  sa_press() {
    local r; r=$(ax press SecurityAgent "$1")
    case "$r" in pressed*) log "  $1: $r"; return;; esac
    case "$1" in "Always Allow") mouse $(sa_at 146 -31) click >/dev/null;; Deny) mouse $(sa_at -150 -31) click >/dev/null;; Allow) mouse $(sa_at -59 -31) click >/dev/null;; esac
    log "  $1: clicked ($r)"
  }
  builds() { find "$HOME/Library/Logs/Glovebox" -name 'build-*.log' -newer "$1" 2>/dev/null | wc -l | tr -d ' '; }
  continue_card() { # Continue on Glovebox's signing card, once it's up: only then does macOS ask
    waitfor "install=signing" 40 || { fail "S1: no signing card ($(status))"; return 1; }
    local r; r=$(ax press Glovebox Continue)
    case "$r" in pressed*) log "  Continue: $r";; *) log "  Continue: $r; by the tour instead"; cmd continue;; esac
  }
  guide_check() { # the guide under macOS's window, pointing up at Always Allow
    waitfor "stage=click" 10 || { fail "S1: no guide by the keychain window ($(status))"; return 1; }
    sleep 0.5
    local r w; r=$(field ring); w=$(sa_at 146 -31); log "  guide points at $r, Always Allow at ${w/ /,}"
    [[ "$r" =~ ^[0-9]+,[0-9]+$ ]] || { fail "S1: the guide points at nothing ($(status))"; return 1; }
    local dx=$(( ${r%,*} - ${w% *} )) dy=$(( ${r#*,} - ${w#* } ))
    expect S1 "the guide pointing at Always Allow (off by dx=$dx dy=$dy)" "$(( dx * dx + dy * dy <= 64 ? 1 : 0 ))" "1"
    has S1 "the guide" "$(texts)" "Type your Mac password, then click Always Allow"
    # Its bubble covers none of macOS's window.
    local b; b=$("$T/windows" Glovebox | awk -v sy="$(sa | sed -E 's/.* y=([0-9.-]+) .* h=([0-9.]+).*/\1 \2/')" '
      BEGIN { split(sy, a, " "); bottom = a[1] + a[2] }
      { for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
        if (v["w"] < 400 && v["y"] + 0 >= bottom - 20 && v["y"] + 0 < bottom + 40) print "below" }' | head -1)
    expect S1 "the guide's bubble under macOS's window" "${b:-not found}" "below"
  }
  toinstall() { # toinstall <scenario>: the fake app on the Install step, game files here, the report question answered
    fake "$1"
    cmd steamout
    cmd start
    settle "$1" 60
    cmd fakesteam
    for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
    cmd go install; sleep 1
  }
  xprefs signed-in-paid
  defaults write $GB shareStutters -bool true

  # S0. codesign already on the key's list: no question, no card, the build goes ahead.
  sigkc S0 -T /usr/bin/codesign
  toinstall S0
  touch "$T/s0"
  cmd install
  seen=no; asked=no
  for _ in $(seq 1 240); do
    [ "$(field install)" = signing ] && seen=yes
    [ -n "$(sa)" ] && { asked=yes; shot S0-prompt; }
    [ "$(field install)" = done ] && break
    sleep 0.5
  done
  expect S0 "the signing card, codesign already allowed" "$seen" "no"
  expect S0 "a keychain prompt, codesign already allowed" "$asked" "no"
  expect S0 "install" "$(field install)" "done"
  expect S0 "whether the game has run on the headset (a paid team's apps need no trust: not looked)" "$(field ran)" "-"
  quitall

  # S1. Only the security tool on the key's list: macOS will ask. Its prompts here are expected.
  echo "S1. A key macOS asks about (its prompts are expected)" > "$T/phase"
  sigkc S1
  toinstall S1
  touch "$T/s1"
  cmd install
  waitfor "install=signing" 40 || fail "S1: no signing step before the build ($(status))"
  expect S1 "the install before the build" "$(field install)" "signing"
  # The card says why, and macOS asks only after Continue. Glovebox is behind Finder here, as for a player who
  # went to another app during the download: the card draws there too (an animated change sometimes never did,
  # even once Glovebox was in front again: research/redraw-probe), then the player clicks Glovebox in the Dock.
  cmd noop  # "the app didn't take" if its main thread is stuck
  sleep 2; log "  in the back: $(status | tr ' ' '\n' | grep -E '^(install|card|asks|active)=' | tr '\n' ' ')"
  winshot S1-card-behind
  has S1 "the card behind Finder" "$(texts)" "macOS will ask for your Mac password once"
  gbfront; sleep 2
  log "  at the card: $(status | tr ' ' '\n' | grep -E '^(install|card|asks|active|visible|front)=' | tr '\n' ' ')"
  expect S1 "Glovebox in front after a click in the Dock" "$(field active)" "1"
  expect S1 "a keychain prompt before Continue" "$([ -n "$(sa)" ] && echo yes || echo no)" "no"
  winshot S1-allow-card
  t=$(texts)
  has S1 "the install card" "$t" "macOS will ask for your Mac password once"
  has S1 "the install card" "$t" "It lets Glovebox use your Apple Account"
  continue_card
  if sa_wait; then
    pass "S1: macOS asks once, while Glovebox shows its card"
    guide_check; sleep 1; shot S1-prompt
    log "  prompt: $(sa)"; ax dump SecurityAgent > "$OUT/S1-prompt-ax.txt"
    n=$("$T/windows" SecurityAgent | grep -c .); expect S1 "keychain prompts on screen" "$n" "1"
    # Deny: no build; the card says what happened.
    sa_press Deny
    waitfor "install=error(signing-not-allowed)" 20
    expect S1 "after Deny" "$(field install)" "error\(signing-not-allowed\)"
    sleep 1; winshot S1-denied
    has S1 "the card after Deny" "$(texts)" "Glovebox can't install without this"
    expect S1 "the guide after Deny" "$(field stage)" "off"
    expect S1 "builds started after Deny" "$(builds "$T/s1")" "0"
    # Allow (just once): that one file signs, the key stays as it was, so still no build. Try Again goes
    # straight to macOS's window: the card's why was read already.
    cmd install
    if sa_wait; then
      sa_type "$SPW"; sa_press Allow
      waitfor "install=error(signing-allowed-once)" 30
      expect S1 "after Allow (once)" "$(field install)" "error\(signing-allowed-once\)"
      if [ "$(field install)" = signing ]; then
        log "  after Allow: $("$T/windows" SecurityAgent | grep -c .) prompt(s) up"; shot S1-allow-asked-again
        for _ in 1 2 3 4 5; do [ -n "$(sa)" ] || break; sa_press Deny; sleep 1; done
        waitfor "install=error" 20
      fi
      sleep 1; winshot S1-allowed-once
      t=$(texts); has S1 "the card after Allow" "$t" "Click Always Allow this time"
      expect S1 "the guide after Allow (once)" "$(field stage)" "off"
      expect S1 "builds started after Allow (once)" "$(builds "$T/s1")" "0"
    else fail "S1: no prompt on Try Again ($(status))"; fi
    # Always Allow: the build goes ahead, and nothing asks again.
    cmd install
    if sa_wait; then
      # Clicked where the guide points: it must be Always Allow for this to go on.
      guide_check; sa_type "$SPW"; clickring || sa_press "Always Allow"
      touch "$T/s1-always"
      # The answered prompt takes a moment to check the password and go; a new one after that is a second ask.
      for _ in $(seq 1 20); do [ -n "$(sa)" ] || break; sleep 0.5; done
      asked=no
      for _ in $(seq 1 240); do
        sleep 0.5
        [ -n "$(sa)" ] && { asked=yes; shot S1-asked-again; }
        [ "$(field install)" = done ] && break
      done
      expect S1 "install after Always Allow" "$(field install)" "done"
      expect S1 "a second prompt after Always Allow" "$asked" "no"
      expect S1 "builds after Always Allow" "$(builds "$T/s1-always")" "[1-9]"
      expect S1 "the guide after Always Allow" "$(field stage)" "off"
    else fail "S1: no prompt on the second Try Again ($(status))"; fi
  else
    fail "S1: macOS didn't ask ($(status))"; shot S1-no-prompt
  fi
  for _ in 1 2 3 4 5; do [ -n "$(sa)" ] || break; sa_press Deny; sleep 1; done
  quitall
  security delete-keychain "$SKC" >/dev/null 2>&1
  cp "$T/fake-S1.log" "$OUT/fake-S1.txt" 2>/dev/null
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  security delete-generic-password -s "Glovebox Steam session (dev)" -a current >/dev/null 2>&1

  # S2. A free account (a Personal Team): the same check with its team. visionOS starts its apps only once their
  # developer is trusted on the headset. The game writes Documents/klepton-memprobe.txt in its data container at every
  # launch, and the last page says how to trust only when that file isn't there. With no answer (CI has no headset),
  # a first install here says how, once.
  echo "S2. A free account" > "$T/phase"
  xprefs signed-in-free
  defaults delete $GB trustShown.A1B2C3D4E5 >/dev/null 2>&1
  sigkc S2 -T /usr/bin/codesign
  toinstall S2
  reinstall() { # reinstall <scenario> <what>: Install again from the last page, until it's done again
    cmd install
    for _ in $(seq 1 20); do [ "$(field install)" = done ] || break; sleep 0.5; done
    for _ in $(seq 1 240); do [ "$(field install)" = done ] && break; sleep 0.5; done
    expect "$1" "$2" "$(field install)" "done"
    sleep 1
  }
  cmd install
  for _ in $(seq 1 240); do [ "$(field install)" = done ] && break; sleep 0.5; done
  expect S2 "install with a free account" "$(field install)" "done"
  expect S2 "whether the game has run on the headset (no headset here: no answer)" "$(field ran)" "-"
  expect S2 "the trust steps" "$(field trust)" "steps"
  sleep 1; winshot S2-done
  has S2 "the finished card" "$(texts)" "VPN & Device Management"
  reinstall S2 "a second install"
  t=$(texts)
  if [[ "$t" == *"VPN & Device Management"* ]]; then fail "S2: a second install shows the trust steps again"
  else pass "S2: a second install doesn't show the trust steps again"; fi
  # The game has run on the headset (made up): no trust steps, even on a first install here.
  defaults delete $GB trustShown.A1B2C3D4E5 >/dev/null 2>&1
  cmd ran yes
  reinstall S2 "an install after the game has run on the headset"
  expect S2 "whether the game has run on the headset" "$(field ran)" "yes"
  expect S2 "the trust steps, the game has run there" "$(field trust)" "none"
  sleep 1; winshot S2-done-ran
  t=$(texts); has S2 "the finished card" "$t" "on your Vision Pro"; hasnt S2 "the finished card" "$t" "VPN & Device Management"
  # The game hasn't run there (made up): the trust steps, even after a first install here.
  defaults write $GB trustShown.A1B2C3D4E5 -bool true
  cmd ran no
  reinstall S2 "an install before the game has run on the headset"
  expect S2 "the trust steps, the game hasn't run there" "$(field trust)" "steps"
  sleep 1; winshot S2-done-notrun
  has S2 "the finished card" "$(texts)" "VPN & Device Management"
  cmd ran off
  quitall
  security delete-keychain "$SKC" >/dev/null 2>&1
  cp "$T/fake-S2.log" "$OUT/fake-S2.txt" 2>/dev/null
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  security delete-generic-password -s "Glovebox Steam session (dev)" -a current >/dev/null 2>&1
  xprefs signed-in-paid
fi

# ---------------------------------------------------------------------------------------------
section "U. Update: the steps are checked again first; signing in from the error card installs; a waking headset waits"
# A beta rehearsal: signed out of Xcode, Update on Home went straight to a build that couldn't sign ("Xcode couldn't
# sign the app"). Check Account there led to the sign-in guide and the other steps, then the same card came back.
# The fake engine stands in for Xcode here: its team check fails while "signed out", and its build fails at the
# app step (as an unsigned build does) until build-ok.
if [ -n "$FAKE" ] && [ -x "$FAKE/Contents/MacOS/Glovebox" ]; then
  fakedir() { find "$HOME/Library/Application Support/Glovebox" -path '*/.fake' -type d 2>/dev/null | head -1; }
  signin() { xprefs signed-in-paid; local d; d=$(fakedir); [ -n "$d" ] && echo 999 > "$d/runs-team"; }
  signout() { xprefs signed-out; local d; d=$(fakedir); [ -n "$d" ] && echo 0 > "$d/runs-team"; }
  ubuilds() { find "$HOME/Library/Logs/Glovebox" -name 'build-*.log' -newer "$1" 2>/dev/null | wc -l | tr -d ' '; }
  defaults write $GB shareStutters -bool true

  # U1. Home with an older Glovebox's game on the headset, Xcode signed out. Steam's saved session checks out just
  # after Update (it's checked when the app starts its checks): before, that alone started the build.
  xprefs signed-out
  defaults write $GB installed -data "$(python3 -c 'import json; print(json.dumps({"installed": True, "version": "0.0.1", "latest": "0.0.1"}).encode().hex())')"
  touch "$T/u1"
  fake U1 FAKE_FAIL="team:999" FAKE_TEAM=none FAKE_BUILD_FAIL=app
  cmd steamout
  sleep 2; winshot U1-home
  expect U1 "the page it opens on" "$(field page)" "welcome"
  has U1 "Home" "$(texts)" "A new version of Glovebox: HLA is ready"
  cmd "$(printf 'update\nfakesteam')"
  sleep 1; log "  just after Update: $(status | tr ' ' '\n' | grep -E '^(page|install|steps)=' | tr '\n' ' ')"
  waitfor "page=account" 60 || fail "U1: Update didn't go to the Apple Account step ($(status))"
  sleep 1; winshot U1-account
  expect U1 "the page after Update, signed out" "$(field page)" "account"
  expect U1 "the Apple Account step" "$(stepstate account)" "attention"
  expect U1 "the install, waiting for the account" "$(field install)" "asked"
  expect U1 "builds started while signed out" "$(ubuilds "$T/u1")" "0"
  # Signing in (the guide's part is section B's): the step is done, and the install starts by itself.
  touch "$(fakedir)/build-ok" "$T/u1-in"
  signin
  for _ in $(seq 1 360); do case "$(field install)" in done|error*) break;; esac; sleep 0.5; done
  expect U1 "the install after signing in" "$(field install)" "done"
  expect U1 "builds after signing in" "$(ubuilds "$T/u1-in")" "[1-9]"
  sleep 1; winshot U1-done
  cp "$T/fake-U1.log" "$OUT/fake-U1.txt" 2>/dev/null
  defaults delete $GB installed >/dev/null 2>&1

  # U2. Signed in, but the build couldn't sign; signed out meanwhile. Check Account on the error card, signing in
  # there: the install starts again by itself, and the card doesn't come back.
  xprefs signed-in-paid
  fake U2 FAKE_FAIL="team:999" FAKE_TEAM=none FAKE_BUILD_FAIL=app
  cmd steamout
  cmd start
  waitfor "checked=1" 60
  signin
  for _ in $(seq 1 60); do [ "$(stepstate account)" = done ] && break; sleep 0.5; done
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  waitfor "install=error(signing-failed)" 120 || fail "U2: the build didn't fail to sign ($(status))"
  sleep 1; winshot U2-error
  has U2 "the error card" "$(texts)" "Couldn't use your Apple Account"
  signout
  r=$(ax press Glovebox "Check Account")
  case "$r" in pressed*) log "  Check Account: $r";; *) log "  Check Account: $r; by the tour instead"; cmd recover account;; esac
  waitfor "page=account" 20
  for _ in $(seq 1 60); do [ "$(stepstate account)" = attention ] && break; sleep 0.5; done
  expect U2 "the page after Check Account" "$(field page)" "account"
  expect U2 "the Apple Account step, signed out" "$(stepstate account)" "attention"
  expect U2 "the install after Check Account" "$(field install)" "asked"
  sleep 1; winshot U2-account
  touch "$(fakedir)/build-ok"
  touch "$T/u2-in"
  signin
  # The install going on by itself, nothing clicked.
  for _ in $(seq 1 240); do case "$(field install)" in running|error*|done) break;; esac; sleep 0.25; done
  [ "$(field install)" = running ] && winshot U2-continuing
  log "  after signing in: $(status | tr ' ' '\n' | grep -E '^(page|install)=' | tr '\n' ' ')"
  again=no
  for _ in $(seq 1 360); do case "$(field install)" in error*) again=yes; break;; done) break;; esac; sleep 0.5; done
  expect U2 "the install after signing in" "$(field install)" "done"
  expect U2 "the error card coming back" "$again" "no"
  expect U2 "builds after signing in" "$(ubuilds "$T/u2-in")" "[1-9]"
  sleep 1; winshot U2-done
  cp "$T/fake-U2.log" "$OUT/fake-U2.txt" 2>/dev/null

  # U3. Update while the headset wakes up: its first two checks say Developer Mode is off, then it's on (a beta
  # rehearsal: that sent the player through the Vision Pro steps though nothing had changed). No Vision Pro step;
  # the install goes ahead.
  defaults write $GB installed -data "$(python3 -c 'import json; print(json.dumps({"installed": True, "version": "0.0.1", "latest": "0.0.1"}).encode().hex())')"
  fake U3 FAKE_FAIL="headset:2" FAKE_HEADSET=devmode
  cmd steamout
  sleep 2
  cmd "$(printf 'update\nfakesteam')"
  pages=""; hstates=""; shot3=""
  for _ in $(seq 1 360); do
    pages="$pages $(field page)"; hstates="$hstates $(stepstate headset)"
    # The Vision Pro step still checking while it says Developer Mode is off.
    if [ -z "$shot3" ] && [ "$(field hs)" = settling ]; then shot3=yes; winshot U3-checking; fi
    case "$(field install)" in done|error*) break;; esac
    sleep 0.5
  done
  expect U3 "the install, the headset waking up" "$(field install)" "done"
  if [[ "$pages" == *headset* ]]; then fail "U3: the Vision Pro step came up while the headset woke"; else pass "U3: no Vision Pro step while the headset woke"; fi
  if [[ "$hstates" == *attention* ]]; then fail "U3: the Vision Pro step said something's wrong while the headset woke"
  else pass "U3: the Vision Pro step never said something's wrong while the headset woke"; fi
  [ -n "$shot3" ] && pass "U3: the Vision Pro step was checking while the headset woke" || fail "U3: never saw the Vision Pro step checking"
  sleep 1; winshot U3-done
  cp "$T/fake-U3.log" "$OUT/fake-U3.txt" 2>/dev/null

  # U4. Developer Mode really off: the Vision Pro step comes up, after 10 s of saying so.
  fake U4 FAKE_FAIL="headset:999" FAKE_HEADSET=devmode
  cmd steamout
  sleep 2
  t0=$(date +%s)
  cmd "$(printf 'update\nfakesteam')"
  waitfor "page=headset" 90 || fail "U4: the Vision Pro step didn't come up ($(status))"
  took=$(( $(date +%s) - t0 ))
  expect U4 "the page with Developer Mode off" "$(field page)" "headset"
  expect U4 "the Vision Pro step" "$(stepstate headset)" "attention"
  expect U4 "seconds before it came up (at least 10)" "$took" "[1-9][0-9]+"
  sleep 1; winshot U4-devmode
  cp "$T/fake-U4.log" "$OUT/fake-U4.txt" 2>/dev/null
  defaults delete $GB installed >/dev/null 2>&1
  quitall
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  security delete-generic-password -s "Glovebox Steam session (dev)" -a current >/dev/null 2>&1
  xprefs signed-in-paid
fi

# ---------------------------------------------------------------------------------------------
section "V. Xcode's saved sign-in expired, then Apple's new agreement: their own cards, then the install by itself"
# A beta 3 tester: Xcode's saved sign-in had expired, so the build said "Unable to log in with account … The login
# details for account … were rejected" while Xcode's account and team lists still said signed in. The card said to
# check the account, that check passed, and the install looped. The fake engine fails the app step with the tester's
# message until build-ok; the real Xcode 27 shows its Settings and Apple's sign-in (nobody signs in: Escape).
if [ -n "$FAKE" ] && [ -x "$FAKE/Contents/MacOS/Glovebox" ]; then
  vdir() { find "$HOME/Library/Application Support/Glovebox" -path '*/.fake' -type d 2>/dev/null | head -1; }
  vfails() { local f; f="$(vdir)/build-failed"; [ -f "$f" ] && wc -l < "$f" | tr -d ' ' || echo 0; }
  vbuilds() { find "$HOME/Library/Logs/Glovebox" -name 'build-*.log' -newer "$1" 2>/dev/null | wc -l | tr -d ' '; }
  # The install from the moment it restarts: the pages it shows on the way, and how it ends (and the checks run
  # meanwhile, which the restart waits for).
  vfollow() { # vfollow <seconds>
    VPAGES=""
    touch "$T/vfollow"
    local t0; t0=$(date +%s)
    for _ in $(seq 1 $(( $1 * 2 ))); do
      VPAGES="$VPAGES $(field page)"
      case "$(field install)" in error*|done) break;; esac
      sleep 0.5
    done
    log "  pages on the way: $(echo $VPAGES | tr ' ' '\n' | uniq | tr '\n' ' ')after $(( $(date +%s) - t0 )) s, checks run: $(find "$HOME/Library/Logs/Glovebox" -name 'doctor-*.log' -newer "$T/vfollow" 2>/dev/null | wc -l | tr -d ' '), install=$(field install) steps=$(field steps) stage=$(field stage) active=$(field active) front=$(field front)"
  }
  vguide() { # vguide <seconds>: the guide on its first stages
    for _ in $(seq 1 $(( $1 * 5 ))); do case "$(field stage)" in open|click) return 0;; esac; sleep 0.2; done
    return 1
  }
  # Glovebox's own buttons aren't reachable by accessibility on CI (U2's Check Account isn't either): the tour
  # presses them, the way the button does. The cards' pictures show the buttons.
  signinbutton() { cmd guide signinagain; }
  xprefs signed-in-paid
  defaults write $GB shareStutters -bool true
  fake V FAKE_BUILD_FAIL=signin
  cmd steamout
  cmd start
  waitfor "checked=1" 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  touch "$T/v0"
  cmd install

  # V1. The card: its own words and buttons, not "Couldn't use your Apple Account" with Check Account.
  waitfor "install=error(xcode-signin-expired)" 120 || fail "V1: the expired sign-in didn't get its own card ($(status))"
  sleep 1; winshot V1-card
  t=$(texts); log "  card: $t"
  expect V1 "the install" "$(field install)" "error\(xcode-signin-expired\)"
  has V1 "the card" "$t" "Sign in to Xcode again"
  has V1 "the card" "$t" "It needs your password once more."
  hasnt V1 "the card" "$t" "Check Account"
  hasnt V1 "the card" "$t" "Couldn't use your Apple Account"
  expect V1 "builds that failed" "$(vfails)" "1"

  # V2. Sign In: the guide opens Xcode's Settings on Apple Accounts and points at the account; after a click there,
  # Apple's sign-in; once that's gone the install starts again by itself, straight to the build, and fails the same
  # way (still expired): the same card.
  signinbutton
  for _ in $(seq 1 90); do "$T/windows" Xcode | grep -q 'layer=[08] ' && break; sleep 1; done
  sleep 4
  for i in 1 2 3 4 5 6 7 8; do
    [ "$(field stage)" = welcome ] || break
    out=$(ax press Xcode Continue); log "  first launch $i: $out"
    echo "$out" | grep -q "^pressed" || ax pressat Xcode AXButton -1 >/dev/null
    sleep 4
  done
  vguide 20
  t=$(texts); log "  guide: $t"
  has V2 "Glovebox while the guide runs" "$t" "Follow the steps next to Xcode"
  hasnt V2 "Glovebox while the guide runs" "$t" "Sign in to Xcode again"
  if [ "$(field stage)" = open ]; then
    shot V2-guide-open
    has V2 "the bubble" "$t" "Press these keys"
    osascript -e 'tell application id "com.apple.dt.Xcode" to activate' >/dev/null 2>&1; sleep 1
    key "," "command down"
  fi
  if waitfor "stage=click ring=[0-9]" 20; then
    sleep 1; shot V2-guide-click
    t=$(texts); log "  bubble: $t"
    has V2 "the bubble at Settings" "$t" "Click your Apple Account"
    has V2 "the bubble at Settings" "$t" "Then sign in again."
    "$T/windows" Xcode | tee -a "$LOG" > /dev/null
    clickring
    if waitfor "stage=finish" 8; then
      sleep 1; shot V2-guide-clicked
      t=$(texts); log "  bubble: $t"
      has V2 "the bubble after the click" "$t" "Sign in again"
    else
      fail "V2: a click on the account didn't move the guide on ($(status))"
    fi
  else
    fail "V2: the guide didn't find Xcode's Settings ($(status))"
  fi
  # CI has no account to sign in to again: Apple's sign-in from the pane's own Sign In… stands in for the account's.
  # Accessibility doesn't reach it: a click where it is (76 × 24, 20 pt in from the right edge, 158 pt down on Xcode 27).
  if ! texts | grep -q "Type your Apple Account password"; then
    w=$("$T/windows" Xcode | grep 'name=Apple Accounts' | head -1)
    if [[ "$w" =~ x=([0-9.-]+)\ y=([0-9.-]+)\ w=([0-9.]+)\ h=([0-9.]+) ]]; then
      bx=$(( ${BASH_REMATCH[1]%.*} + ${BASH_REMATCH[3]%.*} - 20 - 38 )); by=$(( ${BASH_REMATCH[2]%.*} + 158 + 12 ))
      mouse "$bx" "$by" click >/dev/null; log "  clicked Xcode's Sign In… at $bx,$by"
    else
      log "  no Apple Accounts window: $("$T/windows" Xcode | tr '\n' ';')"
    fi
  fi
  for _ in $(seq 1 40); do texts | grep -q "Type your Apple Account password" && break; sleep 0.5; done
  t=$(texts); log "  bubble: $t"
  sleep 1; shot V2-guide-sheet
  has V2 "the bubble at Apple's sign-in" "$t" "Type your Apple Account password"
  has V2 "the bubble at Apple's sign-in" "$t" "Same as on your iPhone."
  sleep 5
  expect V2 "the guide while Apple's sign-in is up" "$(field stage)" "finish"
  touch "$T/v2"
  escape
  waitfor "stage=off" 20 || fail "V2: the guide didn't end when Apple's sign-in closed ($(status))"
  sleep 1
  expect V2 "the app in front after the guide" "$(field front)" "$GB"
  vfollow 180
  [[ "$VPAGES" == *account* ]] && fail "V2: the restart went through the Apple Account step" || pass "V2: the restart went straight to the install (no Apple Account step)"
  expect V2 "the install after signing in again, still expired" "$(field install)" "error\(xcode-signin-expired\)"
  expect V2 "builds since the guide ended" "$(vbuilds "$T/v2")" "[1-9]"
  expect V2 "builds that failed" "$(vfails)" "2"
  sleep 1; winshot V2-card-again
  has V2 "the card again" "$(texts)" "Sign in to Xcode again"

  # V3. Try Again (the other button): the same build, the same card.
  [ "$(field stage)" = off ] || { fail "V3: the guide still running before Try Again ($(field stage)): stopped"; cmd guide stop; sleep 1; }
  touch "$T/v3"
  cmd install
  vfollow 180
  expect V3 "the install after Try Again, still expired" "$(field install)" "error\(xcode-signin-expired\)"
  expect V3 "builds that failed" "$(vfails)" "3"

  # V4. Signed in again for real (the build goes through now): Sign In, Apple's sign-in comes and goes, and the
  # install finishes by itself.
  touch "$(vdir)/build-ok" "$T/v4"
  signinbutton
  vguide 30 || fail "V4: the guide didn't start ($(status))"
  cmd guide signedin
  waitfor "stage=off" 20 || fail "V4: the guide didn't end after the sign-in ($(status))"
  vfollow 240
  [[ "$VPAGES" == *account* ]] && fail "V4: the restart went through the Apple Account step" || pass "V4: the restart went straight to the install"
  expect V4 "the install after signing in again" "$(field install)" "done"
  expect V4 "builds after signing in again" "$(vbuilds "$T/v4")" "[1-9]"
  expect V4 "builds that failed" "$(vfails)" "3"
  sleep 1; winshot V4-done
  cp "$T/fake-V.log" "$OUT/fake-V.txt" 2>/dev/null

  # V5. The same tester, signed in again: Apple's new developer agreement wasn't accepted yet. Its own card; the
  # button opens Apple's website; back in Glovebox, the install starts again by itself, straight to the build.
  fake V5 FAKE_BUILD_FAIL=agreement
  cmd steamout
  cmd start
  waitfor "checked=1" 60
  cmd fakesteam
  for _ in $(seq 1 30); do [ "$(field download)" = 100 ] && break; sleep 1; done
  cmd go install; sleep 1
  cmd install
  waitfor "install=error(apple-agreement-pending)" 120 || fail "V5: the agreement didn't get its own card ($(status))"
  sleep 1; winshot V5-card
  t=$(texts); log "  card: $t"
  has V5 "the card" "$t" "Accept Apple's new terms"
  has V5 "the card" "$t" "Accept it on Apple's website, then come back."
  hasnt V5 "the card" "$t" "Check Account"
  expect V5 "builds that failed" "$(vfails)" "1"
  cmd agreement
  for _ in $(seq 1 40); do [ "$(field front)" != "$GB" ] && break; sleep 0.5; done
  web=$(field front); log "  in front after the button: $web"
  [ "$web" != "$GB" ] && pass "V5: the button opened Apple's website ($web in front)" || fail "V5: nothing came up after the button"
  sleep 4; shot V5-website
  expect V5 "the install while the player is on the website" "$(field install)" "error\(apple-agreement-pending\)"
  expect V5 "builds that failed" "$(vfails)" "1"
  # Accepted on the website: back to Glovebox.
  touch "$(vdir)/build-ok" "$T/v5"
  gbfront
  # The restart, then where it goes.
  for _ in $(seq 1 40); do [ "$(field install)" != "error(apple-agreement-pending)" ] && break; sleep 0.5; done
  log "  back in Glovebox: install=$(field install) active=$(field active) front=$(field front)"
  vfollow 240
  [[ "$VPAGES" == *account* ]] && fail "V5: the restart went through the Apple Account step" || pass "V5: back in Glovebox, the install went straight on (no Apple Account step)"
  expect V5 "the install after accepting" "$(field install)" "done"
  expect V5 "builds after coming back" "$(vbuilds "$T/v5")" "[1-9]"
  sleep 1; winshot V5-done
  [ -n "$web" ] && [ "$web" != "$GB" ] && quitapp "$web" "$web"
  cp "$T/fake-V5.log" "$OUT/fake-V5.txt" 2>/dev/null
  quitall
  rm -rf "$HOME/Library/Application Support/Glovebox/alyx"
  security delete-generic-password -s "Glovebox Steam session (dev)" -a current >/dev/null 2>&1
  xprefs signed-in-paid
fi

# ---------------------------------------------------------------------------------------------
section "N. No Xcode at all: Get Xcode opens the App Store, and Xcode arriving clears the step"
# Every Xcode moves out of /Applications and out of Launch Services; coming back and registering
# is what the App Store's install does.
LSREG=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
HOLD=/private/var/tmp/xcodes.noindex
quitall
xprefs signed-in-paid
sudo mkdir -p "$HOLD"
MOVED=()
for x in /Applications/Xcode*.app; do
  [ -L "$x" ] && continue
  # Not ending in .app: Launch Services follows a moved app to its new place (run 8 found it there).
  # Launch Services can still follow it there (run 11 found the held one): forget both places.
  sudo mv "$x" "$HOLD/$(basename "$x").held" && MOVED+=("$(basename "$x")") && "$LSREG" -u "$x" >/dev/null 2>&1
  "$LSREG" -u "$HOLD/$(basename "$x").held" >/dev/null 2>&1
done
log "  moved out: ${MOVED[*]}; Launch Services paths there: $("$LSREG" -dump 2>/dev/null | grep -c 'xcodes.noindex')"
# What the App Store's download puts in Applications first: Xcode's app without its tools yet, and
# maybe without its Info.plist, so Launch Services doesn't know it. (Not at Xcode.app: on CI that's
# a link to the real one, moved out above.)
PLACE=/Applications/Xcode-download.app
placeholder() {
  sudo mkdir -p "$PLACE/Contents"
}
if launch N; then
  cmd start
  settle N 60
  expect N "xcode check" "$(check xcode)" "fail\(xcode-missing\)"
  expect N "Xcode step" "$(stepstate xcode)" "attention"
  expect N "first page" "$(field page)" "xcode"
  cmd go xcode; sleep 2; winshot N-get-xcode
  # Get Xcode: the App Store's page, with the guide on it.
  cmd front; sleep 1
  cmd getxcode
  for _ in $(seq 1 60); do "$T/windows" "App Store" | grep -q "layer=0" && break; sleep 1; done
  sleep 12
  shot N-app-store
  "$T/windows" "App Store" | head -3 >> "$LOG"
  if pgrep -x "App Store" >/dev/null; then pass "N: Get Xcode opens the App Store"; else fail "N: Get Xcode didn't open the App Store"; fi
  expect N "the App Store guide's stage" "$(field stage)" "click"
  t=$(texts); log "  bubble: $t"
  has N "the App Store bubble" "$t" "Click Get"
  winshot N-guide-card
  has N "Glovebox's card while the guide runs" "$(texts)" "Follow the steps next to"
  if [ "$(field ring)" != none ]; then
    note "N: the guide points at Get (ring at $(field ring))"
    clickring; sleep 3; shot N-app-store-2
    t=$(texts); log "  bubble after Get: $t"
    expect N "the App Store guide after clicking Get" "$(field stage)" "confirm|finish"
    [ "$(field stage)" = confirm ] && has N "the App Store bubble after Get" "$t" "Click Install"
  else
    note "N: the guide shows a picture of Get beside the App Store (this window size isn't measured)"
  fi
  # The download starts: Xcode's app shows up in Applications, the guide is done and Glovebox
  # comes back to the front by itself.
  placeholder
  waitfor "stage=off" 30 && pass "N: the guide closes once Xcode starts downloading" || fail "N: the guide didn't notice the download ($(status))"
  sleep 2
  expect N "the app in front after the App Store" "$(field front)" "$GB"
  expect N "xcode check while it downloads" "$(check xcode)" "fail\(xcode-missing\)"
  expect N "Xcode step while it downloads" "$(stepstate xcode)" "working"
  winshot N-downloading
  has N "the Xcode card while it downloads" "$(texts)" "Xcode is downloading"
  # The player moved on to another app: when Xcode is in, Glovebox doesn't jump in front of it.
  open -a Finder; sleep 2
  "$LSREG" -u "$PLACE" >/dev/null 2>&1; sudo rm -rf "$PLACE"
  for x in "${MOVED[@]}"; do sudo mv "$HOLD/$x.held" "/Applications/$x" && "$LSREG" -f "/Applications/$x" >/dev/null 2>&1; done
  for _ in $(seq 1 90); do [ "$(check xcode)" = ok ] && break; sleep 1; done
  expect N "xcode check once Xcode is in /Applications" "$(check xcode)" "ok"
  sleep 3
  expect N "the app in front when Xcode is in, the player in Finder" "$(field front)" "com.apple.finder"
  settle N 60
  expect N "Xcode step once Xcode is in" "$(stepstate xcode)" "done"
  cmd front; sleep 1; winshot N-after
fi
[ -d "$PLACE" ] && { "$LSREG" -u "$PLACE" >/dev/null 2>&1; sudo rm -rf "$PLACE"; }
for x in "${MOVED[@]}"; do [ -d "$HOLD/$x.held" ] && sudo mv "$HOLD/$x.held" "/Applications/$x" && "$LSREG" -f "/Applications/$x" >/dev/null 2>&1; done
osascript -e 'tell application "App Store" to quit' >/dev/null 2>&1

# ---------------------------------------------------------------------------------------------
section "DMG. The disk image's window (scripts/dmg)"
# A player double-clicks the download: the volume mounts and Finder opens its window by itself.
quitall
if [ -n "$DMG" ] && [ -f "$DMG" ]; then
  # The Finder window that wasn't there before (N left one open).
  dmgwin() { "$T/windows" Finder | grep 'layer=0' | grep -vxF -f "$T/finder-before" | sed -E 's/.* x=([0-9.-]+) y=([0-9.-]+) w=([0-9.]+) h=([0-9.]+).*/\1 \2 \3 \4/' | awk '$3 > 400 && $3 < 900 && $4 > 250 && $4 < 700 { printf "%d,%d,%d,%d\n", $1, $2, $3, $4 }' | head -1; }
  for mode in false true; do
    dark $mode; sleep 2
    { "$T/windows" Finder | grep 'layer=0'; echo none; } > "$T/finder-before"
    open "$DMG"
    for _ in $(seq 1 40); do [ -d /Volumes/Glovebox/Glovebox.app ] && [ -n "$(dmgwin)" ] && break; sleep 1; done
    sleep 3
    look=$([ $mode = true ] && echo dark || echo light)
    r=$(dmgwin)
    if [ "$mode" = false ]; then
      if [ -d /Volumes/Glovebox/Glovebox.app ] && [ -L /Volumes/Glovebox/Applications ]; then pass "DMG: it mounts with Glovebox and an Applications shortcut"
      else fail "DMG: the volume doesn't have Glovebox and Applications ($(ls -la /Volumes/Glovebox 2>&1 | tr '\n' ' '))"; fi
      [ -f /Volumes/Glovebox/.VolumeIcon.icns ] && pass "DMG: the volume carries the app's icon" || note "DMG: no volume icon"
      if [ -n "$r" ]; then pass "DMG: Finder opens its window by itself ($r)"; else fail "DMG: no Finder window for the disk image"; fi
    fi
    if [ -n "$r" ]; then screencapture -x -R "$r" "$OUT/DMG-window-$look.png"; fi
    shot "DMG-screen-$look"
    "$T/windows" Finder >> "$LOG"
    hdiutil detach /Volumes/Glovebox -force >/dev/null 2>&1; sleep 2
  done
  dark false
else
  note "DMG: no disk image given"
fi

# ---------------------------------------------------------------------------------------------
section "K. Nothing of Glovebox's runs in the background"
# A beta rehearsal: macOS 27 said Glovebox ran in the background and listed it in Login Items &
# Extensions › Allow in the Background. Build 2026100614 started the engine and devicectl through
# a copy of itself with no window (Glovebox -spawn-disclaimed); now they're its plain children and
# stop when it quits (H, H3, H4). After every run above: no background item, no notification about it.
# First a crash: Glovebox killed while the engine checks the Mac. The engine notices the app is gone
# (GLOVEBOX_APP_PID) and stops with what it started.
if launch K; then
  cmd start
  e=""
  for _ in $(seq 1 60); do e=$(pgrep -f "glovebox\.py doctor" | head -1); [ -n "$e" ] && break; sleep 0.25; done
  if [ -n "$e" ]; then
    noteQuit "kill -9"
    pkill -9 -f "Glovebox.app/Contents/MacOS/Glovebox"
    sleep 4
    left=$(leftovers)
    if [ -n "$left" ]; then fail "K: 4 s after Glovebox was killed mid-check, still running: $(echo "$left" | tr '\n' ';')"
    else pass "K: Glovebox killed mid-check: the engine stopped by itself, nothing left running"; fi
  else
    note "K: no doctor run caught to kill Glovebox in the middle of ($(status))"
  fi
fi
quitall
sleep 40
sudo sfltool dumpbtm > "$OUT/btm-end.txt" 2>&1
btmlog() { sudo log show --start "$BTM_START" --style compact --info --debug --predicate 'subsystem BEGINSWITH "com.apple.backgroundtaskmanagement" OR process == "backgroundtaskmanagementd" OR process == "BackgroundTaskManagementAgent"' 2>/dev/null; }
btmlog > "$OUT/btm-log.txt"
n0=$(grep -c 'UUID:' "$OUT/btm-start.txt"); n1=$(grep -c 'UUID:' "$OUT/btm-end.txt")
log "  background items: $n0 at the start, $n1 now; background task log: $(wc -l < "$OUT/btm-log.txt" | tr -d ' ') lines"
g=$(grep -iE -B3 -A12 'glovebox-vr\.installer|Glovebox\.app' "$OUT/btm-end.txt")
if [ -n "$g" ]; then fail "K: Glovebox is a background item (btm-end.txt):"; echo "$g" | head -20 | sed 's/^/    /' | tee -a "$R"
else pass "K: no background item of Glovebox's after every run above ($n1 items, $n0 at the start; btm-end.txt)"; fi
reg=$(grep -iE 'glovebox' "$OUT/btm-log.txt" | grep -iE 'shouldRegister:(true|1)|should post background tasks=(true|1)|registered')
if [ -n "$reg" ]; then fail "K: Background Task Management registered Glovebox (btm-log.txt):"; echo "$reg" | head -5 | cut -c1-260 | sed 's/^/    /' | tee -a "$R"
else pass "K: Background Task Management never registered Glovebox (no shouldRegister:true)"; fi
gl=$(grep -iE 'glovebox' "$OUT/btm-log.txt")
if [ -n "$gl" ]; then note "K: the background task log names Glovebox $(echo "$gl" | wc -l | tr -d ' ') times (btm-log.txt):"; echo "$gl" | head -6 | cut -c1-260 | sed 's/^/    /' | tee -a "$R"
else pass "K: the background task log never names Glovebox"; fi
# Each time macOS checked Glovebox for background tasks: the quit just before it, and what was still running.
grep 'checkTasksAllowedInBackgroundForAppURL: result' "$OUT/btm-log.txt" | while read -r _ t rest; do
  s=${t%%.*}; q=$(awk -v s="$s" '$1 <= s' "$OUT/quits.txt" 2>/dev/null | tail -1)
  note "K: a background check at $s, ${rest##*result=}; the quit before it: ${q:-none}"
done
bg=$(awk '/^== /{h=$0; next} {print h" "$0}' "$OUT/banners.txt" 2>/dev/null | grep -i glovebox)
if [ -n "$bg" ]; then fail "K: a notification about Glovebox (banners.txt, banner-*.png):"; echo "$bg" | head -5 | cut -c1-260 | sed 's/^/    /' | tee -a "$R"
else pass "K: no notification about Glovebox the whole run ($(grep -c '^== ' "$OUT/banners.txt" 2>/dev/null || echo 0) banner changes seen)"; fi

# ---------------------------------------------------------------------------------------------
section "K2. Controls: a helper with no window, and the host name lookup, in an app of their own"
# They show the checks see what they look for on this Mac. Repro (repro.swift) starts itself with no
# window the way build 2026100614 did and quits with it still running; then it looks the Mac's name
# up the way that made macOS ask about the local network. Its own bundle id, so nothing macOS
# remembers about it lands on Glovebox; last, as macOS remembers answers.
RA="$T/Repro.app"
rm -rf "$RA"; mkdir -p "$RA/Contents/MacOS"
xcrun swiftc -O -o "$RA/Contents/MacOS/Repro" "$HERE/repro.swift" 2>&1 | grep -v warning | head -5
cat > "$RA/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>io.github.glovebox-vr.repro</string>
<key>CFBundleExecutable</key><string>Repro</string>
<key>CFBundleName</key><string>Repro</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
</dict></plist>
PLIST
codesign -s - --force "$RA" >/dev/null 2>&1
rquit() { osascript -e 'with timeout of 5 seconds' -e 'tell application id "io.github.glovebox-vr.repro" to quit' -e 'end timeout' >/dev/null 2>&1; }
if [ -x "$RA/Contents/MacOS/Repro" ]; then
  open -n "$RA" --args -helper 90
  sleep 8
  log "  Repro running: $(pgrep -lf "Repro.app/Contents/MacOS/Repro" | tr '\n' ';')"
  # The tester's Mac registered Glovebox 33 s after the quit.
  rquit; sleep 50
  shot K2-after-quit
  log "  after quitting Repro, its helper: $(pgrep -lf "Repro -spawn-disclaimed" | tr '\n' ';')"
  sudo sfltool dumpbtm > "$OUT/btm-control.txt" 2>&1
  btmlog | grep -i repro > "$OUT/btm-log-control.txt"
  if grep -qiE 'glovebox-vr\.repro|Repro\.app' "$OUT/btm-control.txt"; then
    pass "K2: the control: Repro's windowless helper makes it a background item here (btm-control.txt), so K would see Glovebox's"
  else
    note "K2: the control: Repro's helper left no background item on this Mac ($(wc -l < "$OUT/btm-log-control.txt" | tr -d ' ') background task log lines name it): K's item check is weak here"
  fi
  rb=$(awk '/^== /{h=$0; next} {print h" "$0}' "$OUT/banners.txt" 2>/dev/null | grep -i repro | head -3 | cut -c1-200)
  if [ -n "$rb" ]; then note "K2: the control's notification: $rb"; else note "K2: no notification named Repro"; fi
  pkill -f "Repro.app/Contents/MacOS/Repro" 2>/dev/null
  # The host name lookup.
  before=$(lprompts)
  open -n "$RA" --args -hostname
  sleep 12
  after=$(lprompts)
  shot K2-hostname
  if [ "$after" -gt "$before" ]; then
    pass "K2: the control: an app looking its host name up gets the local network alert here, so L and Prompts would see Glovebox's"
    log "  local network prompt: $(ax pressat UserNotificationCenter AXButton 0)"
  else
    note "K2: the control: the host name lookup got no local network alert on this Mac: L's checks are weak here"
  fi
  rquit; sleep 2; pkill -f "Repro.app/Contents/MacOS/Repro" 2>/dev/null
else
  note "K2: Repro didn't build"
fi

# ---------------------------------------------------------------------------------------------
section "Prompts"
kill $WATCHER 2>/dev/null
quitall
if grep -v "Xcode's own setup.*owner=SecurityAgent" "$OUT/prompts.txt" 2>/dev/null | grep -v "^[0-9:]* \[K2\. " | grep -v "^[0-9:]* \[S1\. " | grep -q .; then
  fail "a password or permission prompt showed up (prompts.txt, prompt-*.png):"
  sed 's/^/    /' "$OUT/prompts.txt" | head -20 | tee -a "$R"
else
  pass "no password or permission prompt from Glovebox the whole time (the K2 controls and S1's key aside)"
  grep -q . "$OUT/prompts.txt" 2>/dev/null && { note "Xcode's own prompts while it set itself up (expected):"; sed 's/^/    /' "$OUT/prompts.txt" | head -5 | tee -a "$R"; }
fi

printf '\n%s\n' "$FAILS failed" | tee -a "$R"
[ "$FAILS" = 0 ]
