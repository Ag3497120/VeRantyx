#!/usr/bin/env bash
# Builds, signs and notarizes a Glovebox installer release, with the Sparkle appcast
# that existing copies update from. Runs on the Mac that holds the Developer ID certificate.
#
# Once, on that Mac:
#   1. Developer ID certificate: Xcode › Settings › Accounts › Manage Certificates › + ›
#      Developer ID Application.
#   2. Notary login, saved in the keychain:
#        xcrun notarytool store-credentials glovebox-notary --apple-id <email> --team-id <TEAMID>
#   3. The update-signing key: `scripts/release.sh keys`. It makes the EdDSA key pair (the private
#      half stays in the login keychain) and prints the public half; put that in App/project.yml as
#      GLOVEBOX_SPARKLE_KEY and commit it. Never change it afterwards, or existing copies stop
#      accepting updates.
#   4. Nothing for the upload: the files are attached to the GitHub release v<version> of glovebox-vr/glovebox.
#      Every release attaches its own appcast.xml, so releases/latest/download/appcast.xml (the app's
#      GLOVEBOX_APPCAST_URL) is always the newest, and a fixed-name Glovebox.dmg, so
#      releases/latest/download/Glovebox.dmg is always the newest first download.
#
# Each release:
#   Write release-notes/<version>.md first: two or three plain lines on what changed, shown in the update window.
#   GLOVEBOX_SRC=<glovebox-vr/glovebox checkout at the release tag> GLOVEBOX_KIT_ZIP=<its runtime kit> \
#     scripts/release.sh 0.1.0 [build-number]
# It publishes nothing itself: it ends by listing the files to upload.

set -euo pipefail
# VeRantyx Standalone (this branch) is an unofficial build: it is not signed, notarized or published as a Glovebox
# release, and must never be attached to glovebox-vr/glovebox's releases or signed with its update key.
echo "This branch (VeRantyx Standalone) has no release flow: build it locally (README.md)."; exit 1
cd "$(dirname "$0")/.."
ROOT=$PWD
PROFILE="${GLOVEBOX_NOTARY_PROFILE:-glovebox-notary}"
BUILD_DIR="$ROOT/App/build/release"

sparkle_bin() {
  local bin
  bin=$(find "$ROOT/App/build" -type d -path '*artifacts/sparkle/Sparkle/bin' 2>/dev/null | head -1)
  if [ -z "$bin" ]; then
    (cd App && xcodegen -q && xcodebuild -resolvePackageDependencies -project Glovebox.xcodeproj -scheme Glovebox -derivedDataPath build >/dev/null)
    bin=$(find "$ROOT/App/build" -type d -path '*artifacts/sparkle/Sparkle/bin' | head -1)
  fi
  echo "$bin"
}

if [ "${1:-}" = keys ]; then
  "$(sparkle_bin)/generate_keys"
  exit 0
fi

VERSION="${1:?usage: release.sh <version> [build-number] | keys}"
BUILD="${2:-$(date +%Y%m%d%H)}"
# Where this release's files are served from: its GitHub release. The appcast's download links point here.
DOWNLOADS="${GLOVEBOX_DOWNLOADS:-https://github.com/glovebox-vr/glovebox/releases/download/v$VERSION}"

# Preconditions, each with the fix.
IDENTITY=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -1)
[ -n "$IDENTITY" ] || { echo "No Developer ID Application certificate (step 1 above)."; exit 1; }
TEAM=$(echo "$IDENTITY" | sed -n 's/.*(\([A-Z0-9]*\))$/\1/p')
xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1 || { echo "No notary profile '$PROFILE' (step 2)."; exit 1; }
grep -q 'GLOVEBOX_SPARKLE_KEY: "[A-Za-z0-9+/=]\{40,\}"' App/project.yml || { echo "No update key in App/project.yml (step 3)."; exit 1; }
[ -f "${GLOVEBOX_SRC:-}/engine/glovebox.py" ] || { echo "Set GLOVEBOX_SRC to a glovebox-vr/glovebox checkout."; exit 1; }
[ -f "${GLOVEBOX_KIT_ZIP:-}" ] || { echo "Set GLOVEBOX_KIT_ZIP to the release's runtime kit (engine/kit/build-kit.sh)."; exit 1; }
NOTES="$ROOT/release-notes/$VERSION.md"
[ -s "$NOTES" ] || { echo "No release notes: write release-notes/$VERSION.md (see release-notes/README.md)."; exit 1; }

echo "Glovebox $VERSION ($BUILD), signed by $IDENTITY, for $DOWNLOADS"
rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"
# 0. The kit's own Mac tools (klepton-ld runs on the player's Mac) are signed like the app, in a copy of the kit zip:
#    the notary service looks inside zips, and an unsigned executable in one fails the whole app.
mkdir -p "$BUILD_DIR/kit" && ditto -x -k "$GLOVEBOX_KIT_ZIP" "$BUILD_DIR/kit"
find "$BUILD_DIR/kit" -type f -perm +111 | while IFS= read -r f; do
  if file -b "$f" | grep -q "Mach-O.*executable"; then codesign -f -s "$IDENTITY" -o runtime --timestamp "$f"; fi
done
(cd "$BUILD_DIR/kit" && ditto -c -k --norsrc --noextattr --noqtn --keepParent glovebox-kit "$BUILD_DIR/glovebox-kit.zip")
export GLOVEBOX_KIT_ZIP="$BUILD_DIR/glovebox-kit.zip"

# 1. Archive, signed with Developer ID and the hardened runtime. The engine phase copies the
#    Glovebox source and the kit in and signs any binaries among them with the same identity.
(cd App && xcodegen -q && xcodebuild archive -project Glovebox.xcodeproj -scheme Glovebox -configuration Release \
  -derivedDataPath build -archivePath "$BUILD_DIR/Glovebox.xcarchive" \
  MARKETING_VERSION="$VERSION" CURRENT_PROJECT_VERSION="$BUILD" \
  CODE_SIGN_STYLE=Manual CODE_SIGN_IDENTITY="$IDENTITY" DEVELOPMENT_TEAM="$TEAM" OTHER_CODE_SIGN_FLAGS=--timestamp \
  GLOVEBOX_KEYCHAIN_SERVICE="Glovebox Steam session" \
  | grep -E "error:|ARCHIVE (SUCCEEDED|FAILED)")
APP="$BUILD_DIR/Glovebox.app"
ditto "$BUILD_DIR/Glovebox.xcarchive/Products/Applications/Glovebox.app" "$APP"
# Sparkle's helpers keep Sparkle's own signatures through an archive: re-sign them with ours, inside out, as
# Sparkle's documentation does for Developer ID, then the framework and the app.
SPK="$APP/Contents/Frameworks/Sparkle.framework/Versions/B"
codesign -f -s "$IDENTITY" -o runtime --timestamp "$SPK/XPCServices/Installer.xpc"
codesign -f -s "$IDENTITY" -o runtime --timestamp --preserve-metadata=entitlements "$SPK/XPCServices/Downloader.xpc"
codesign -f -s "$IDENTITY" -o runtime --timestamp "$SPK/Autoupdate"
codesign -f -s "$IDENTITY" -o runtime --timestamp "$SPK/Updater.app"
codesign -f -s "$IDENTITY" -o runtime --timestamp "$APP/Contents/Frameworks/Sparkle.framework"
# The Steam session goes in the data protection keychain (no per-app access dialogs after updates) when the app is
# signed with a keychain access group, which a Developer ID app may use only with a provisioning profile that allows
# it: GLOVEBOX_INSTALLER_PROFILE, a Developer ID profile for io.github.glovebox-vr.installer. Without one, the app
# uses the login keychain (SteamKeychain).
ENT=App/Glovebox/Glovebox.entitlements
if [ -n "${GLOVEBOX_INSTALLER_PROFILE:-}" ]; then
  BUNDLE=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$APP/Contents/Info.plist")
  security cms -D -i "$GLOVEBOX_INSTALLER_PROFILE" > "$BUILD_DIR/profile.plist"
  [ "$(/usr/libexec/PlistBuddy -c "Print Entitlements:com.apple.application-identifier" "$BUILD_DIR/profile.plist")" = "$TEAM.$BUNDLE" ] \
    || { echo "GLOVEBOX_INSTALLER_PROFILE is not a profile for $TEAM.$BUNDLE"; exit 1; }
  cp "$GLOVEBOX_INSTALLER_PROFILE" "$APP/Contents/embedded.provisionprofile"
  ENT="$BUILD_DIR/release.entitlements"
  cat > "$ENT" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>com.apple.application-identifier</key><string>$TEAM.$BUNDLE</string>
  <key>com.apple.developer.team-identifier</key><string>$TEAM</string>
  <key>keychain-access-groups</key><array><string>$TEAM.$BUNDLE</string></array>
</dict></plist>
PLIST
else
  echo "No GLOVEBOX_INSTALLER_PROFILE: the Steam session stays in the login keychain."
fi
codesign -f -s "$IDENTITY" -o runtime --timestamp --entitlements "$ENT" "$APP"
codesign --verify --deep --strict --verbose=1 "$APP"

# GLOVEBOX_SIGN_ONLY=1: stop here with the signed app (local checks such as -keychain-selftest; not for release).
if [ "${GLOVEBOX_SIGN_ONLY:-}" = 1 ]; then echo "Signed, not notarized: $APP"; exit 0; fi
# 2. Notarize and staple the app, so it opens with a double-click.
ditto -c -k --keepParent "$APP" "$BUILD_DIR/notarize.zip"
xcrun notarytool submit "$BUILD_DIR/notarize.zip" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$APP"
spctl --assess --type execute --verbose "$APP"

# 3. The zip Sparkle updates from, and a disk image for first-time downloads, with the branded window
#    (scripts/dmg: art, icon positions, dmgbuild in a throwaway venv under the build folder).
mkdir -p "$BUILD_DIR/updates"
ZIP="$BUILD_DIR/updates/Glovebox-$VERSION.zip"
ditto -c -k --keepParent "$APP" "$ZIP"
DMG="$BUILD_DIR/Glovebox-$VERSION.dmg"
scripts/dmg/make-dmg.sh "$APP" "$DMG" "$BUILD_DIR/dmgbuild"
codesign --sign "$IDENTITY" --timestamp "$DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$PROFILE" --wait
xcrun stapler staple "$DMG"
# The same disk image under a fixed name, for the website's "latest" download link.
cp "$DMG" "$BUILD_DIR/Glovebox.dmg"

# 4. The appcast, signed with the EdDSA key from the keychain, with the release notes in it (the update window
#    shows them; a notes file named like the zip is how generate_appcast finds them).
cp "$NOTES" "$BUILD_DIR/updates/Glovebox-$VERSION.md"
"$(sparkle_bin)/generate_appcast" --download-url-prefix "$DOWNLOADS/" --embed-release-notes \
  --link "https://glovebox.vision" "$BUILD_DIR/updates"
APPCAST="$BUILD_DIR/updates/appcast.xml"
grep -q 'sparkle:edSignature' "$APPCAST" || { echo "The appcast isn't signed."; exit 1; }
grep -q '<description' "$APPCAST" || { echo "The appcast has no release notes."; exit 1; }

echo
echo "Ready: $(basename "$DMG"), Glovebox.dmg, $(basename "$ZIP"), appcast.xml"
echo "Attach these to the GitHub release v$VERSION of glovebox-vr/glovebox (same names; $DOWNLOADS/),"
echo "and mark it the latest release last, so no copy sees the new appcast before its files are there:"
echo "  $DMG"
echo "  $BUILD_DIR/Glovebox.dmg"
echo "  $ZIP"
echo "  $APPCAST"
echo "Existing copies see it within a day, or at once with Glovebox › Check for Updates."
