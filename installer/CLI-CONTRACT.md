# How the app reads the engine

The engine is `engine/glovebox.py` in glovebox-vr/glovebox. Its contract is that repo's
`docs/INSTALLER.md`. This file records how the Mac app uses it, so either side can change knowing
what the other relies on. The app's decoder is `Sources/GloveboxKit/Contract.swift`, and its tests
use the engine's exact lines.

## Running it

- **Command:** `xcrun python3 <source>/engine/glovebox.py doctor|build|install --json`, run from the
  source folder.
- **Source folder:** the app carries the source of its release tag, with that release's
  `glovebox-kit.zip` at its root. Before running anything it copies both to
  `~/Library/Application Support/Glovebox/source/<version>/`, once per app build. The copy is needed
  because the engine writes into `klepton/`, which must never happen inside the signed app. This
  follows the engine's docs/INSTALLER.md, "Bundling (beta)".
- **Environment:**
  - `GLOVEBOX_WORK`: `~/Library/Application Support/Glovebox/<package>`.
  - `GLOVEBOX_TEAM`: only when the player picked one of several teams.
  - `DEVELOPER_DIR`: the newest installed Xcode.
  - `GLOVEBOX_APP_PID`: the app's own pid. Once that process is gone, even after a crash, the
    engine stops its process group and exits, so nothing of Glovebox's keeps running headless.
  - The engine finds the kit by itself. `GLOVEBOX_KIT` passes through only when a developer sets it.
- **Cancel:** SIGINT to the engine's process group, so xcodebuild and devicectl stop too. Any exit
  after a cancel counts as cancelled.
- **Quit:** the app stops every engine run and every tool it started (SIGINT to each process group,
  SIGKILL after 3 s) before it exits. macOS 27 lists an app whose processes outlive it in Login
  Items & Extensions › Allow in the Background, and notifies the player.
- **Exit codes:** `doctor` exits 1 when something is missing. That isn't an error: the app reads
  the checks.

## Before the engine can run

Without a working Xcode there is no `xcrun python3`, so the app checks these itself first:

- **Mac:** Apple silicon.
- **Disk:** 15 GB free.
- **Xcode:** an Xcode of version 26 or later, found by bundle id, wherever it is.
- **Xcode's Python:** `xcrun python3` runs. If it doesn't, the license or first launch is
  pending. The fix is one password prompt: `xcodebuild -license accept && xcodebuild -runFirstLaunch`.

Once these pass, `doctor`'s results replace the app's own.

## Checks

The app shows five rows:

- This Mac: `mac` + `disk`
- Xcode: `xcode` + `visionos`
- Apple Developer: `team`
- Apple Vision Pro: `headset`, paired
- Developer Mode: `headset`, Developer Mode part

How the app turns check results into codes:

- **`headset`:** one check, read as two rows.
  - `guide: pair-headset` means not paired.
  - `guide: developer-mode` means paired, with Developer Mode off. The headset's name comes from
    `detail` ("Name: …" or "Name (visionOS …)").
  - `warn` means set up but asleep. Setup goes on, and the install waits for the headset.
- **Problem codes** come from the check id and its `fix`:
  - `xcode` with open-url "Get…" is `xcode-missing`; "Update…" is `xcode-too-old`.
  - `xcode` with open-app is `xcode-first-launch`: Xcode's license or its first-launch parts. Opening Xcode runs both,
    with Apple's own prompt naming Xcode. Glovebox never asks for the Mac's administrator password: the engine uses
    the Xcode the app picked (`DEVELOPER_DIR`) instead of changing the Mac's default (`xcode-select`).
  - `visionos` is `visionos-missing`.
  - `team` with open-url is `team-free-only`; with open-app it's `apple-account-missing`.
  - Any other check gets `<check>-missing` and a generic card: the engine's `detail` as the title
    and its `fix.label` as the button, run by `fix.kind`.
- **Running fixes:**
  - The app runs `command` fixes itself, once, with progress read from the percentages it prints
    (`xcodebuild -downloadPlatform visionOS`).
  - `open-app` for `xcode` opens the Xcode the app uses; the checks every few seconds see when its setup is done.
- **Teams:** read from Xcode's preferences, the same way the engine reads them. Paid teams come first, as
  in the engine: with a paid team, only paid teams count; without one, free Personal Teams do. With several
  the player picks one, and the app passes it as `GLOVEBOX_TEAM`.

## Install

| Stage the player sees | What runs |
|---|---|
| Download | The app, with SteamLauncherKit. It fetches the `steam.mac_download` prefixes from every depot in `steam.depots` (package.json) into `<work>/linux/`. |
| Convert | `build`'s steps `kit`, `runtime`, `game`, `pipelines`, `defaults`, `translate` |
| Build | `build`'s step `app` |
| Install | `install` |

- **Progress:** `progress` events move the bar. With `done`/`total` it's exact; otherwise each line
  creeps it forward. A `step` event completes that step.
- **Time left:** typical durations (Convert 90 s, Build 60 s, Install 40 s) until the engine
  reports its own.
- **Signing key:** before `build`, the app reads the team's key's access lists (never prompts). If codesign may
  not use it yet (a key the App Store Xcode app made), the app shows a card saying why macOS will ask. After
  Continue it signs one scratch file itself, so macOS asks once, with the guide under macOS's window pointing at
  Always Allow; it starts `build` only once a re-read shows Always Allow took.
- **Expiry:** the Home screen's "opens until" date comes from the built app's
  `embedded.mobileprovision`. Home shows Renew two days before a free team's install stops opening (7 days
  after it was built), and a month before a paid team's.
- **Before an install:** Install, Update, Renew and Try Again run `doctor` again and start `build` only once a
  run begun after the click finds every step done; otherwise the first step that isn't comes up. A way on from an
  install's error card into a step (Check Account) starts the install again by itself once the steps are done.
- **Trust:** visionOS starts a free team's apps only once their developer is trusted on the headset. At every launch
  the game writes `Documents/klepton-memprobe.txt` in its data container, which an update keeps. After `install`
  with a free team, the app copies that file (`devicectl device copy from`, as `collect` does; nothing is started
  on the headset). There: no trust steps. Not there ("Failed to retrieve the file node"): the trust steps
  (Settings › General › VPN & Device Management, then Trust). No answer: only a team's first install here gives them.
- **Errors** carry only a `message`, so the app picks the card by its words:
  - "unable to log in with account" or "login details for account" (checked first): `xcode-signin-expired`.
    Xcode's saved sign-in expired, while its account and team lists still say signed in, so `doctor` passes. Card
    "Sign in to Xcode again": its guide opens Xcode's Apple Accounts and points at the account; once Apple's
    sign-in has come and gone, the install starts again by itself (`doctor` again, then `build`).
  - "PLA Update available" or "Program License Agreement": `apple-agreement-pending`. Apple updated its developer
    agreement and the account hasn't accepted it. Card "Accept Apple's new terms": its button opens
    developer.apple.com/account; once Glovebox is back in front, the install starts again by itself.
  - "no paid Apple Developer" or step `team`: `signing-no-team`.
  - "did not build", "provision" or "signing": `signing-failed`.
  - "locked", "unable to connect", "not connected", "tunnel", "unavailable", "unexpectedly closed" or "no paired
    Vision Pro": `headset-unreachable`. The app waits for `doctor` to see the headset, then runs `install` again.
    (`install` itself first tries twice more, 5 s apart, when the headset drops the connection: "Connection with the
    remote side was unexpectedly closed", seen right after a headset falls asleep.)
  - "game files missing": `game-files-missing`.
  - Anything else: a generic card with Try Again and Copy Diagnostics.
- **Diagnostics:** an error carries the log of the run it came from (`EngineError.log`), and Copy Diagnostics shows
  that log's last lines, not the newest log (the checks write one every few seconds).

## Stutter reports

Decided 2026-10-05: once the player says yes, the headset uploads stutter reports by
itself. The engine's docs/INSTALLER.md, "Sharing stutter reports (beta)", is the contract. The
upload and its endpoint belong to the engine side.

- **The Mac asks** once, a few seconds into the install, in a standard alert (`ShareAsk.swift`).
  Home has a switch to change the answer.
- **The answer goes to `build`:**
  - `GLOVEBOX_SHARE_STUTTERS=1|0`;
  - `GLOVEBOX_SHARE_STUTTERS_AT=<unix time the player answered>`.

  The engine writes both into `alyx-env.txt`. If the player hasn't answered, neither variable is
  set.
- **On the headset**, the launcher reads those lines (launcher/App/AlyxLauncher/StutterSharing.swift).
  - If they're missing, it asks a few seconds into the game download, in a sheet with the same
    words.
  - An answer given on the headset goes to `Documents/klfz/report-consent.json` as
    `{"answer": true, "at": <unix time>}`.
  - The newest answer wins, so a change on the Mac reaches the headset with the next Update.
  - The account menu has the same switch.

## Would help, from the engine

None of these block the app:

- **`code` on `error` events**, so cards don't depend on message wording.
- **`total` on the `translate` and `app` progress lines.** Even a library count helps.
- **`install --device <id>`:** today the engine uses the first paired headset.
- **`build` on a never-paired Mac:** `headsets()` may list a discovered but unpaired headset.
  `check_headset` would then say "Developer Mode is off" instead of "not paired".
- **Headset name:** `KLEPTON_DISPLAY_NAME` is package.json's `app_name` ("Glovebox: HLA"), else
  its `game`. The app shows the same name on its Done screen.
