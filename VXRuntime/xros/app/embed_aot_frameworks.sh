#!/bin/sh
# Xcode build phase (project.yml): the module frameworks tools/xros_aot.sh made ($VXRT_LIBDIR/aot_fw) into the app's
# Frameworks folder, each signed with the app's identity before Xcode signs the app (translator/aot.c opens them with
# dlopen). Nothing there: nothing to do (the static registry has the modules).
set -e
src="$VXRT_LIBDIR/aot_fw"
dst="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH"
mkdir -p "$dst"
rm -rf "$dst"/VXA_*.framework
[ -d "$src" ] || exit 0
id=${EXPANDED_CODE_SIGN_IDENTITY:--}
n=0
for f in "$src"/VXA_*.framework; do
    [ -d "$f" ] || continue
    ditto "$f" "$dst/$(basename "$f")"
    codesign --force --sign "$id" --timestamp=none --generate-entitlement-der "$dst/$(basename "$f")"
    n=$((n + 1))
done
echo "embed_aot_frameworks: $n frameworks signed with ${EXPANDED_CODE_SIGN_IDENTITY_NAME:-$id}"
