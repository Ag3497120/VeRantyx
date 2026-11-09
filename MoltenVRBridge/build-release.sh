#!/bin/bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
kl="$repo/NativeARM/build/Klepton"
if [[ ! -d "$kl/.git" ]]; then git clone https://github.com/shinyquagsire23/Klepton.git "$kl"; fi
commit=$(cat "$here/klepton/COMMIT")
if [[ -n "$(git -C "$kl" status --porcelain)" ]]; then
 echo 'Use a clean Klepton checkout for this complete patch (not upstream apply.py).' >&2; exit 1
fi
git -C "$kl" checkout "$commit"
git -C "$kl" apply "$here/klepton/working-runtime.patch"
cp -R "$here/klepton/new/runtime/." "$kl/runtime/"
python3 "$repo/NativeARM/klepton-patches/apply.py" "$kl"
make -C "$kl" mvk
fz="$repo/NativeARM/build/Fossilize"
if [[ ! -d "$fz/.git" ]]; then git clone https://github.com/ValveSoftware/Fossilize.git "$fz"; fi
git -C "$fz" checkout 556da9ff7d7616704dda515756219ab1dab693ec
git -C "$fz" submodule update --init --recursive
make -C "$repo/NativeARM/host" FOSSILIZE=1 MVK_DIR='./moltenvk-game'
sdk="$repo/NativeARM/build/OpenXR-SDK"
if [[ ! -d "$sdk/.git" ]]; then git clone --depth 1 --branch release-1.1.49 https://github.com/KhronosGroup/OpenXR-SDK.git "$sdk"; fi
if command -v glslangValidator >/dev/null; then
 glslangValidator -V "$here/warp.vert" -o "$here/warp.vert.spv"
 glslangValidator -V "$here/warp.frag" -o "$here/warp.frag.spv"
elif command -v glslc >/dev/null; then
 glslc "$here/warp.vert" -o "$here/warp.vert.spv"
 glslc "$here/warp.frag" -o "$here/warp.frag.spv"
else
 echo 'Install glslang or shaderc first.' >&2; exit 1
fi
clang++ -arch arm64 -std=c++17 -O2 "$here/bridge.cpp" -I"$sdk/include" \
 -I"$kl/vendor-moltenvk/out/include" -L"$kl/vendor-moltenvk/out/macos" -lMoltenVK -o "$here/bridge"
mvk_link=$(otool -L "$here/bridge" | awk '/libMoltenVK.dylib/ {print $1; exit}')
install_name_tool -change "$mvk_link" '@loader_path/libMoltenVK.dylib' "$here/bridge"
codesign --force --sign - "$here/bridge"
xcrun swiftc -emit-library -O -target arm64-apple-macos14.2 "$here/loading/LoadingScene.swift" -o "$here/libalyxloading.dylib"
install_name_tool -id '@loader_path/libalyxloading.dylib' "$here/libalyxloading.dylib"
codesign --force --sign - "$here/libalyxloading.dylib"
echo 'Built alyx_host, bridge, loading renderer and warp shaders. See RELEASE_PACKAGING.md for the binary allowlist.'
