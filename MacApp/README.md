# VeRantyx for Mac (the app)

A Mac app for players who would rather not use Terminal: it sets up the vxrt version and starts the games from a
window. It carries the runtime built (VXRuntime, the encoder, the SteamVR driver and capture proxy, ALVR's streaming
core, the launch scripts), so the player's Mac needs only macOS and Steam: no Homebrew, Xcode, Python or Rust.

- **Get started**: checks the Mac (Apple silicon, macOS 14.2 or later, Rosetta 2, with a button that has macOS install
  it), Steam for Mac (installed, running), where the games go (any folder, an external SSD too), then gets SteamVR and
  the games' Windows builds: downloaded with the player's own Steam account through Valve's steamcmd, or copied from a
  Steam library they have (a Windows PC's drive, a CrossOver or Whisky bottle; an APFS clone on the same disk). Then it
  gathers the Microsoft runtime DLLs from the installers those installs carry (`Sources/Redist.swift`, the same choice
  of files as `tools/vxrt_redist.py`).
- **Play**: the installed games, with what is known about each (games/*.json), a picture setting, and the session:
  SteamVR up, the headset connected, the frame rate, the game's log, Stop. With ALVR, the frame rate the headset
  shows beside the Mac's, and the latency, from the headset's own statistics (ALVRBridge's `VeRantyx: headset:` line
  in session_log.txt), with advice when the network holds the stream up (fewer than three quarters of the Mac's
  frames shown, over 40 ms on the network, or video lost); the "it ran" report carries them too.
- **Each game's way to the headset**: a switch on every game card, next to its Play/Headset button: the ALVR app
  (Apple Vision Pro's from the App Store, or a Quest's or Pico's, as the Headset page says) or the VeRantyx app on an
  Apple Vision Pro. Kept per game (the setting `headsetRoutes`); a game with none chosen follows the Headset page. The
  ALVR app starts the game's script with `VERANTYX_ALVR=1`, the VeRantyx app with `VERANTYX_ALVR=0` and the quality
  preset (`VXRT_VR_QUALITY`); the card shows the headset and picture setting the game will use.
- **No Man's Sky** (and other games wrapped in Steam's DRM): started by the player's own Windows Steam on VXRuntime
  (`~/Verantyx_VR_Drive/vxrt_wsteam`), through `scripts/launch_vxrt_nms_vr.sh` (`VXRT_FLAT=1` on the Mac's screen) with
  `VXRT_WSTEAM_NO_START=1`. The Play page's Windows Steam card opens that Steam (`scripts/launch_vxrt_windows_steam.sh`);
  the player signs in in its window and answers its windows (an update, the game's agreement, "Steam Service Error")
  themselves: the app never signs in, accepts anything or presses a button in Steam for them. Stop ends the game and
  SteamVR; Windows Steam stays open.
- **Get and play**: the Play page lists the known games not installed yet, each with "Get and play" (on this Mac,
  in the headset). The flow: what is missing (the game; SteamVR for the headset), its size and the disk's free
  space, a yes from the player; then the download the game's own way (`Title.source`): Valve's steamcmd with the
  player's account (they type the password and Steam Guard code into steamcmd's console themselves), or, for a game
  Windows Steam starts, Windows Steam itself (`scripts/vxrt_windows_steam_ask.sh -- steam://install/<id>`: Steam shows
  its own install window, answered by the player, and installs only what the account owns; progress from its app
  manifest); then the runtime files if none are gathered yet, and the start. Windows Steam has to be running, opened
  and signed in to by the player (the flow waits, with Continue); installing Windows Steam itself is not done by the
  app. `VeRantyx --get <id> [--flat] --dry-run` runs the flow answered yes with the downloads and the start only
  printed.
- **Headset**: Meta Quest or Pico through ALVR (Wi-Fi or a USB cable), Apple Vision Pro with the VeRantyx app, or with
  the ALVR app; the steps for each.
- **Help**: the logs folder, a report form on GitHub filled in with the Mac's facts, Stop everything.

It uses the same places as the scripts (`~/Verantyx_VR_Drive/vxrt_steam`, `vxrt_redist`, the overlay and the root;
another folder can be chosen) and starts the games with the same scripts (`scripts/launch_vxrt_alyx.sh`,
`launch_vxrt_game.sh`), from its own copy, with `VXRT_NO_WINE=1` and its logs in `~/Library/Logs/VeRantyx`. A setup made
with either works with the other.

steamcmd runs in a pseudo-terminal shown in a console in the app (`Sources/SteamCmd.swift`). When it asks for the
password or a Steam Guard code, the player types it into the console's field and it is written to steamcmd as typed;
the app keeps it nowhere (not in a file, its settings or the console's text: steamcmd does not echo it). steamcmd
remembers the sign-in itself. Only the account name is kept (in the app's settings). Network: steamcmd from Valve
(`steamcdn-a.akamaihd.net`, the first time; Homebrew's is used if installed), the games from Steam, and with a USB
cable Google's Android platform tools, which ALVR downloads once when there is no adb.

## Languages

English, Japanese, Korean, Simplified and Traditional Chinese, Spanish, French, German, Portuguese (Brazil), Russian
and Italian. The app speaks the first of the Mac's preferred languages it has (English when none), or the one chosen
in Help > About (the setting `language`; it applies when the app starts again, which the picker offers to do).

Every text is written as `T("English", "日本語")` at its call site (`TF("Copying %@…", "%@ をコピー中…", name)` when it
holds values: the English stays constant). The other languages are tables, `Localizations/<code>.json`, from the
exact English text to its translation (`%@` as in the English; `%1$@`, `%2$@` to reorder); `build_app.sh` copies them
into `Contents/Resources/Localizations`. `python3 MacApp/check_localizations.py` lists the texts a table lacks (and
the ones no longer used, and placeholders that differ): run it after changing a text. To add a language: a new
`Localizations/<code>.json` with every English text, its code and its own name in `Language.all`
(`Sources/Base.swift`), and the code in `CFBundleLocalizations` (`Info.plist`).

## Build

```bash
MacApp/build_app.sh                    # build/app/VeRantyx.app and build/app/VeRantyx-<version>.dmg, signed ad hoc
```

Needs what the repository's own setup builds with (Xcode or its command line tools, mingw-w64, Rust). The version is
`MacApp/VERSION`. Signed ad hoc, the app runs on the Mac that built it; another Mac asks once to allow it (System
Settings > Privacy & Security > Open Anyway).

To hand it out without that step, sign it with a Developer ID and have Apple notarize it (the publisher, once):

1. Xcode > Settings > Accounts > the team > Manage Certificates > + > **Developer ID Application** (needs the paid
   Apple Developer Program).
2. `xcrun notarytool store-credentials verantyx-notary --apple-id <Apple ID> --team-id <team>` (asks for an
   app-specific password, made at appleid.apple.com; kept in the keychain).
3. `VERANTYX_SIGN_IDENTITY="Developer ID Application: <name> (<team>)" VERANTYX_NOTARY_PROFILE=verantyx-notary MacApp/build_app.sh`

With a Developer ID everything is signed with the hardened runtime: VXRuntime with `entitlements/vxrt.plist` (it maps the
Windows programs' code itself, and loads Steam for Mac's `steamclient.dylib`, which Valve signs), the encoder with
`entitlements/encoder.plist` (it loads the ALVR bridge). `VERANTYX_HARDENED=1` tries the same with ad-hoc signing.

The licenses folder in the app holds VeRantyx's license, THIRD_PARTY_NOTICES.md, ALVR's and OpenVR's licenses, and
the license and notice texts of the Rust crates in the ALVR bridge (`crate_licenses.py`).

## Testing without the window

```bash
VeRantyx.app/Contents/MacOS/VeRantyx --collect-redist          # gathers the runtime DLLs (VXRT_REDIST=<folder>: elsewhere)
VeRantyx.app/Contents/MacOS/VeRantyx --play 546560 120         # starts a game as Play does, prints what the app sees
VeRantyx.app/Contents/MacOS/VeRantyx --snapshot <folder>       # a PNG of each page
VeRantyx.app/Contents/MacOS/VeRantyx -language ko --snapshot <folder>   # the same in another language
```

## Checked (2026-10-18)

- The runtime DLLs: `--collect-redist` and `tools/vxrt_redist.py collect` on the same libraries chose the same 136
  DLLs, every one with the same version and SHA-256.
- Half-Life: Alyx started from the app's own copy of the runtime on the MacBook Air M5 (`--play 546560`, the Quest
  setting, no headset): SteamVR up, the web UI stand-in (now `scripts/vxrt_helper.swift`) told the game its page was
  ready, the game loaded its menu (map "startup"), the encoder took its frames (30-40 a second with no headset) with
  ALVR's streaming core and game audio, Stop ended everything. The same with the hardened runtime (ad-hoc,
  `VERANTYX_HARDENED=1`): VXRuntime ran the game, its Steam bridge loaded Steam for Mac's client (`SteamAPI_Init`
  OK), the encoder loaded the ALVR bridge.
- Copying from a Steam library (a library made for the test): the app's folder and its manifest cloned into
  vxrt_steam, its executable found, anti-cheat files noticed, a second copy refused.
- The steamcmd console with a stand-in program: lines written over themselves, steamcmd's progress lines (87 %,
  downloading), a line typed into it, its exit status.
- The pages (`--snapshot`), in Japanese.
- Signed with a Developer ID and notarized by Apple (release mac-v0.1.0); Alyx started from that signed bundle on the
  MacBook Air M5 (`SteamAPI_Init` OK, map "startup").

Not checked yet: a real steamcmd sign-in and download through the app (it needs the player's own account), a Quest or
Pico, copying from another disk.
