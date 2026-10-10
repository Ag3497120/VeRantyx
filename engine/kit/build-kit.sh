#!/bin/zsh
# build-kit.sh [out dir]: the Glovebox runtime kit, everything a player's Mac links into the game app that is the
# same for every player and is all open source: Klepton's runtime (Klepton.xcframework), its game add-on with LuaJIT
# (KleptonAlyx.xcframework), MoltenVK, SDL3, sdl2-compat (as SDL2.framework) and klepton-ld. Players download it
# from a release, so their Mac needs only Xcode: it translates their game and signs, and builds nothing else.
# Maintainers and CI run this; it needs cmake (Homebrew) on top of Xcode.
#   GLOVEBOX_DEPS  where the dependency sources and builds live (default <repo>/src, gitignored); cloned at the
#                  pinned versions below when missing
#   KIT_VERSION    the kit's version (default: the repo's short commit)
set -euo pipefail
G=${0:A:h:h:h}; K=$G/klepton; SRC=${GLOVEBOX_DEPS:-$G/src}
OUT=${1:-$G/build/kit}; OUT=${OUT:A}; VER=${KIT_VERSION:-$(git -C $G rev-parse --short HEAD)}
SDL3_TAG=release-3.4.16; SDL2C_TAG=release-2.32.72
LUAJIT_REV=c6ffc141; FOSSILIZE_REV=556da9f
mkdir -p $SRC
say() { print -r -- "[kit] $*"; }

clone() {  # clone <url> <dir> <tag-or-rev>
  [ -d $2/.git ] && return
  say "fetching $(basename $2) $3"
  if [[ $3 == release-* ]]; then git clone -q --depth 1 --branch $3 $1 $2
  else git clone -q $1 $2 && git -C $2 checkout -q $3 && git -C $2 submodule -q update --init --recursive; fi
}
clone https://github.com/libsdl-org/SDL.git $SRC/SDL3 $SDL3_TAG
clone https://github.com/libsdl-org/sdl2-compat.git $SRC/sdl2-compat $SDL2C_TAG
git -C $SRC/sdl2-compat apply --check $G/deps/sdl2-compat/0001-visionos-build.patch 2>/dev/null &&
  git -C $SRC/sdl2-compat apply $G/deps/sdl2-compat/0001-visionos-build.patch
clone https://github.com/LuaJIT/LuaJIT $SRC/LuaJIT $LUAJIT_REV
clone https://github.com/ValveSoftware/Fossilize.git $SRC/Fossilize $FOSSILIZE_REV

XR=$(xcrun --sdk xros --show-sdk-path)
if [ ! -d $SRC/build-sdl3-xros/Release-xros/SDL3.framework ]; then
  say "building SDL3 for visionOS"
  cmake -S $SRC/SDL3 -B $SRC/build-sdl3-xros -G Xcode -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=1.0 -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_FRAMEWORK=ON > /dev/null
  cmake --build $SRC/build-sdl3-xros --config Release -- CODE_SIGNING_ALLOWED=NO > /dev/null
fi
if [ ! -f $SRC/build-sdl2c-xros/Release-xros/libSDL2-2.0.0.dylib ]; then
  say "building sdl2-compat for visionOS"
  cmake -S $SRC/sdl2-compat -B $SRC/build-sdl2c-xros -G Xcode -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=1.0 -DSDL2COMPAT_TESTS=OFF \
    -DSDL3_INCLUDE_DIRS=$SRC/SDL3/include > /dev/null
  cmake --build $SRC/build-sdl2c-xros --config Release -- CODE_SIGNING_ALLOWED=NO > /dev/null
fi
if [ ! -f $SRC/LuaJIT-xros/src/libluajit.a ]; then
  say "building LuaJIT (interpreter only) for visionOS"
  rm -rf $SRC/LuaJIT-xros; cp -R $SRC/LuaJIT $SRC/LuaJIT-xros
  make -C $SRC/LuaJIT-xros -s clean > /dev/null
  make -C $SRC/LuaJIT-xros -s HOST_CC="clang -arch arm64" CROSS="" TARGET_CC=clang \
    TARGET_FLAGS="-target arm64-apple-xros1.0 -isysroot $XR" TARGET_SYS=iOS \
    XCFLAGS="-DLUAJIT_ENABLE_GC64 -DLUAJIT_NO_UNWIND -DLUAJIT_DISABLE_JIT" BUILDMODE=static > /dev/null
fi

say "building Klepton (runtime, game add-on, klepton-ld, MoltenVK)"
cd $K
# No build Mac's paths in what ships: renamed at compile time, and a clean build so every object gets it
export KL_PATHMAP="-ffile-prefix-map=$K/=klepton/ -ffile-prefix-map=$G/=glovebox/ -ffile-prefix-map=$SRC/=deps/"
export KL_LD_PATHMAP="-oso_prefix $K/"
rm -rf build/xros build/xrsim build/klepton-ld build/klepton-ld.dSYM build/Klepton.xcframework
make -s mvk > /dev/null
make -s FZ_DIR=$SRC/Fossilize XROS_PLATFORMS=xros build/Klepton.xcframework build/klepton-ld > /dev/null
make -s FZ_DIR=$SRC/Fossilize alyx-xros > /dev/null
(cd visionos && bash mkmvk.sh > /dev/null)
(cd visionos && KLEPTON_RUNTIME_ONLY=1 KLEPTON_DEPS=$SRC XROS_PLATFORMS=xros bash mkguest_alyx.sh > /dev/null)

say "assembling kit $VER"
rm -rf $OUT; mkdir -p $OUT/Frameworks $OUT/bin
cp build/klepton-ld $OUT/bin/
ditto build/Klepton.xcframework $OUT/Klepton.xcframework
for f in KleptonAlyx SDL3 SDL2 MoltenVK; do ditto visionos/Frameworks/$f.xcframework $OUT/Frameworks/$f.xcframework; done
# The license of everything in the kit, as the binaries it carries require (tools/licenses.py: one list)
xcrun python3 $G/tools/licenses.py kit $OUT/LICENSES
{ print "{"
  print "  \"kit\": \"$VER\","
  print "  \"klepton\": \"$(git -C $G log -1 --format=%h -- klepton)\","
  print "  \"sdl3\": \"$SDL3_TAG\", \"sdl2_compat\": \"$SDL2C_TAG\", \"luajit\": \"$LUAJIT_REV\", \"fossilize\": \"$FOSSILIZE_REV\","
  print "  \"moltenvk\": \"$(awk -F'"' '/^MVK_VERSION=/{print $2}' tools/mvk_fetch.sh)\""
  print "}"; } > $OUT/KIT.json
# no macOS metadata (._ files) in the zip
(cd ${OUT:h} && rm -f ${OUT:t}.zip && ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent ${OUT:t} ${OUT:t}.zip)
say "$OUT.zip ($(du -sh $OUT.zip | cut -f1))"
