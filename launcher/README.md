# Alyx launcher for Vision Pro

A launcher in Steam's style for the Half-Life: Alyx port. The player signs in to Steam on the headset. Steam
confirms they own the game, the game files download straight from Steam's servers, and their Steam Cloud saves come
along. No Mac is needed.

- `Sources/SteamLauncherKit`: a Steam client written in Swift, with no SteamKit, .NET or other outside dependencies.
  It vendors LZMA (public domain), Zstandard (BSD) and zlib, and on Linux it uses OpenSSL for tests.
- `App/`: the visionOS SwiftUI launcher (`xcodegen` in `App/`, then build the `AlyxLauncher` scheme).
- `Sources/steamctl`: a command-line driver for testing against live Steam.
- `tools/ws_bridge.py`: a WebSocket bridge. It's only needed on Linux, where Foundation has no WebSocket client.

## What works, and how it was tested

| Piece | How | Tested |
|---|---|---|
| Connection | WebSocket to a Steam CM (`wss://<cm>/cmsocket/`), protobuf framing, gzip multi-messages, heartbeats. Port 443 CMs come first | Live, from a Linux cloud box |
| Sign-in | `Authentication` service: account name and password (RSA-encrypted with Steam's key; Password AutoFill works), then Steam Guard, usually one tap of Approve in the Steam app, or a code. Stores a refresh token in the Keychain and never stores the password. QR sign-in exists in the kit but isn't offered: Steam refuses its `s.team/q` links unless they are scanned inside the Steam app, and a headset can't show a QR code to a phone | Password sign-in worked on device (TestFlight build 16) |
| Ownership | License list → PICS package info → does a package grant app 546560? Steam also enforces this itself: for an unowned app it refuses both the depot key and the manifest code (confirmed live: `AccessDenied`) | Refusal confirmed live |
| Download | PICS app info → depot key → manifest request code → CDN manifest (zip, encrypted names) → chunks (AES-256, then LZMA or Zstd), each checked by Adler-32. Whole files are checked by SHA-1. 16 chunks in flight, resumable through a chunk journal | Live on a free depot (111 MB at 72 MB/s, every file verified), including a stop and resume |
| Alyx layout | Depots 546561 (content) and 546564 (Linux). The ARM64 ELF libraries become empty placeholders, because their signed translations ship in the app. x86-64 and Windows builds are skipped. Files land in `Documents/alyx/assets` (the depot's `game/`), which is where the app already reads them. Soname symlinks are created for real, unlike `devicectl` staging | Path and placeholder logic tested on the free depot. Alyx's own file list needs an owner's sign-in |
| Controllers | Play waits until both PS VR2 Sense controllers are on (GameController, `GCProductCategorySpatialController`) and lights up by itself when they are. The accessory-tracking permission (ARKit `.accessoryTracking`) is asked at Play, so the system's dialog appears over the launcher, not as the game starts. If it was turned off, the page says so and links to Settings. A host can turn the check off with `LauncherModel.requiresControllers` | Simulator screens only |
| Cloud saves | Automatic two-way sync like the Steam client's, using Alyx's own Auto-Cloud rule from Steam (all of `game/hlvr/save`, read live). It starts as the launcher opens (beside the game's build check) and shows its progress on the page. At Play the game waits only for that sync's downloads (its uploads carry on as the game starts), then one quick look at the cloud (a single listing) catches a save made elsewhere since; a full sync runs only if that look finds one, or if the launcher's sync didn't get through. Play waits as long as saves keep coming; if nothing moves for 10 s (Steam stopped answering) the game starts with the saves it has, and the sync stops before its next file (a cancelled Steam request now stops waiting at once). Uploads are on (`LauncherModel.cloudUploads`; off is download-only, and each sync then logs what would have gone up). The `.hl1`/`.hl2`/`.hl3` level files the game leaves behind never go up (`GameProfile.cloudNeverUpload`); `personal.cfg` (each slot's options, and the global one) syncs like any save, as on a PC. Saves from the last session go up at the next launch, because the game exits the app. Saves are kept where the game looks, in lowercase, whatever case the cloud's names use (some are `SAVE/S0`, from Windows). A headset save goes up only over the exact cloud version it was last in sync with, and that version is backed up first. If the cloud changed, or there's no history, the cloud's copy wins and the headset's is kept in `.cloud-backups/`. A slot started on the headset that shares its name with a different cloud slot moves to the next free slot number, so both games are kept. Files are written to a temp file, hash-checked, then renamed into place. Deletions are never propagated. Uploads use Steam's upload-batch API | 20 sync-logic tests (fake cloud), including mixed case, slot collisions, a reinstall, downloads before uploads, the quick look and a sync stopped midway; a test that a cancelled Steam request stops waiting. Real account, 2026-10-09: a new headset save went up byte for byte and nothing else in the cloud changed (`personal.cfg` stayed on the headset then; it syncs since 0.1.5) |

Alyx's public build info, read live: build 25487405, content manifest 1436897171613240821, Linux manifest
9198205754520568813. These are the same manifests as the depot on the Mac.

```bash
swift build && python3 tools/ws_bridge.py &            # the bridge only on Linux
.build/debug/steamctl info 546560                      # Alyx's depots, anonymously
.build/debug/steamctl download-anon 1007 1006 /tmp/x   # full download pipeline on a free depot
.build/debug/steamctl launcher-test /tmp/y --interrupt # install, stop, resume, install state
.build/debug/steamctl qr                               # sign in, then: owns 546560 / cloud 546560 <dir>
.build/debug/steamctl cloud-dryrun <copy>              # the app's save sync, offline, on a copy of Documents/alyx/assets
```

`cloud-dryrun` takes a copy of the headset's `Documents/alyx/assets` holding `hlvr/` (every spelling of `save/`)
and the launcher's sync history `.launcher-cloud.json`. With no `--listing <json>` the cloud is taken from that
history. It prints the report and the tree before and after, and changes only the copy. The copy must be on a
case-sensitive disk (the headset's is): on a Mac, a `Case-sensitive APFS` disk image.

## Screens

`-demo signin|approve|code|checking|notowned|install|downloading|ready|controllers|denied|preparing` shows fixed
screens for review. CI (`.github/workflows/launcher.yml`) builds for the visionOS simulator and saves a screenshot of
every screen, plus one live run showing the real sign-in screen.

`preparing` is the wait between Play and the game's first frame. The game app drives it: it sets
`LauncherModel.preparing` (progress 0 to 1, optional time left) inside the closure it passes to Play, and clears it
when the game shows.

## Not done yet

- **A first real-account run** of upload and download (see the table).
- **Test saves.** The dev headset's `hlvr/save` holds our test saves (e.g. `klbalcony.sav`). They would upload like
  any other save, so clear them before the first sync on that device.
- **Background download.** visionOS suspends an app whose window is closed. The download stops safely and resumes
  where it left off, but for now the window has to stay open.
- **Embedding in the real app.** This is a separate app today. The Klepton app would show `RootView` first and open
  the game's Full Space on Play. That integration goes through the "Boot Alyx engine via Klepton" thread.
- **Size.** Alyx's full install is about 60–70 GB (the Mac's Chapter 1 subset is 51.5 GB). A "Chapter 1 only" option
  is easy to add if we want one.

See `docs/LAUNCHER-TESTFLIGHT.md` for the distribution question.
