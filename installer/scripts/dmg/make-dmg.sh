#!/usr/bin/env bash
# Makes the Glovebox disk image: the app, an Applications shortcut, and the branded window (background art,
# icon positions, no toolbar). Used by release.sh; CI runs it on an unsigned app to look at the window.
#   scripts/dmg/make-dmg.sh <Glovebox.app> <out.dmg> [work dir]
# dmgbuild (pinned in requirements.txt, hash-checked) goes in a throwaway venv in the work dir. It writes the
# window's layout file itself, so nothing scripts Finder and no Automation prompt appears.
set -euo pipefail
ART="$(cd "$(dirname "$0")" && pwd)"
APP="${1:?usage: make-dmg.sh <Glovebox.app> <out.dmg> [work dir]}"
OUT="${2:?usage: make-dmg.sh <Glovebox.app> <out.dmg> [work dir]}"
WORK="${3:-$(dirname "$OUT")/dmgbuild}"
[ -d "$APP" ] || { echo "No app at $APP"; exit 1; }

if [ ! -x "$WORK/venv/bin/dmgbuild" ]; then
  xcrun python3 -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" install -q --disable-pip-version-check --no-cache-dir --only-binary :all: --require-hashes \
    -r "$ART/requirements.txt"
fi
# The volume shows the app's icon in Finder's sidebar and on the desktop.
ICON="$APP/Contents/Resources/AppIcon.icns"
[ -f "$ICON" ] || ICON=
mkdir -p "$(dirname "$OUT")" && rm -f "$OUT"
# dmgbuild 1.6.5 also records the background as a bookmark (pBBk), and with it Finder shows no background at all
# (macOS 26.2 and later, dmgbuild#273; CI saw it on 15.7 too). dmgbuild 1.6.7 dropped the bookmark (dmgbuild#275) but needs Python 3.10 and
# Xcode's is 3.9, so the same change is made here: the background is found by its alias alone.
"$WORK/venv/bin/python" -I - "$ART/settings.py" "$APP" "$ART" "$ICON" "$OUT" <<'PY'
import sys
import dmgbuild.core
settings, app, art, icon, out = sys.argv[1:]
dmgbuild.core.Bookmark.for_file = staticmethod(lambda path: None)
dmgbuild.core.build_dmg(out, "Glovebox", settings, defines={"app": app, "art": art, "icon": icon})
PY
