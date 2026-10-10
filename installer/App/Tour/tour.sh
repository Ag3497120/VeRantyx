#!/bin/bash
# The walkthrough video: the Glovebox app's main flow and other paths, with captions, and its
# Xcode guide on the real Xcode and Device Hub. CI only (.github/workflows/installer-video.yml):
# it presses keys and moves the pointer, so never run it on a Mac someone is using.
#
#   tour.sh <Glovebox.app (Debug)> <out folder>   ->  <out>/glovebox-walkthrough.mp4
set -u
BUNDLE=$1
OUT=$2
T="$OUT/tour"
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"
rm -f "$T"/next "$T"/status
X=$(xcode-select -p | sed 's#/Contents/Developer##')
echo "Xcode: $X ($(xcodebuild -version | head -1)), macOS $(sw_vers -productVersion)"
for tool in display windows trim; do xcrun swiftc -O -o "$T/$tool" "$HERE/$tool.swift" 2>&1 | grep -v warning | head -5; done
# More room than CI's 1024x768 where the display allows it, and no Dock in the way.
"$T/display"
defaults write com.apple.dock autohide -bool true && killall Dock
# CI's Macs come with Reduce Motion and Reduce Transparency on, which stills the guide's keys and
# makes glass solid. Turn them off if the runner allows it; the app's status line says (rm, rt).
for k in reduceMotion reduceTransparency; do
  echo "$k was $(defaults read com.apple.universalaccess $k 2>&1 | head -1)"
  defaults write com.apple.universalaccess $k -bool false 2>&1 | head -2
done
sleep 3

ax() { osascript -l JavaScript "$HERE/ax.js" "$@" 2>&1 | head -c 3000; echo; }
key() { osascript -e "tell application \"System Events\" to keystroke \"$1\" using {$2}" 2>&1; }
escape() { osascript -e 'tell application "System Events" to key code 53' 2>&1; }
front() { osascript -e "tell application id \"$1\" to activate" >/dev/null 2>&1; }
frontapp() { osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true' 2>&1; }
quitapp() { osascript -e "tell application id \"$1\" to quit" >/dev/null 2>&1; sleep 3; pkill -f "$2" 2>/dev/null; }
elapsed() { echo $(( $(date +%s) - START )); }
mouse() { osascript -l JavaScript "$HERE/mouse.js" "$@" 2>&1; }
shot() { screencapture -x "$OUT/step-$1.png"; }

# The app takes one command at a time (Tour.swift).
cmd() {
  echo "[$(elapsed)s] $*"
  printf '%s\n' "$*" > "$T/next.tmp" && mv "$T/next.tmp" "$T/next"
  for _ in $(seq 1 50); do [ -f "$T/next" ] || return 0; sleep 0.1; done
  echo "the app didn't take: $*"
}
say() { cmd "caption $1|$2"; }
hold() { sleep "$1"; }
status() { cat "$T/status" 2>/dev/null; }
waitfor() { # waitfor <pattern> <seconds>
  for _ in $(seq 1 $(( $2 * 5 ))); do status | grep -q "$1" && return 0; sleep 0.2; done
  echo "timed out waiting for $1 ($(status))"; return 1
}
clickring() {
  local at; at=$(status | grep -o 'ring=[0-9]*,[0-9]*' | cut -d= -f2)
  [ -n "$at" ] || { echo "no ring"; return 1; }
  mouse "${at%,*}" "${at#*,}" click
}

# Xcode's first launch (Xcode 27: Components, External Agent Access, What's New), walked before
# recording with nothing to download: the video starts the way a set-up Mac looks.
walk_first_launch() {
  for p in iphoneos watchos appletvos xros; do defaults write com.apple.dt.Xcode "IDEPlatformsFirstLaunchSelected-$p" -bool NO; done
  defaults write com.apple.dt.Xcode IDEFirstLaunchSuppressPlatformChooser -bool YES
  open -a "$X"; sleep 30
  for page in 1 2 3 4 5 6 7 8; do
    names=$(ax windows Xcode); echo "first launch $page: $names"
    if [ -z "${names//[[:space:]]/}" ] || echo "$names" | grep -q "Welcome to Xcode\|no windows"; then break; fi
    out=$(ax press Xcode Continue); echo "$out"
    echo "$out" | grep -q "^pressed" || ax pressat Xcode AXButton -1
    sleep 4
  done
  screencapture -x "$OUT/xcode-after-first-launch.png"
  quitapp com.apple.dt.Xcode "$X/Contents/MacOS/Xcode"
}

# Both recorders write their file only when their time runs out: a signal stops screencapture
# without saving it. So the recording runs a fixed time, longer than the tour, and the last
# caption stays up until it ends. Without a file from a short test, ffmpeg records instead.
record() { # record <seconds>
  mkfifo "$T/keys"; exec 3<>"$T/keys"
  screencapture -v -V 3 -x "$T/test.mov" <"$T/keys" >/dev/null 2>&1
  local size; size=$(stat -f%z "$T/test.mov" 2>/dev/null || echo 0)
  echo "test recording: $size bytes"
  if [ "$size" -gt 20000 ]; then
    RECORDER=screencapture
    screencapture -v -V "$1" -C -k -x "$OUT/raw.mov" <"$T/keys" >"$T/rec.log" 2>&1 &
  else
    RECORDER=ffmpeg
    command -v ffmpeg >/dev/null || brew install ffmpeg >/dev/null 2>&1
    ffmpeg -hide_banner -f avfoundation -list_devices true -i "" 2>&1 | grep -i "screen" | head -3
    ffmpeg -nostdin -y -loglevel error -f avfoundation -framerate 30 -capture_cursor 1 -capture_mouse_clicks 1 -i "Capture screen 0:none" \
      -t "$1" -c:v libx264 -preset veryfast -pix_fmt yuv420p "$OUT/raw.mov" >"$T/rec.log" 2>&1 &
  fi
  REC=$!
}

end_recording() { # wait for the recorder's time to run out
  for _ in $(seq 1 600); do kill -0 "$REC" 2>/dev/null || break; sleep 1; done
  echo "recorder: $RECORDER, file $(stat -f%z "$OUT/raw.mov" 2>/dev/null || echo none)"; tail -5 "$T/rec.log" 2>/dev/null
}

walk_first_launch

open -n "$BUNDLE" --args -demo welcome -appearance light -tour "$T"
for _ in $(seq 1 60); do [ -f "$T/status" ] && break; sleep 0.5; done
mouse 1500 900
sleep 4
# The tour takes about 320 s; the rest of the recording is the closing caption, cut off later.
MAX=370
record $MAX
sleep 2
START=$(date +%s)

# ---- Main flow ----
say "Glovebox" "Half-Life: Alyx on Apple Vision Pro, built on your Mac from your own Steam copy."; hold 6
say "Main flow" "Five steps along the top. Each card is one thing to do, with one big button."; hold 3
# A click on the step bar, which sits where the title bar would be: it has to reach the app.
win=$("$T/windows" Glovebox | grep "name=Glovebox" | head -1)
wx=$(echo "$win" | sed -E 's/.* x=([0-9]+).*/\1/'); wy=$(echo "$win" | sed -E 's/.* y=([0-9]+).*/\1/'); ww=$(echo "$win" | sed -E 's/.* w=([0-9]+).*/\1/')
mouse $(( wx + ww / 2 )) $(( wy + 25 )) click; hold 1
echo "step bar click: $(status)"
hold 2

cmd state xcode-get
say "1 · Xcode" "No Xcode yet: Get Xcode opens the App Store, with a guide."; hold 4

# ---- The App Store guide, on the real App Store ----
cmd guide appstore
for _ in $(seq 1 40); do "$T/windows" "App Store" | grep -q "layer=0" && break; sleep 1; done
"$T/windows" "App Store"
hold 4
echo "App Store guide: $(status)"
shot appstore
say "1 · Xcode · real App Store" "It shows what to click: Get, then Install. (This test Mac's App Store can't connect.)"; hold 6
say "1 · Xcode · real App Store" "Once the download starts, the guide closes and Glovebox comes back."; hold 4
cmd guide stop
osascript -e 'tell application "App Store" to quit' >/dev/null 2>&1
cmd front
cmd state xcode-downloading
say "1 · Xcode" "You can continue while it downloads."; hold 4
cmd state xcode-setup
say "1 · Xcode" "Then one click to finish its setup."; hold 4

# ---- Xcode's first open, on the real Xcode, its license not accepted yet ----
for k in IDEXcodeVersionForAgreedToGMLicense IDELastGMLicenseAgreedTo IDEXcodeVersionForAgreedToBetaLicense IDELastBetaLicenseAgreedTo; do
  sudo defaults delete /Library/Preferences/com.apple.dt.Xcode "$k" >/dev/null 2>&1
done
cmd guide setup
say "1 · Xcode · real Xcode" "Open Xcode opens it, with the guide."
for _ in $(seq 1 60); do "$T/windows" Xcode | grep -q "layer=8" && break; sleep 1; done
"$T/windows" Xcode
if waitfor "stage=click ring=[0-9]" 20; then
  hold 2
  shot xcode-license
  say "1 · Xcode · real Xcode" "It points at Agree on Xcode's license."; hold 5
  clickring
else
  hold 4
fi
if waitfor "stage=confirm" 15; then
  hold 1
  shot xcode-password
  say "1 · Xcode · real Xcode" "Then the Mac's password, the one that unlocks it. (Skipped in this video.)"; hold 6
  escape >/dev/null
fi
echo "setup guide: $(status)"
cmd guide stop
sudo "$X/Contents/Developer/usr/bin/xcodebuild" -license accept
(quitapp com.apple.dt.Xcode "$X/Contents/MacOS/Xcode") >/dev/null 2>&1 &
cmd front
say "1 · Xcode" "Then Xcode's own pages, with the guide on each Continue."; hold 4
cmd state xcode-visionos
say "1 · Xcode" "visionOS gets added by itself."; hold 4

cmd state steam-signin
say "2 · Steam" "Scan the code with the Steam app. No password needed."; hold 7
cmd state steam-scanned
say "2 · Steam" "Then tap Approve on the phone."; hold 4
cmd state steam-owned
say "2 · Steam" "You own the game: it starts downloading right away."; hold 5

cmd state account-signin
say "3 · Apple Account" "Sign In opens Xcode, and a guide shows you how."; hold 5

# ---- The Xcode guide, on the real Xcode ----
cmd guide signin
say "3 · Apple Account · real Xcode" "The guide waits for Xcode, then appears right next to it."
for _ in $(seq 1 60); do "$T/windows" Xcode | grep -q "layer=0" && break; sleep 1; done
"$T/windows" Xcode
hold 4
say "3 · Apple Account · real Xcode" "First, the keys to press. They keep pressing themselves until you do."; hold 6
# Pressed while Glovebox is in front, the keys can't reach Xcode: the guide brings Xcode forward.
cmd front; sleep 1
say "3 · Apple Account · real Xcode" "Pressed in Glovebox by mistake? Xcode comes forward for another go."
key "," "command down"; sleep 2
echo "after ⌘, in Glovebox: front=$(frontapp) $(status)"
hold 2
key "," "command down"
if waitfor "stage=click ring=[0-9]" 15; then
  shot settings
  say "3 · Apple Account · real Xcode" "Then it jumps to the exact button, with an arrow and a ring."; hold 5
  clickring
  waitfor "stage=finish" 6 || { echo "the click didn't open the sign-in: pressing the button"; ax press Xcode "Sign In…"; }
else
  "$T/windows" Xcode
  ax press Xcode "Sign In…"
fi
waitfor "stage=finish" 10
# Apple's sheet slides in; the bubble moves to its Apple Account field once it's there.
for _ in $(seq 1 20); do "$T/windows" Xcode | grep -q "name= layer=0 .*w=480 h=600" && break; sleep 0.5; done
hold 2
"$T/windows" Xcode
shot apple-signin
say "3 · Apple Account · real Xcode" "Then it points at Apple's sign-in. (Skipped in this video.)"; hold 6
cmd state account-team
say "3 · Apple Account" "Signed in: the guide closes and the app comes back."; hold 4
(escape; sleep 1; quitapp com.apple.dt.Xcode "$X/Contents/MacOS/Xcode") >/dev/null 2>&1 &
cmd front
say "3 · Apple Account" "Only if you have more than one team, you pick one."; hold 5

# ---- Pairing, on the real Device Hub ----
cmd state headset-pair
say "4 · Apple Vision Pro" "Pair opens Device Hub, with the guide."; hold 5
cmd guide pair
say "4 · Apple Vision Pro · real Device Hub" "The guide sits right next to +."
if ! waitfor "ring=[0-9]" 40; then
  echo "Device Hub: $(defaults read "$X/Contents/Applications/DeviceHub.app/Contents/Info" CFBundleIdentifier 2>&1)"
  lsappinfo list 2>/dev/null | grep -i -A3 "device hub" | head -8; "$T/windows"
fi
shot device-hub; hold 4
plus=$(status | grep -o 'ring=[0-9]*,[0-9]*')
clickring; hold 2
echo "after +: $(status)"; "$T/windows" "Device Hub"
shot device-hub-menu
say "4 · Apple Vision Pro · real Device Hub" "Then at Pair Nearby Device."; hold 4
now=$(status | grep -o 'ring=[0-9]*,[0-9]*')
if [ -n "$now" ] && [ "$now" != "$plus" ]; then
  clickring; waitfor "stage=device ring=[0-9]" 6
else
  echo "the guide didn't see the menu"; escape >/dev/null
fi
hold 1
echo "after Pair Nearby Device: $(status)"; "$T/windows" "Device Hub"
shot device-hub-pair
say "4 · Apple Vision Pro · real Device Hub" "Then at Vision Pro, a device type: Device Hub then looks for the headset."; hold 4
clickring; waitfor "stage=finish" 5; hold 1
echo "after Vision Pro: $(status)"; "$T/windows" "Device Hub"
shot device-hub-visionpro
say "4 · Apple Vision Pro · real Device Hub" "Then Remote Devices on the headset, and pick it here."; hold 5
escape >/dev/null
# The code window needs a real headset: a stand-in of its size (Tour.swift).
cmd guide paircode; waitfor "stage=finish ring=[0-9]" 6; hold 1
echo "code window: $(status)"
shot device-hub-code
say "4 · Apple Vision Pro" "The headset shows a code. You type it here, on the Mac."; hold 6
cmd guide stop
cmd state headset-devmode
say "4 · Apple Vision Pro" "Paired: the app notices by itself."; hold 4
(quitapp com.apple.dt.Devices DeviceHub) >/dev/null 2>&1 &
cmd front
say "4 · Apple Vision Pro" "Last, Developer Mode on the headset."; hold 5

# ---- Install ----
cmd state install-ready
say "5 · Install" "One click builds the game and installs it over Wi-Fi."; hold 5
cmd state install-running
say "5 · Install" "One ring for the whole install."; hold 4
cmd state install-consent
say "5 · Install" "While it works, it asks once for help making Glovebox better."; hold 7
cmd answer share; hold 1
cmd state install-headsetwait
say "5 · Install" "If the headset is asleep, it waits."; hold 4
cmd state install-done
say "5 · Install" "Done: the game is on the Vision Pro."; hold 5
cmd state home
say "6 · Next time" "Next time it opens here, with updates and renewals."; hold 5

# ---- Other paths ----
cmd state steam-password
say "Other paths · Steam" "Or sign in with a password, then approve in the Steam app."; hold 5
cmd state steam-approve
say "Other paths · Steam" "No phone at hand? Enter a code instead."; hold 4
cmd state steam-code
say "Other paths · Steam" "The Steam Guard code, from the app or email."; hold 4
cmd state steam-notowned
say "Other paths · Steam" "An account without the game."; hold 4
cmd state account-free
say "Other paths · Apple Account" "A free Apple Account works too: reinstall it every 7 days."; hold 5
cmd state install-signing
say "Other paths · Install" "With the App Store's Xcode, macOS asks once, before the first build."; hold 5
cmd state install-done-free
say "Other paths · Install" "A free account's first install: trust it once on the Vision Pro."; hold 6
cmd state home-renew-free
say "Other paths · Home" "Two days before a free account's week runs out: Renew."; hold 5
cmd state install-error
say "Other paths · Install" "If something fails, it says what, with Try Again."; hold 5
say "" "End of the walkthrough."; hold 3

LENGTH=$(elapsed)
echo "tour: $LENGTH s"
screencapture -x "$OUT/last-frame.png"
end_recording
ls -la "$OUT"
"$T/trim" "$OUT/raw.mov" "$OUT/glovebox-walkthrough.mp4" $(( LENGTH + 4 ))
pkill -f "$BUNDLE/Contents/MacOS/Glovebox" 2>/dev/null
ls -la "$OUT"
