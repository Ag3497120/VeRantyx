#!/bin/bash
# Half-Life: Alyx's guest for the visionOS app — mkguest.sh's job for a Linux
# guest, whose libraries are not one APK directory but the depot's two bin/
# folders, the launcher ELF and the Debian C++ runtime.
#
# Writes, for the slices in XROS_PLATFORMS (xros, xrsim):
#   Frameworks/alyx/<lib>.xcframework   each ELF, translated by klepton-ld
#   Frameworks/alyx/libs.txt            their names (targets.py's "@" list)
#   Frameworks/KleptonAlyx.xcframework  static: libklalyx.a + a no-JIT LuaJIT
#   Frameworks/SDL3.xcframework         SDL3 (src/build-sdl3-<slice>)
#   Frameworks/SDL2.xcframework         sdl2-compat, wrapped as SDL2.framework
#
# Inputs (see patches/sdl2-compat/README.md in the project repo for the SDL
# and LuaJIT builds):
#   ALYX_DEPOT   default ~/alyx-depot (linux/game, sysroot, src/)
#   make alyx-<slice> (build/<slice>/libklalyx.a) must have run.
# Options:
#   KLEPTON_LDWRAP=1     relink each translation with Apple's linker (tools/klepton_ldwrap.py: the same bytes
#                        at the same addresses, plus what an Xcode-built dylib has). klepton-ld's own output is
#                        kept in build/guest/alyx-kl/ and only rewrapped when it changes.
#   KLEPTON_GUEST_OUT    where the guest xcframeworks and libs.txt go (default Frameworks/alyx; gen_xcodeproj.py
#                        takes another folder as KLEPTON_GUEST_DIR). Its frameworks are staged in
#                        build/guest/<its last component>.
#   KLEPTON_GUEST_ONLY=1 stop after the guest libraries (leaves KleptonAlyx, SDL3 and SDL2 as they are)
#   KLEPTON_RUNTIME_ONLY=1 skip the guest libraries: only KleptonAlyx, SDL3 and SDL2 (Glovebox's prebuilt kit,
#                        which needs no game)
#   KLEPTON_DEPS         the dependency builds (LuaJIT-<slice>, SDL3, build-sdl3-<slice>, build-sdl2c-<slice>;
#                        default $ALYX_DEPOT/src)
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
LD="$ROOT/build/klepton-ld"
DEPOT="${ALYX_DEPOT:-$HOME/alyx-depot}"
DEPS="${KLEPTON_DEPS:-$DEPOT/src}"
GAME="$DEPOT/linux/game"
RUNTIME_ONLY="${KLEPTON_RUNTIME_ONLY:-0}"
OUT="${KLEPTON_GUEST_OUT:-Frameworks/alyx}"; OUT=${OUT%/}
STAGE="build/guest/$(basename "$OUT")"
WRAP="$ROOT/tools/klepton_ldwrap.py"
if [ "$RUNTIME_ONLY" != 1 ]; then
  [ -x "$LD" ] || { echo "!! $LD missing — run 'make build/klepton-ld' first"; exit 1; }
  [ -d "$GAME/bin/linuxsteamrtarm64" ] || { echo "!! no depot at $GAME"; exit 1; }
fi

SLICES=""
for P in ${XROS_PLATFORMS:-xros xrsim}; do
  case "$P" in xros|xrsim) SLICES="$SLICES $P" ;; *) echo "!! XROS_PLATFORMS: '$P'"; exit 1 ;; esac
done
klplat() { [ "$1" = xros ] && echo visionos || echo visionossim; }
fwdir()  { [ "$1" = xros ] && echo xros || echo xrsimulator; }
bplat()  { [ "$1" = xros ] && echo XROS || echo XRSimulator; }   # CFBundleSupportedPlatforms
# A framework's Info.plist, with the keys App Store processing checks on every nested bundle: the versions,
# the platform, and a MinimumOSVersion equal to the binary's own (LC_BUILD_VERSION minos; a plist below the
# binary is ITMS-90208).
# fwplist <framework dir> <executable> <bundle id> <short version> <slice>
fwplist() {
  local minos
  minos=$(vtool -show-build "$1/$2" | awk '$1 == "minos" {print $2; exit}')
  [ -n "$minos" ] || { echo "!! no minos in $1/$2"; exit 1; }
  cat > "$1/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$2</string>
  <key>CFBundleIdentifier</key><string>$3</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$2</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$4</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$(bplat $5)</string></array>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>$minos</string>
  <key>UIDeviceFamily</key><array><integer>7</integer></array>
</dict></plist>
PLIST
}

if [ "$RUNTIME_ONLY" != 1 ]; then
mkdir -p "$OUT" "$STAGE"
: > "$OUT/libs.txt.new"

# Every ELF the launcher can reach. Symlinks (hlvr.sh's soname links) are the
# same file under another name: the runtime asks by stem, so one translation
# answers both.
SRCS=()
for f in "$GAME"/bin/linuxsteamrtarm64/*.so* "$GAME"/hlvr/bin/linuxsteamrtarm64/*.so* \
         "$GAME"/bin/linuxsteamrtarm64/hlvr "$DEPOT"/sysroot/*.so*; do
  [ -L "$f" ] && continue; [ -f "$f" ] || continue
  head -c4 "$f" | grep -q ELF || continue
  SRCS+=("$f")
done

n=0
for f in "${SRCS[@]}"; do
  b=$(basename "$f"); NAME=${b%%.so*}
  FWNAME=$(printf '%s' "$NAME" | tr '+' 'x')
  ID_NAME=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9.-' '-')
  FWARGS=()
  for S in $SLICES; do
    FW="$STAGE/$(fwdir $S)/$FWNAME.framework"
    mkdir -p "$FW"
    T="$FW/$FWNAME"
    [ "${KLEPTON_LDWRAP:-0}" = 1 ] && { T="build/guest/alyx-kl/$(fwdir $S)/$FWNAME"; mkdir -p "$(dirname "$T")"; }
    if [ ! -f "$T" ] || [ "$f" -nt "$T" ] || [ "$LD" -nt "$T" ]; then
      "$LD" "$f" -o "$T" --platform "$(klplat $S)" \
            --install-name "@rpath/$FWNAME.framework/$FWNAME" --quiet
    fi
    if [ "$T" != "$FW/$FWNAME" ] && { [ ! -f "$FW/$FWNAME" ] || [ "$T" -nt "$FW/$FWNAME" ] || [ "$WRAP" -nt "$FW/$FWNAME" ]; }; then
      python3 "$WRAP" "$T" -o "$FW/$FWNAME" > /dev/null
    fi
    fwplist "$FW" "$FWNAME" "dev.klepton.guest.alyx.$ID_NAME" 1.0 "$S"
    FWARGS+=(-framework "$FW")
  done
  rm -rf "$OUT/$NAME.xcframework"
  xcodebuild -create-xcframework "${FWARGS[@]}" -output "$OUT/$NAME.xcframework" > /dev/null
  echo "$NAME" >> "$OUT/libs.txt.new"
  n=$((n + 1))
done
sort -u "$OUT/libs.txt.new" | tr '\n' ' ' > "$OUT/libs.txt"; rm -f "$OUT/libs.txt.new"
echo "[mkguest_alyx] $n guest libraries -> $OUT/$([ "${KLEPTON_LDWRAP:-0}" = 1 ] && echo ' (ld-wrapped)')"
fi
[ "${KLEPTON_GUEST_ONLY:-0}" = 1 ] && exit 0

# The runtime add-on and LuaJIT, one static library per slice.
LIBARGS=()
for S in $SLICES; do
  LJ="$DEPS/LuaJIT-$S/src/libluajit.a"
  [ -f "$ROOT/build/$S/libklalyx.a" ] || { echo "!! build/$S/libklalyx.a missing — make alyx-$S"; exit 1; }
  [ -f "$LJ" ] || { echo "!! $LJ missing — see the LuaJIT build in the README"; exit 1; }
  mkdir -p "build/alyx-static/$S"
  libtool -static -o "build/alyx-static/$S/libKleptonAlyx.a" "$ROOT/build/$S/libklalyx.a" "$LJ" 2>/dev/null
  LIBARGS+=(-library "build/alyx-static/$S/libKleptonAlyx.a")
done
rm -rf Frameworks/KleptonAlyx.xcframework
xcodebuild -create-xcframework "${LIBARGS[@]}" -output Frameworks/KleptonAlyx.xcframework > /dev/null
echo "[mkguest_alyx] Frameworks/KleptonAlyx.xcframework"

# SDL3 and sdl2-compat. sdl2-compat builds a bare dylib; the bundle wants a
# framework (Xcode signs what it embeds under Frameworks/), so wrap it, with
# the install name the runtime dlopens (KL_SDL2_DYLIB = Frameworks/SDL2.framework/SDL2).
# SDL3's own framework (cmake) has no version keys in its Info.plist, which App
# Store processing requires of every bundle: a copy gets them.
SDL3_VERSION=$(awk '/#define SDL_(MAJOR|MINOR|MICRO)_VERSION/ {v = v (v ? "." : "") $3} END {print v}' \
               "$DEPS/SDL3/include/SDL3/SDL_version.h")
S3ARGS=(); S2ARGS=()
for S in $SLICES; do
  B3="$DEPS/build-sdl3-$S/Release-$(fwdir $S)/SDL3.framework"
  B2="$DEPS/build-sdl2c-$S/Release-$(fwdir $S)/libSDL2-2.0.0.dylib"
  [ -d "$B3" ] && [ -f "$B2" ] || { echo "!! SDL builds for $S missing ($B3, $B2)"; exit 1; }
  F3="build/alyx-sdl3/$(fwdir $S)/SDL3.framework"
  rm -rf "$F3"; mkdir -p "$(dirname "$F3")"; cp -R "$B3" "$F3"
  for k in CFBundleShortVersionString CFBundleVersion; do
    plutil -extract $k raw "$F3/Info.plist" >/dev/null 2>&1 || plutil -insert $k -string "$SDL3_VERSION" "$F3/Info.plist"
  done
  S3ARGS+=(-framework "$F3")
  FW="build/alyx-sdl2/$(fwdir $S)/SDL2.framework"
  mkdir -p "$FW"
  cp "$B2" "$FW/SDL2"
  install_name_tool -id @rpath/SDL2.framework/SDL2 "$FW/SDL2"
  fwplist "$FW" SDL2 org.libsdl.sdl2-compat 2.32.72 "$S"
  S2ARGS+=(-framework "$FW")
done
rm -rf Frameworks/SDL3.xcframework Frameworks/SDL2.xcframework
xcodebuild -create-xcframework "${S3ARGS[@]}" -output Frameworks/SDL3.xcframework > /dev/null
xcodebuild -create-xcframework "${S2ARGS[@]}" -output Frameworks/SDL2.xcframework > /dev/null
echo "[mkguest_alyx] Frameworks/SDL3.xcframework, Frameworks/SDL2.xcframework"
