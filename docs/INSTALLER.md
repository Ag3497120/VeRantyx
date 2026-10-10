# The Glovebox app (Mac): design

The app is the only thing a player downloads. It turns "I own the game, a Mac and a Vision Pro" into the game on
the headset, and keeps it up to date. The same rules as the headset launcher apply:

- **No assumed knowledge.** Nobody needs to know what Xcode, a team, a profile or Developer Mode is.
- **One thing per screen.** Each screen has a picture or short looping clip, a title of a few words and one button.
  Never a paragraph.
- **Detect, don't ask.** Every screen watches for its own condition and moves on by itself when it is met. The
  player never says "done".
- **Do everything we can.** The player acts only where macOS, Apple or Steam require a person, and then the button
  opens exactly the right place.
- **Errors say what to do.** Never a log. The log is one click away for bug reports.

## Screens

| # | Screen | Moves on when | The player's action (only if needed) |
|---|---|---|---|
| 1 | Welcome: the headset clip, "Play Half-Life: Alyx on Vision Pro" | button | Start |
| 2 | Mac check | `doctor` mac + disk ok | Free up space (opens Storage settings) |
| 3 | Xcode | `doctor` xcode ok | Get Xcode (App Store page); "Finish setup" runs `xcodebuild -runFirstLaunch` with an admin prompt |
| 4 | visionOS support | `doctor` visionos ok | none: the app runs `xcodebuild -downloadPlatform visionOS` with a progress bar |
| 5 | Apple account | `doctor` team ok (any team in Xcode's settings; a paid one is used first) | Open Xcode Settings → Accounts (clip shows the two clicks). A free account works; its installs last 7 days |
| 6 | Steam | signed in and the game is owned | Account name + password, then "Approve on your phone" (Steam Guard) |
| 7 | Download | game binaries + shader packs present | none (progress bar; about 1.7 GB) |
| 8 | Vision Pro | `doctor` headset ok | Pair: clip of Settings → General → Remote Devices on the headset; Developer Mode: clip of Settings → Privacy & Security |
| 9 | Build | `build` done | none (progress through kit, runtime, pipelines, translate, app); about 2 minutes. With the App Store's Xcode, once before the first build: "macOS will ask for your Mac password once" (why), then Continue; macOS asks for the Mac password, and a guide under its window points at Always Allow. If Xcode's saved sign-in expired: "Sign in to Xcode again", then a guide to Xcode's Apple Accounts; once Apple's sign-in closes, the install starts again by itself. If Apple's developer agreement changed: "Accept Apple's new terms" opens Apple's website; back in Glovebox, the install starts again by itself |
| 10 | Install | `install` done | "Put on your headset" when the first launch needs the device unlocked |
| 11 | Done | — | "Open it on your headset". A free account's first install: trust it once on the headset first. The app stays in the menu bar for updates and renewals |

Screens 2–5 and 8 are the same `doctor` run, re-run every 2 s while the screen is up. Steps already satisfied are
skipped without being shown.

## The engine contract

The app runs `xcrun python3 engine/glovebox.py <command> --json` from the release's source folder and reads one
JSON object per line:

- `{"event": "check", "check": "xcode", "status": "ok|missing|warn", "detail": "...", "fix": {...}}`
  - `fix.kind`: `open-url`, `open-app`, `command` (the app runs it), `admin-command` (the app runs it with an
    authorization prompt) or `guide` (the app shows its own clip: `pair-headset`, `developer-mode`)
- `{"event": "progress", "step": "translate", "line": "...", "done": 3, "total": 17}`
- `{"event": "step", "step": "pipelines", "status": "ok", "detail": "..."}`
- `{"event": "error", "message": "..."}`, then exit code 1
- `{"event": "done", "step": "build", "ok": true, "app": "/path/KleptonAlyx.app"}`

Steam sign-in and the download happen in the app itself, with SteamLauncherKit (`launcher/`, also a macOS
library). The app puts the files at `GLOVEBOX_WORK/linux/game/` (default `~/Library/Application Support/Glovebox/alyx`),
filtered by `packages/alyx/package.json` → `steam.mac_download`.

## Bundling

- The app carries the source of its release tag (this repository without `.git`, about 12 MB) and that release's
  runtime kit (`glovebox-kit.zip` from the GitHub release, about 20 MB, all open source).
- Before running anything it copies both to `~/Library/Application Support/Glovebox/source/<version>/`. The kit goes
  in as `glovebox-kit.zip` at the source root. The build writes into the Klepton tree, which must never happen
  inside the signed app bundle.
- It runs `xcrun python3 engine/glovebox.py <command> --json` from that folder. The engine finds the kit there by
  itself. `GLOVEBOX_WORK` defaults to `~/Library/Application Support/Glovebox/alyx`.
- The Steam download writes the depot's files under `GLOVEBOX_WORK/linux/game/` (paths without the leading
  `game/`), only those matching `packages/alyx/package.json` → `steam.mac_download`. That's about 280 MB of
  binaries plus about 1.4 GB of shader packs.
- `glovebox build` takes about 90 seconds on an M5 Mac once the files are there. The Debian runtime downloads are
  cached in the work folder.

## Feedback

A "Send feedback" item in the menu bar runs `glovebox collect --json`. It copies the headset app's pipeline
recording, its hitch log (`slow.txt`: every pipeline the game compiled for 20 ms or more, with the time) and the game
console to `GLOVEBOX_WORK/feedback/<time>/`, and turns the recording into a recipe with no shader code. The recipe
and hitch log are what we merge into the next release's pipeline list. The console log is the bug report.

## Sharing stutter reports

The player is asked once whether the headset may send stutter reports by itself (`reports/`; no shader code, no
logs, nothing personal).

- The Mac app asks while the install runs and passes the answer to `glovebox build` as
  `GLOVEBOX_SHARE_STUTTERS=1` (share) or `0` (don't), with `GLOVEBOX_SHARE_STUTTERS_AT=<unix time of the answer>`.
  Unset means not answered yet.
- The engine writes both lines into the app's `alyx-env.txt` (the build time stands in for a missing `_AT`), so the
  build carries the answer and when it was given.
- With no line there, the headset launcher asks while the game downloads. An answer given on the headset is kept in
  Documents with its own time, and the newest answer wins: a change on the Mac reaches the headset with the next
  Update.
- With a yes, the headset app uploads what's new once each time it opens. The server also counts each such headset
  once a day, by its anonymous install id, Glovebox version and headset model (`reports/`).

## Updates

- **The app itself:** Sparkle 2. The appcast and the signed zips live on GitHub Releases. Each update is checked
  against our EdDSA key and Apple's code signature. The app is Developer ID–signed and notarized.
- **The game build:** each release names a source tag and a runtime kit. When either changes, or Steam has a new
  game build, Home says "A new version of Glovebox: HLA is ready" (the headset app's name). Install Update rebuilds
  and reinstalls. The headset keeps its saves and its
  downloaded game data.

## Later: more packages

`packages/<id>/package.json` describes a game: where its files come from, its settings, its pipeline recipe and what
it needs. The screens above don't change. Steam games use the Steam screens. Other sources (for example a Quest game
the player owns, run through Klepton's Android support) get their own source screen.
