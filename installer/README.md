# Glovebox installer (Mac)

The Mac app that puts a game you own on Steam onto your Apple Vision Pro: it checks the Mac and
the headset, signs in to Steam, and then downloads, converts, builds and installs with the
player's own developer account. Half-Life: Alyx is the first recipe.

The app never builds anything itself. It runs the engine, `engine/glovebox.py` from
glovebox-vr/glovebox (`doctor`, `build`, `install`, with `--json`), and draws what it reports. The
engine's contract is that repo's `docs/INSTALLER.md`; [CLI-CONTRACT.md](CLI-CONTRACT.md) says how
the app reads it. Terminal users run the same engine, so both paths behave the same.

## Design rules

- Assume the player knows nothing. One step at a time, one thing to do per screen.
- Show, don't tell: a picture or a mock-up of the exact screen to use, a few words, one button.
- Automate everything that can be automated, and notice by itself when a manual step is done
  (checks re-run every 3 seconds while the player is busy elsewhere).
- The engine says what's wrong and what fixes it; the words and pictures the player sees live in
  the app (`FixCards.swift`, `InstallView.swift`). A problem the app doesn't know yet still gets
  the engine's own sentence and its fix.

## Layout

| Path | What |
|---|---|
| `Sources/GloveboxKit` | Reading the engine's output, the engine runner, setup order, install progress, Xcode's teams and profiles, diagnostics redaction. Builds and tests on Linux too. |
| `FakeEngine/glovebox` | A script that prints what the engine prints, for development, tests and screenshots. |
| `App/` | The SwiftUI app (XcodeGen: `App/project.yml`). |
| `scripts/release.sh` | Developer ID signing, notarization, the disk image and the Sparkle appcast, for a GitHub release. |
| `scripts/dmg/` | The disk image's window: background art (with its HTML source), icon positions, `make-dmg.sh` (dmgbuild, pinned). |

The app reuses `SteamLauncherKit` from `../launcher` for Steam sign-in, the ownership check and the
download, the same code as the headset launcher. The download is the app's own step: the files
in the engine package's `steam.mac_download` go to `<work>/linux/game`, where `build` looks.

The app bundles the Glovebox source (`GLOVEBOX_SRC` at build time) and copies it to
`~/Library/Application Support/Glovebox/src/<build>` on first use, since the engine writes into
its own tree. It runs it with Xcode's Python (`xcrun python3`, with `DEVELOPER_DIR` set to the
newest Xcode, so `xcode-select` never matters). Until Xcode works there is no Python, so the app
checks the Mac, the disk and Xcode (installed, new enough, license accepted) itself first.

## Running it

```sh
brew install xcodegen
cd installer/App && xcodegen && open Glovebox.xcodeproj
```

- `GLOVEBOX_SRC=/path/to/glovebox` (a checkout) when running xcodegen/xcodebuild bundles the real
  engine; at run time it makes the app use that checkout in place.
- Without it, Debug builds bundle the fake engine, and the download is simulated, so the whole
  flow runs. `FAKE_FAIL="team:3"` in the scheme's environment makes a check fail three times
  first (see the script's header for the other switches). `GLOVEBOX_ENGINE=/path/to/script`
  runs any other stand-in.
- `-demo <screen>` opens one screen with made-up state; `-snapshot out.png` also saves it and
  quits. CI does this for every screen (`.github/workflows/installer.yml`, artifact
  `installer-screenshots`).
- `swift test` in this folder runs GloveboxKit's tests against the fake engine.

## Updates

On this branch (VeRantyx Standalone) Sparkle is off: no feed, no key (`App/project.yml` says why).

Sparkle 2 checks `https://github.com/glovebox-vr/glovebox/releases/latest/download/appcast.xml` once a day: every
release attaches its own appcast, so the latest release's is always the newest. It installs an update
only when its EdDSA signature matches the public key built into the app and it's signed by the
same Developer ID. Updates stay off until `GLOVEBOX_SPARKLE_KEY` is set in `App/project.yml`
(`scripts/release.sh keys` prints it).

## Copy Diagnostics

Help › Copy Diagnostics, and every error card, put a report on the clipboard: versions, every
check's result with the engine's own words, the last error and the newest engine log's tail. Steam
sessions, account names, email addresses, team and device ids, people's names in device names
and the home folder are taken out first (`Redactor`, with tests).
