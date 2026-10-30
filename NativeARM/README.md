# NativeARM — Valve's Linux ARM64 Half-Life: Alyx on macOS, no Wine

Started 2026-10-11. The goal is to run the ARM64 Linux build that Valve ships
for the Steam Frame directly on Apple Silicon — no Rosetta, no Direct3D
translation, no VM — with a small OpenVR runtime of our own feeding the
existing VeRantyx streaming pipeline and ALVR controllers / hand input, so the game
can run faster than it does under Wine and still needs no tracked VR
controllers.

## What is known (spike of 2026-10-11)

- Steam depot **546564** ("linux") of app 546560 contains both
  `game/bin/linuxsteamrt64` (x86-64) and **`game/bin/linuxsteamrtarm64`** plus
  `game/hlvr/bin/linuxsteamrtarm64`: 86 ARM64 ELF files, 260 MB. The content
  depot (546561, 72 GB) is shared with the Windows install.
  `scripts/fetch_alyx_arm64.sh` downloads it with your own Steam account.
- `hlvr` (26 KB) only dlopens `libengine2.so` and calls `Source2Main`; our
  launcher does the same.
- The binaries are glibc (symbol versions 2.17–2.38) + libstdc++ (GLIBCXX up
  to 3.4.32). Relocations: RELATIVE / ABS64 / JUMP_SLOT / GLOB_DAT, plus 63
  `R_AARCH64_TLSDESC` and 3 `TLS_TPREL64` (mimalloc). No IRELATIVE, no RELR.
- [Klepton](https://github.com/shinyquagsire23/Klepton) (MIT) translates
  Android ARM64 ELF `.so` files into signed Mach-O dylibs (`klepton-ld`) and
  supplies a bionic libc / NDK / Vulkan-to-MoltenVK runtime on macOS and
  visionOS. Its relinker handles these glibc binaries as they are:
  `libtier0`, `libengine2`, `libsteam_api` translate cleanly (TLS and x18
  rewrites applied). Its loader covers the four relocation types above;
  TLSDESC/TPREL support is the one loader addition needed.
- System libraries the game expects from the Steam Linux Runtime (no public
  ARM64 depot exists) come from Debian trixie arm64 packages
  (`scripts/fetch_debian_libs.sh`): libstdc++6, glib, png, ssl, udev, …
  They are translated the same way and become guest libraries.
- After the game's own libraries and the Debian ones resolve each other,
  what remains for the host shim is (`tools/census.py`): ~800 glibc
  functions (incl. `__*_chk` fortify variants, `__xstat`/`stat64` family,
  `__isoc23_*`, locale `_l` variants, `__errno_location`, `__ctype_b_loc`,
  `__cxa_thread_atexit_impl`), 113 X11/xcb and 28 Wayland symbols (to stub —
  the game runs with `-nowindow`), and the Adreno Vulkan driver
  (`libvulkan_freedreno_hlvr.so`), which is not loaded at all: Vulkan goes to
  MoltenVK through Klepton's `kl_vulkan`.
- `libopenvr_api.so` is replaced by our own runtime; `libsteam_api.so` talks
  to a Steam client that will not exist here — how to satisfy it is an open
  question (the user owns the game; nothing of Valve's is redistributed).

## Status (2026-10-11, end of day 1)

Working: every library of the depot loads with 0 unresolved imports (glibc
layer in `host/kl_glibc.c`, patched Klepton in `klepton-patches/`); the engine
initialises through tier0, file system, SDL (dummy video + Vulkan surface from
the shim), **our OpenVR runtime** (`vrruntime/`: IVRClientCore/System/
Compositor/Input/Chaperone/Overlay/... built on the SteamVR 2.15.6 headers, plus
the Valve-internal IVRMailbox_001 layout from Proton), Vulkan on MoltenVK with a
device-feature clamp, the sound system (System V semaphores needed an ABI-safe
`semctl`), and renders its first frames to the synthetic swapchain.

Steam: `CNetworkSystem::Init()` requires a working `SteamAPI_Init`. The Linux
steam_api finds `~/.steam/steam.pid` and dlopens `~/.steam/sdkarm64/steamclient.so`;
`host/kl_steambridge.c` answers that with the macOS Steam client's arm64
`steamclient.dylib` (same C++ ABI, talks to the macOS Steam process). Needs
**macOS Steam.app running and signed in** by the owner of the game.

## Status (2026-10-11, day 1 evening): main menu renders in stereo

With Steam signed in, the engine loads `maps/startup`, starts the server and
client game systems, attaches both (emulated Rift S) controllers through our
runtime, brings up Panorama and the main menu, and streams stereo frames
(1632x1056 per eye, ~30 eye pairs/s on an M1 Max with the synchronous readback)
into the shared frame file — verified by dumping a frame. It ran 5 minutes
without a fault. Fixed on the way:

- Steam's own Breakpad (`Breakpad_SteamMiniDumpInit` in the macOS
  steamclient) took the Mach exception port, so every fault became a minidump
  and a silent `_exit(1)`; the bridge answers `Breakpad_*` with no-ops.
- Klepton's image table held 64 images; Alyx loads ~110, so dlsym on later
  images (libclient's `CreateInterface`) missed — raised to 1024.
- libvscript's LuaJIT arena insisted on the low 2 GB (`Plat_VirtualAlloc` with
  a max address); arm64 macOS cannot map below 4 GB at all (a smaller
  `__PAGEZERO` is SIGKILLed at exec), so `host/kl_luafix.c` drops the limit,
  nops the hook's range check and commits the arena RW instead of RWX.
- libvideo's 1703 bare `svc #0` (debug breaks) would run Darwin syscall x16;
  they become nops at load.
- glibc `locale_t` is read directly (`->__ctype_b` at 0x68 by libc++); the
  guest now gets glibc's struct layout wrapping the Darwin locale.
- 4 KB commits inside reservations (Panorama) failed on 16 KB pages; widened.

Later the same day:

- Right eye: Source 2's ARM (Deckard) path sizes the eye texture for the union
  of both frusta but draws every eye with the left eye's range at the left of
  it. The runtime now gives both eyes the union frustum and cuts each eye's
  own range back out (`VERANTYX_SHARED_FRUSTUM=0` to disable). Both eyes full,
  1298x1056 each, right eye correct.
- Readback is asynchronous (two slots, worker thread; `VERANTYX_SYNC_READBACK=1`
  for the old path): the game thread went from 26 ms to 4.5 ms per frame.
  Menu: steady 60 fps at the 60 Hz virtual display; ~69 fps at
  `VERANTYX_DISPLAY_HZ=90` (Wine path on the same Mac: ~30 fps).
- `blr x18` veneer no longer uses the real x18 (kernel race crashed libmpg123).
- `scripts/launch_native_vr.sh` runs MacEncoder + the game; the encoder takes
  `VERANTYX_SHARED_FRAME`. Encoder verified at the game's rate (no headset).

Open: playing with the headset (poses, Joy-Con/hand input through the
bindings) not yet tried; "Attempting to purge m_fencedResourceList" warnings;
in-level performance unmeasured.

Run: `build/host/alyx_host --libdir build/guest/lib --game ~/Verantyx_VR_Drive/alyx_linux -- -vr -steam -noasserts -nopassiveasserts +map startup -vulkan -nowindow`
(`VERANTYX_BENCH=1` synthesises poses without a headset; `VERANTYX_SHARED_FRAME`
must name the frame file the Mac encoder reads; the native runtime never falls
back to a Wine prefix. Prefer `scripts/launch_native_vr.sh` for a session;
`--probe lib.so` reports imports).

## Status (2026-10-27): native Alyx in the Mac app (offline checks only)

Build `MacApp/build_app.sh`, then choose **Get started → Game launch mode →
Native ARM64**. This is one application preference: supported titles use native;
other titles retain Windows and display that runtime explicitly. The catalogue
(`Title.nativeSupport`) declares native adapters; shared setup and game cards do
not select runtimes by Alyx's app ID. Both installations are kept when switching.
Native has its own installation record and readiness; it requires ALVR and
signed-in macOS Steam, without Windows SteamVR or Microsoft runtime DLLs. Pico 4 Ultra
uses the existing **Meta Quest or Pico (ALVR)** headset choice.

The app installs licensed depots **546561** (content) and **546564** (Linux
ARM64) through its private SteamCMD on the selected drive. SteamCMD asks for
credentials and Steam Guard in the app's existing console. Public manifest IDs
come from Steam and are checked against the completed Linux installation.

Existing Windows game files seed an independent working copy under
`native_alyx/downloads/alyx-content/`. On APFS this uses copy-on-write clones,
not hard links. An old Windows manifest no longer triggers a full-game space
requirement: SteamCMD's Linux `app_update 546560 -beta public validate` checks
existing files and requests missing or changed content. A completion marker
and missing-file-only seeding preserve Steam-updated files on retry. Steam's
transient downloading/temp/workshop directories are not copied. Windows files
are never modified. Symbolic links inside the Windows installation are omitted
from the seed, including cyclic, broken and external links; Steam validation
supplies any missing depot files. The final native version is prepared from the
validated working copy; the working copy stays available for future updates.

Download size and additional disk requirements depend on the actual changes.
SteamPipe uses deltas, but large pack-file changes can still require substantial
downloads and staging space ([Valve's SteamPipe documentation](https://partner.steamgames.com/doc/sdk/uploading)).
The app reserves space for native components and preparation, and lets Steam
validate the actual update. On disks without file cloning, independent working
and prepared copies need additional space. No real Steam download was performed
for this change; actual transfer size remains to be checked by the user.

The ARM64 support packages in `dependencies.json` are versioned and SHA-256
checked before extraction. Their copyright notices stay with the download cache.

Installations live in `<selected drive>/native_alyx/`. A completed version is
published by atomically writing `installation.json` after validating ARM64 ELF
headers, required game content and guest-library aliases. Cancellation keeps
resumable downloads and never marks a partial installation ready. Settings,
saves and shader caches live in `state/`, outside the replaceable version.
Source depots and the Windows installation are never written through symlinks.
Old completed versions are retained; automatic storage reclamation is not
implemented. Choose a drive with room for both downloads and preparation.

The app carries a compiled `native-session`, `alyx_host`, MoltenVK, the encoder
and the ALVR bridge. Playing does not build or fetch runtime components and
requires no Python, Rust, Homebrew or Terminal commands. Klepton and MoltenVK
are pinned build dependencies, fetched under `NativeARM/build/` by the app
build. Local builds use ad-hoc signing, including the host's loader entitlements;
no Apple Developer account, Developer ID or notarization is needed on this Mac.
The existing Windows driver / proxy / ALVR may be reused from an installed app
when MinGW is missing, only after Git proves those components' sources match.

The helper creates a fresh frame map and private OpenVR / Linux Steam service
paths. It waits for advancing ALVR poses, finite frusta for both eyes and IPD,
then starts the native engine. Encoder exit 75 restarts only that encoder.
Stop, errors and loss of the parent app request normal game exit through
OpenVR's `VREvent_Quit`. Delivery and `AcknowledgeQuit_Exiting` are recorded in
`session.json`. The helper waits 60 seconds before showing a slow-quit notice,
then continues waiting with its session lock, frame map and encoder intact.
Repeated Stop / SIGINT / SIGTERM do not escalate. **Force stop** is a separate
explicit action, offered after the timeout; developer session configurations
can set `quitWaitSeconds` and opt in with `allowForceKill: true`. Missing fields
retain the safe defaults. The app also waits for native game exit when closing,
returning to its window if the quit times out. Only after game exit does the
helper terminate and reap its encoder; there are no process-name kills in the
native supervisor. An existing VR
session blocks Play. The host still uses the real Mac Steam client and licence
checks. The game's menu, audio, controllers and reconnect behaviour on a real
Pico remain to be checked by the user.

Picture quality is a single shared preference in Play. Each runtime maps
Recommended / Sharper to its own rendering and stream settings. Legacy per-Alyx
mode and native quality preferences migrate to the global mode and shared
quality on first app start; both installation records and saves are preserved.
Setup readiness and step counts follow the chosen runtime. Windows Rosetta
preparation is shown with the launch-mode choice; native setup does not require
it (the current Mac SteamCMD has an arm64 slice).

The earlier static fixes include independent controller actions and releases,
source restrictions and set priority, live IPD and haptics, per-eye fenced
readback, stereo pairing within a frame, BGRA conversion, timeout quarantine
and draining on cleanup. The encoder refuses a requested but unavailable ALVR
bridge and keeps legacy input from overwriting ALVR tracking.

### Build and offline verification

```bash
MacApp/build_app.sh
make -C NativeARM check check-app check-syntax check-encoder
make -C NativeARM check-sanitize
```

The C++ suite uses fake Vulkan and synthetic shared memory. Swift checks cover
ELF / SONAME parsing, source isolation and persistent settings, Steam metadata
and download paths, PTY output beyond the visible console buffer, synthetic
Debian extraction, separate app readiness / mode / quality, frame-rate math,
and fake-process session restart, timeout, cancellation, cooperative/slow quit,
parent loss, encoder failure, explicit force and owned cleanup. Python checks
exercise probe cancellation, collection errors, immutable evidence and the
timeout wrapper using synthetic hosts only.
No test starts Steam, Alyx, an ALVR server or the real encoder. The host and
encoder are compiled and linked, and nested ad-hoc signatures are verified.
The Windows shader-progress, loading/HEVC loopback, encoder-supervisor,
controller-hotplug, D3D11 drawing and compute checks also pass. The full
`make -C VXRuntime check` currently stops at the translator differential test;
a separate build of that test from `main` reproduces the same failures.
These offline checks do not establish a game or headset regression run.

### User check through the app

1. Open the built `build/app/VeRantyx.app`. Sign in to your own Mac Steam.
2. Choose **Meta Quest or Pico (ALVR)** on the Headset page and install/open the
   matching ALVR client on Pico 4 Ultra, following that page's instructions.
3. In **Get started**, select **Native ARM64** in **Game launch mode**, select
   Alyx in **Games**, and press **Download**. Answer SteamCMD's own sign-in /
   Guard prompts in the app. Setup prepares files without starting a game.
   Native repair is available on the game's setup row. Selecting Windows games
   additionally prepares their Windows SteamVR / runtime dependencies.
4. In **Play**, choose picture quality once in **Settings for this session** and
   press Play on Alyx. Open ALVR on the Pico and wear it; the app waits up to
   five minutes for live eye geometry and tracking.
5. Check picture in both eyes, head movement, hands, buttons, grabbing, throwing,
   reloading, locomotion, sound, saves, level changes and reconnection. Stop in
   the app. A new headset model needs a new game session.

### FPS capture and comparison

The session card shows Mac stereo-frame rate separately from ALVR's headset
frame rate and latency. Native Mac FPS comes from differences between published
stereo-pair counters, not the cumulative frame count. **Compare Alyx FPS** records
60 seconds when requested. Enter the same scene/save name for both modes; the
card displays each mode's latest capture for that name. Captures contain the
mode, quality, route, duration, samples and app build, and are saved under
`~/Library/Logs/VeRantyx/fps-comparisons/`. Help's log export includes these files
and recent native session logs with personal paths redacted.

Use identical scene, view, render resolution, display rate, MSAA and stream
budget, warm shader caches, and alternate native / VXRT / VXRT / native. Mac
counters measure different pipeline stages (native published pairs, VXRuntime
encoder capture); headset FPS is their common output. Capture labels do not
prove equivalent settings. No performance result or playability is claimed by
these offline checks; the old menu measurements above remain historical.

## Status (2026-10-27, Pico 4 Ultra): loading pause diagnostics

The user's native session `0E9CB32E-60FB-4A51-94C2-C953794685A5`
reported a grey picture and pause sounds after connecting. Its engine log
contains `show_message` / `Press Right Hand To Start`, followed by `paused the
game`, while eye-pair counters and ALVR frame reception continue. This is log
evidence from the user's session, not a harness run or a successful playtest.

The native mailbox initially discarded that loading-page text. It now writes
the message atomically to a private `game-prompt.txt` for diagnosis. The native
supervisor keeps this path out of the encoder's environment, including after
restarts, as described under [no native trigger overlay](#status-2026-10-27-follow-up-no-native-trigger-overlay).
New loads, `end_loading`, mailbox shutdown and session cleanup clear the file.

The runtime also now sends OpenVR's binding-load result and bindings-updated
events after parsing actual action bindings. The previous implementation
loaded them without notifying the game. Whether that missing notification was
preventing this session's input from activating remains to be checked in Alyx.

Diagnostics under `~/Library/Logs/VeRantyx/native/<session>/` now distinguish:

- `hardware-encoder.log`: raw ALVR trigger values, accumulated trigger presses,
  controller buttons and tracking, at most one changed-input line per second.
- `alyx_host.log`: binding events delivered to the game, action/source handle
  lookups, action-update errors and API read counts. Raw inputs and active sets
  are sampled every five seconds even if action updates stop or fail.
- `alyx_host.log`: per-eye sampled brightness range, variation and pixel hash,
  alongside existing frame counters/timings, to distinguish a flat submitted
  image from missing video. No screenshots are saved.

Offline coverage includes successful/failed binding notifications through
`PollNextEvent`, loading-message parsing, diagnostic prompt publication and
cleanup, per-eye pixel statistics and prompt isolation from the encoder.
The synthetic runtime, supervisor and app tests pass, including ASan/UBSan/TSan.
No Steam, Alyx or headset run was started for this change. Menu entry and
subsequent level transitions require the user's playtest.

## Status (2026-10-27, follow-up): mixed gamepad and hand input

The user's next session, `9AE23E18-3FE0-48C9-B1B4-766C11F9C829`, shows
the trigger reaching both ALVR and native shared memory. Binding events are
delivered and the game resolves its action handles, but `UpdateActionState`
fails repeatedly with `restrictedDevice=0x103`. That handle was returned for
`/user/gamepad`. The old implementation accepted restrictions only for the two
hands, rejecting Alyx's mixed input batch and leaving hand actions inactive.

Restrictions now accept registered input paths with no local bindings. Such a
source remains inactive, while the other sets in the batch update normally;
a secondary set applies to all other devices, including both hands when the
primary restriction is the gamepad. Invalid handles still fail validation,
before changing the previous snapshot. Reads restricted to a known but unbound
source return inactive digital, analog and pose data without leaking hand input.

A synthetic regression reproduced the same error before the fix and passes
after it. It covers gamepad-first/gamepad-last batches, actual shared-memory
trigger press/release, secondary sets, unbound source reads and invalid handles.
The native checks, syntax checks and ASan/UBSan/TSan pass. The app was rebuilt;
the effect on Alyx's menu still needs the user's run.

This session remains paused and its sampled eye images are black before the
prompt is drawn. It cannot establish native gameplay performance. Native ARM64
removes x86 translation, but the Linux compatibility layer, Vulkan-to-Metal path
and frame readback still have costs. If low frame rate remains after menu entry,
capture a CPU sample of that live host and compare matching scenes/settings;
the existing logs alone do not identify the long interval between frames.

## Status (2026-10-27, follow-up): presets, audio and connection state

The user reports menu entry and normal frame rate after the mixed-input fix.
Session `4761641F-E926-4FA5-AA66-37AD62EC5A37` then exposed three issues:
audio stayed on the Mac, the app kept waiting for the headset despite video,
and applying an in-game graphics preset stopped new frames. Logs and a read-only
CPU sample show the engine continuing to run with repeated
`VRCompositorError_InvalidBounds`, rather than a stopped game process.

- The app now reads log modification times directly from the filesystem.
  `URL.resourceValues` cached the first timestamp on the reused ALVR log URL,
  making a live connection look stale. Tests cover this cache case and
  connect/disconnect/reconnect parsing.
- ALVR audio aggregates now have a unique UID and selection name for each
  encoder process. A forced reconnect previously failed to recreate the fixed
  UID and disabled game audio. The legacy UDP capture is no longer started in
  ALVR mode; failed aggregate creation also releases its new tap. Audio settings
  are archived at start/restart/stop as `alvr-*-audio.json` alongside the logs.
- The native launcher now fixes Source 2 fidelity with `-noautofidelity
  +vr_fidelity_level 3`, as the standard Windows launcher does. The app's eye
  budget remains the quality control. The compositor tolerates only subpixel
  edge roundoff, still rejects genuinely invalid rectangles, and logs the exact
  image size, bounds and submit flags on rejection. Synthetic checks cover
  resizing, edge roundoff, invalid bounds and recovery.

Audio output and live graphics-preset changes still need a user retest. A new
build may be put beside an open app with `VERANTYX_APP_BUILD_DIR`; this work
does not stop or replace the user's running app. No game or headset was started
by the agent for these changes.

## Status (2026-10-27, follow-up): gameplay and intermittent pose reads

The user reports playing native Alyx on Pico 4 Ultra with good frame rate,
but occasional abrupt picture shifts and dropped held objects. This report
does not change the harness baselines or the profile's native playability level.

In session `85F7DE13-9D9F-48F4-B98F-FC44C0D76497`, ALVR's frame log occasionally
has pose-stamp ages of roughly 93 million milliseconds while tracking packets
continue arriving. A read-only, 12-second probe of that session's shared frame
file observed 8 published frames with a zero pose timestamp out of 865 frames.
These are diagnostics from the user's run, not a harness performance result.

The native pose reader retries a shared-memory snapshot 64 times. If the encoder
is interrupted while writing, all retries can fail; the old sampling path then
used an empty packet, lost the ALVR pose timestamp and reported stale hand input.
That also clears the action binding's held-grip state. A synthetic regression
reproduced this failure before the fix.

Pose sampling now retains the previous coherent packet across an interrupted
write. Its original timestamp is preserved, and reusing it does not refresh the
existing 500 ms input timeout. Coherent release/untracked/invalid packets still
take effect immediately. Frustum formatting and file writes now happen after
the encoder finishes publishing the pose, shortening the vulnerable interval.
Rate-limited diagnostics report unavailable reads, reused packets and input age.

The regression now passes, including held grip/trigger actions, unchanged pose
timestamps in published frames, actual release, timeout, recovery and invalid
packets. Native checks, syntax checks, encoder type checking and ASan/UBSan/TSan
pass. The effect on the user's picture shifts and object drops still needs a
headset retest.

## Status (2026-10-28): shadow flicker report and pending pipeline draws

The user reports that shadows kept flickering during native gameplay in session
`882CBA5A-3AD4-4A31-9B48-2A57496BCEBC`, including after loading. Its host log
records `33306 binds skipped while compiling` by the 250th compiled pipeline.
These are diagnostics from the user's session, not a harness result. They
identify omitted draw work; they do not establish the cause of the reported
persistent flicker.

The asynchronous pipeline shim previously returned from a bind immediately and
dropped draws until the next graphics-pipeline bind if compilation was pending.
That can omit shadow casters from a cached shadow map and draw different sets
of objects for the two eyes. It now waits for that pipeline at its first bind,
while keeping ahead-of-use compilation on worker threads. A new material can
therefore cause a first-use pause. Failed compiles still suppress draws rather
than using a stale binding; their error and the final empty queue are logged.

The controlled compiler regression fails with the previous implementation and
passes after the change: concurrent shadow/eye binds retain their first draw,
creation state survives the caller reusing its memory, and compilation failure
wakes all waiters and permits a later valid binding. It is part of `make check`
and the address/undefined/thread sanitizer targets. The host build also tracks
the copied pipeline include so an incremental rebuild contains this change.
Headset verification of the flickering shadows and first-use pauses is pending.
No game files, graphics settings, playtest level or performance baseline change.

### Follow-up: flicker gone, compilation stalls during movement

The user tested that candidate in session
`080ACC25-DFBF-4574-943C-651B037F2CC6` and reports that the shadow flicker is gone,
but sharp head turns lag and frequent severe freezes appeared. The session log
shows continued first-bind waits as new pipelines are created, alongside long
gaps between submitted frames. This is user-session evidence, not a harness
performance result. Waiting at bind exposed the underlying compilation cost;
it is not a complete performance solution.

The worker was always passing `VK_NULL_HANDLE` to MoltenVK instead of the
pipeline cache Source 2 supplied. It now retains that cache through compilation.
Cache creation keeps the driver's internal synchronization enabled; snapshots
and merges wait for pending writers, and cache destruction is deferred until
workers finish. This lets the engine's existing cache reuse and serialization
include asynchronously compiled shaders. A pending pipeline needed by a draw
also moves ahead of background jobs that have not started. Bind diagnostics
separate time spent queued from time in compilation.

The fake-driver regression reproduces the discarded cache argument before this
change and covers cache lifetime, snapshot/merge barriers, demand priority and
retained shadow/eye draws. The optional `make -C NativeARM check-pipeline-cache`
uses the locally available MoltenVK and synthetic SPIR-V: it creates pipelines,
saves and reloads cache data, recreates shader modules, and checks MoltenVK's
conversion counters. The cold pass converts shaders; the reloaded pass performs
no SPIR-V-to-MSL conversions. It starts no game or headset session and downloads
nothing. This does not establish Alyx performance: cold shaders can still pause,
and warm-scene frame timing, cache persistence in the engine and head-turn
latency need a user retest. No quality settings or baselines change.

### Follow-up: progressive materials and a native loading map

The user reports that the cache candidate still freezes and shadows flicker
again in session `9E54D0A5-05D3-4A2C-9451-A20D9AE240F6`; the loading map is also
absent. Its bind diagnostics show compilation time rather than queue delay.
These reports supersede any expectation that the previous candidate solved
the visual or performance problems. There is no new game performance baseline.

Colour pipelines now become visible at the next `WaitGetPoses` frame boundary
after compilation, allowing progressive material loading without waiting at
every bind. Both eyes use the same readiness decision. Depth-only passes still
wait to keep cached shadow maps complete; first-use depth compilation can
therefore still pause. Cache reads take coherent snapshots of completed entries
without draining the queue. Source-cache merges also no longer drain pending
writers; only a merge destination waits for exclusive access. Size/data query
pairs remain coherent as compilation continues, including short-buffer retries.

The native mailbox previously acknowledged the loading service without drawing
its scene graph. It now publishes the level's loading state to a private
`loading-map.json`. The encoder reads the installed game's map OBJ geometry,
renders a stereo overview with the current level highlighted, and sends it on
loading heartbeats even when the game submits no frames. Asset reads and drawing
run off the encoder loop, and the resulting image is cached. End-of-load and
session cleanup remove the map. This is a native map overview; the full SteamVR
web scene-graph renderer is not implemented. No game models or textures are
copied into the app or repository, and the trigger prompt remains disabled.

Regressions cover nonblocking colour binds with a deliberately stalled compiler,
activation in both eyes at the next frame, retained depth draws, cache snapshots
and source merges while compilation is blocked, destination synchronization,
coherent size/data pairs and short-buffer retries. Native mailbox and session
checks cover map delivery and cleanup. `make -C NativeARM check-loading-map` uses
two synthetic OBJ fixtures, exercises asynchronous rendering and clearing, and
rejects traversal names. A separate read-only check rendered the user's installed
map assets. Gameplay flicker, first-use pauses and loading transitions still need
headset verification; no game was started by the agent.

### Follow-up: cache isolation and actual render-pass attachments

The user reports continued shadow flicker and first-use effect stalls in native
session `70322519-642A-4E8D-A893-A2266F72D9E0`, including when shooting a lock.
The log shows new pipeline batches, a cache-serialization error and a long frame
gap around a later material/effect batch. These are user-session diagnostics;
the exact effect responsible is not identified by the existing log.

Workers were still compiling into caches owned by the engine. The wrapper had
to wait for outstanding writers when the engine merged into a destination cache,
and MoltenVK's own cache locks could also hold up serialization. A regression
with a deliberately stalled compiler reproduced the blocked destination merge.
Workers now create a private cache seeded from the engine cache, compile outside
the engine-cache lock, then merge completed results back. All direct engine-cache
operations share the short publication lock. A render thread can snapshot or
merge engine caches while a worker remains blocked in compilation.

Depth-pass classification also used colour-blend fields without checking the
render-pass attachments. Vulkan ignores those fields in a depth-only pass, so
an alpha-tested shadow pass could be treated as a progressive colour material;
a colour-only effect with dynamic blend state could be made to wait instead.
The runtime now records the attachment usage of every subpass for render-pass
versions 1 and 2, and reads dynamic-rendering formats. Ignored colour/depth state
is omitted from copied creation data. Depth pipelines log their actual targets.

Both regressions fail with the previous code and pass after the change. Native
unit checks include ignored pointers, unused colour attachments, subpass changes,
render-pass 2, cache lifetime and shader/frame concurrency. The MoltenVK check
also submits a depth-only draw with a fragment shader and misleading ignored
blend state, then verifies every depth pixel by GPU readback. It uses synthetic
shaders and no game. This validates these defects, not the user's final headset
result: native gameplay flicker and first-use stalls still need confirmation.

## Status (2026-10-29): archive restoration must not starve material workers

The user reports a uniform pink headset picture after the progressive-rendering
change. Native sessions `387E1E14-A5C3-4FFD-8A5F-F1C93DFD9475` and
`5EEA16EB-AD6A-4A7E-878F-69D462F57DAC` publish identical flat eye images
(sample deviation zero) and start three compilation workers, but record no
completed pipeline or completed archive import. This is already present before
encoding; it is not evidence of a transport-only failure.

The persistent archive had grown to 12,960,564 bytes. A separate read-only
MoltenVK import of that archive took 63,368.6 ms. Sampling that synthetic process
found `MVKPipelineCache::readData` rebuilding Metal libraries. Import ran inside
the archive mutex on a material worker; the other workers waited for the same
mutex. Progressive rendering then omitted every pending draw, retaining the
clear image. The previous small synthetic archive test did not expose this.

Restoration now runs on a dedicated thread into its own cache, outside the
archive mutex. Material workers immediately use current-session caches; they
skip optional archive seeding/publication when its mutex is busy. The imported
libraries merge into the live archive on completion. Saves preserve the old
file until restoration finishes, and device teardown waits for that thread
before destroying its Vulkan objects. There is no return to waiting in graphics
binds, and no need to delete the user's cache.

A regression deliberately stalls archive import: it fails against the previous
code and passes with this change, including a completed material/draw, preserving
the old file, busy archive operations, and cleanup. Native checks, sanitizers
and synthetic MoltenVK cache/depth-readback tests pass. The optional large-cache
GPU check completed a synthetic draw/readback while restoration was still active;
its source archive's SHA-256 was unchanged. The initial standalone import had
already warmed Metal's own disk cache, so its timings are not a before/after
game performance comparison. The Windows check still fails on the previously
recorded translator differential cases. No game was launched or gameplay
baseline changed; headset confirmation and the separate shadow issue remain.

## Status (2026-10-29): startup clears and offscreen shadow report

Latest user headset follow-up: freezes are gone, but successive attached frames
still alternate between a missing and present offscreen-caster shadow. This is
a user report, not a formal playtest. The shadow symptom remains unresolved.

The user narrowed the remaining shadow symptom down to casters leaving the head
frustum: their shadows break while the caster is offscreen and recover on looking
back. User session `F9D54CC7-A0FC-4887-8F39-1979DF9A7485` also starts with flat
eye images before its intro. These are user observations and diagnostics, not a
confirmed explanation for all shadow flicker.

The native compositor now presents entirely magenta startup clear images as
black, for both eyes together. It checks converted pixels on the readback worker,
verifies every pixel only after a cheap candidate check, and stops filtering once
scene content appears. A black intro does not end protection prematurely; later
authored magenta is retained. No pipeline wait or sleep was added. The synthetic
startup regression fails before this change and passes after it.

The same user log shows Source 2 requesting unsupported EDS3 dynamic colour-write
masks. Previously the shim disabled that feature only when retrying device
creation, after the engine had selected dynamic state. This violates the
[Vulkan dynamic-state requirements](https://docs.vulkan.org/spec/latest/chapters/pipelines.html):
the engine can still emit a write mask that the driver cannot apply. This is
a concrete compatibility defect; its connection to the particular offscreen
shadow report remains to be confirmed. The native host now hides EDS3 from this
engine when the GPU lacks dynamic colour masks, letting it select static pipeline
variants before device creation. Fully capable drivers retain EDS3. Enumeration
keeps the two-call/count/capacity contract. `KL_VK_ALYX_STATIC_COLOR_WRITE=0`
restores the previous negotiation for a controlled diagnostic.

`tools/render_probe.py` runs the licensed installed ARM64 game in a fresh APFS
clone under `runs/`, replacing mutable cfg/save/cache links with private
directories. Steam identity and ownership checks remain the real client's. It
refuses an existing game/SteamVR session, starts no encoder, synthesizes head
turns, samples coherent shared frames, hashes the retained artifacts, and seals
its result into the common ledger. At the end of collection, or on SIGINT /
SIGTERM, it requests normal exit through the same native OpenVR protocol.
`--quit-timeout 60` sets when a slow quit is reported; the default continues
waiting without sending the game a signal. `--allow-force-kill` explicitly
permits SIGKILL of its owned process group after that wait. The developer
`scripts/run_timeout.sh` uses the same protocol and flags; a command that does
not implement it must exit on its own unless force was explicitly enabled.

While waiting, the probe retains its overlay/frame map and checkpoints images,
`frames.json`, `pending.json` and `shutdown.json`. It keeps the held head pose
live without synthesizing further trigger presses. Final evidence includes
the shutdown method, delivery/acknowledgement, wait and exit code, and hashes a
closed log snapshot (`alyx_host-final.log`). Collection and total duration are
reported separately. The ledger is sealed and the private overlay removed only
after game exit; results and images are retained.

The train probe `20261029T084136Z-alyx-native-a1_intro_world_2` and quarantine
probe `20261029T084312Z-alyx-native-a2_quarantine_entrance` complete without early
exit or fatal diagnostics (`o1`, `o7`); their logs no longer request the unsupported
device feature. The control probe `20261029T084603Z-alyx-native-a2_quarantine_entrance`
restores EDS3 and reproduces that request. GPU capability/cache/depth-readback
checks, native regressions, syntax/encoder checks and sanitizers pass. The
nonblocking colour/depth bind policy remains covered by the blocked-compiler
tests. The full Windows runtime check still stops at its previously documented
translator differential failures; no Windows runtime source changed.

The packaged-host probe `20261029T085017Z-alyx-native-startup` completes 65 seconds
(`o1`) with zero sampled magenta-clear frames (`o5`); seven clear pairs were
presented as black (`o16`). It records no fatal diagnostics (`o15`) or unsupported
device-feature request (`o17`). Its largest sampled publication gap is 194 ms
(`o4`), so it does not establish hitch-free rendering.

These probes are not headset playtests or a frame-time benchmark. Their sampled
publication gaps include collector work in the earlier probes; they do not establish stall-free gameplay
or disappearance of the reported offscreen-caster shadow flicker. No gameplay
baseline or playtest level changed.

### Follow-up: synchronous engine cache exports during head turns

A clean probe with image conversion deferred until after play,
`20261029T085608Z-alyx-native-a2_quarantine_entrance`, still records a 351 ms
publication gap (`o4`). Profiling probe
`20261029T085825Z-alyx-native-a2_quarantine_entrance` retains native stacks
(`o22`): frame workers spend time in `klvk_GetPipelineCacheData`, both waiting
for `g_kva_cache_mu` and serializing shaders in MoltenVK. Background compilation
alone did not remove the frame's synchronous cache-export cost.

With `KL_VK_PRIVATE_CACHE=1` (the native host's default) and a configured persistent
archive, completed asynchronous shaders now stay in the worker archive instead
of being copied back into the engine's per-thread caches. Engine cache exports
still return the real driver cache data; the cache no longer grows with those
background shaders. Archive-mode workers seed and publish only through the
worker archive, so they never acquire the engine-cache mutex. Without a
configured archive the previous engine seeding/publication policy is retained. No draw waits
or fabricated cache blobs were introduced. Slow exports (8 ms or more) are logged
outside the cache lock.

A regression deliberately blocks publication into the engine cache: it fails
before this change and now completes compilation, drawing and cache export while
the archive is saved. Real MoltenVK checks verify that the engine cache stays its
initial size, the persistent archive still gives zero SPIR-V conversions with
recreated shader modules and empty/null engine caches, and the GPU depth draw
survives. Native checks, syntax checks and ASan/UBSan/TSan pass.

The intermediate packaged probe
`20261029T091427Z-alyx-native-a2_quarantine_entrance` still records four slow
exports of just 36 bytes (`o23`) and one sampled gap above 100 ms (`o6`). Cache
allocation has therefore moved outside the shared engine-cache lock. Unchanged
archive-mode exports now reuse their real serialized snapshot; explicit engine
merges and inline graphics/compute creation invalidate the next snapshot. An
already queried size/data pair remains coherent across such a mutation. The
unchanged-export regression fails before this change; regressions also cover
export progress while worker cache creation is deliberately blocked, snapshot
invalidation and the preserved size/data pair.
The final isolation regression also deliberately blocks reads from engine
caches: archive-mode compilation and exports must finish without entering that
read path. Persistent reuse with empty/null engine caches still passes on the GPU.

The new policy is bracketed by native control probes. Control
`20261029T090907Z-alyx-native-a2_quarantine_entrance` sets `KL_VK_PRIVATE_CACHE=0`
and reproduces a 665 ms sampled gap after second 30 (`o5`), two gaps above 100 ms
(`o6`) and repeated slow engine exports (`o23`). This is an isolated simulated
head-turn test, not a headset performance baseline. The earlier uninstrumented
private-cache probe `20261029T090344Z-alyx-native-a2_quarantine_entrance` records
a 140 ms maximum including startup (`o4`). Packaged verification uses the
same controlled route. The offscreen shadow report still needs a real
headset retest.

Final probe `20261029T093437Z-alyx-native-a2_quarantine_entrance` records a 37 ms
maximum sampled gap after second 30 (`o5`) and no gaps above 100 ms in that
window (`o6`), compared with the control's 665 ms maximum. Sampled magenta clears
are zero (`o7`), with no fatal diagnostics (`o20`) or unsupported feature request
(`o22`). Startup still includes a 126 ms sampled gap (`o4`) and slow engine-cache
operations (`o23`). This verifies the tested route, not universally hitch-free
gameplay or the particular offscreen shadow symptom. No gameplay baseline or playtest level is changed.

## Status (2026-10-29): prioritize continuous rendering during shader warmup

The user confirms the loading hint is now correctly timed, but first head turns
and pistol effects still hitch until warmed up; shadows still flicker, including
after temporarily recovering on save reload. User session
`E40B506D-A958-4B41-8AE0-11664FA46B6B` used the all-first-draw-waits policy below.
Its log records individual bind waits around 250–590 ms, mostly compilation,
and ends with 473 completed pipelines, no failures or suppressed failed binds.
These are user-log diagnostics, not a harness performance measurement. Keeping
every draw did not eliminate the reported flicker, so pending-draw omission is
not an established explanation for that session's shadow problem.

At the user's explicit preference, **all pending graphics binds now return
without waiting**, including depth/stencil passes. Their draws are omitted until
the pipeline is ready. Already compiled materials continue to render; new walls
or effects can temporarily be absent. Compute pipelines and unsupported graphics
creation data retain the synchronous fallback. The persistent shader archive
remains enabled to reduce repeat work across sessions.

Workers prioritize pipelines demanded by a draw through an atomic hint; binds
never acquire the queue mutex. Cache creation, seeding and merging use a separate
mutex, so a driver cache operation cannot block enqueueing the next material.
Deferred driver-object destruction also runs outside the queue lock. A separate,
short readiness snapshot at WaitGetPoses activates completed pipelines for both
eyes together, avoiding a completion between the two eyes changing visibility.

The blocked-compiler regression fails with the previous waiting implementation
and passes with progressive binds. It covers colour and depth draws, same-frame
suppression after a compiler finishes, activation at the next frame, demand
priority, cache lifetime and queue progress while cache work is stalled. Native
checks and ASan/UBSan/TSan pass; the MoltenVK test verifies archive reuse with
empty/null engine caches and GPU depth readback after readiness. This does not
prove stall-free gameplay in the headset. Shadow flicker remains unresolved;
omitted cold draws can also leave cached shadow contents incomplete until the
engine redraws them. No gameplay baseline or playability level changed.

## Status (2026-10-28): complete passes, persistent shaders and loading stages

The first-draw waiting policy in this historical entry is superseded by the
2026-10-29 continuous-rendering policy above.

User session `B5160DC2-54E3-49A5-A165-5A46AEE092EE` records the input hint
before `Medved paused the game` (host log lines 9386 and 9453). It also starts
its engine pipeline caches with zero bytes and later records first-bind waits
whose time is mostly compilation. These are user-session diagnostics, not a
harness performance measurement or proof that all reported flicker shares one
cause. The earlier progressive-material experiment is superseded: colour
passes can also write cached lighting/shadow masks or stencil. All first draws
are retained; failed pipelines alone suppress draws.

The native supervisor now supplies `KL_VK_PIPELINE_ARCHIVE` in its persistent
state directory, outside game assets and per-session logs. Workers seed their
private caches from completed, rewritten shaders across engine caches and
sessions, including calls with no engine cache. Completed results are saved
atomically after first-bind waiters are released, outside the engine-cache
mutex. Saves are throttled; device teardown waits for workers and flushes the
remaining entries. Abrupt termination may lose the last few seconds of cache
updates. This avoids repeating shader translation for cached materials; a
previously unseen shader can still stall at first use. It is not a guarantee
of stall-free cold gameplay.

The loading state now combines the mailbox hint with the engine's actual
pause notification, independent of the player's name. It displays Loading
until that pause, clears the hint on unpause/end/new load, and ignores late
messages outside a load. The first `startup` load sends black frames without
a room, map or hint, then returns to the game's own frames for its intro.

`SetStageOverride_Async` now supplies the installed OBJ room, DDS texture,
render settings and model-to-standing transform to the encoder. The renderer
acknowledges only a prepared matching request; it retains the room during the
map and input hint, samples the real head pose per eye, and associates the
cached image with its original pose timestamp. This replaces the generic room
with the stage requested by the game. The city uses the installed authored
map data. A read-only render of the installed train diorama exposed two more
defects: its 89,478,660-byte DDS exceeded the old 64 MiB file limit, and direct
Metal texture rows needed a V-axis flip for the OBJ atlas. The loader now maps
larger DDS containers and uploads at most a 4096-square mip. The train room
render passes after these changes; a sparse synthetic large DDS covers the
size/mip regression. No game asset is copied into the repository or app; test assets are
synthetic. Room choice and alignment during an actual train transition still
need a headset retest.

Offline checks cover both pause/message orders, stale events, initial boot,
room/pose delivery, asynchronous scene rendering, preservation of colour and
depth draws, and a blocked archive writer while a first bind completes. The
local MoltenVK test reloads the archive with recreated shader modules and
verifies zero SPIR-V conversions with empty and null engine caches, plus GPU
readback of the first depth draw. Native checks and ASan/UBSan/TSan pass. The
full Windows `make check` reaches its unrelated translator differential test
and fails on cpuid/xgetbv and gather forms; those sources were not changed.
No game was launched, baseline changed, or playability level raised.

## Status (2026-10-27, follow-up): no native trigger overlay

At the user's request, the native helper no longer gives the encoder a loading
prompt path, including after an encoder restart. The host still records mailbox
messages for diagnosis, and trigger input is unchanged. Session checks cover an
inherited prompt path, restart, private host diagnostics and cleanup. The native
checks and syntax checks pass; no game or headset was started for this change.

The current Recommended / Sharper choices select a fixed rendering budget and
stream pixel budget. Native bitrate ceilings are 40 / 150 Mbps. The encoder
already adapts bitrate within either preset: every 5 seconds, two or more loss
events lower it by 20%; 30 clean seconds raise it by 10%. The default floor is
40% of the ceiling; Sharper preserves its earlier 28 Mbps native / 40 Mbps
Windows floor when raising the ceiling to 150 Mbps. ALVR contributes decoder-recovery requests and two loss
events when mean network time exceeds 80 ms or enough frames are sent without
any display statistics. Total motion-to-display latency is not used as network
latency: it also includes the Mac's rendering and the headset's processing.

## Status (2026-10-27, follow-up): continuous movement and turning inputs

The user reports no forward movement in Continuous mode and world-camera turns
when pressing face buttons even with snap turning disabled. Valve documents
separate snap/continuous turning and an option to disable controller turning:
[Alyx update 1.1](https://store.steampowered.com/news/posts/?appids=546560&enddate=1585431603&feed=steam_community_announcements).
The installed Touch binding uses the off-hand stick's deflection for walking,
the main-hand stick for turning, and no A/B/X/Y bindings for turning.

The native reader ignored `force_input: position`, so its boolean Walk action
required a physical stick click. Session `CAFDC9DC-9B0C-42DA-AA52-2DEB32F8DC8C`
shows the left stick deflected while Walk remains off. Position-threshold
buttons now use stick magnitude and independent activation/release thresholds;
the 0.25/0.20 defaults were checked against the installed SteamVR binding UI.
Physical stick clicks remain separate. Joystick position bindings now apply
their configured deadzone/maxzone, including Alyx's 75% continuous-turn
deadzone; D-pad bindings distinguish touch from click activation.

Synthetic locomotion fixtures reproduce the binding mechanisms without
redistributing the game's files. The walking regression failed before the fix.
Checks cover forward/backward/sideways deflection, threshold hysteresis, actual
stick clicks, analog deadzones, touch/click D-pads, inactive action sets, stale
input and face buttons with small stick drift. Native checks, syntax checks and
ASan/UBSan/TSan pass.

The exact face-button turning report is not established by the existing
five-second input samples. New rate-limited `locomotion:` lines record raw
sticks, face buttons and evaluated Walk/TurnLeft/TurnRight/ContinuousTurn
states at transitions, so a new user run can distinguish unexpected input from
the game's response. No game settings, saves or Valve files were changed.
Continuous movement and the turning option still require a headset retest.

## Status (2026-10-27, follow-up): Sharper bitrate ceiling

Sharper now starts at a 150 Mbps ceiling for both native ARM64 and Windows
ALVR sessions. Rendering and stream sizes stay at their existing Sharper values.
Adaptive bitrate remains enabled by default. The app preserves the previous
fallback floors (28 Mbps native, 40 Mbps Windows) unless a developer explicitly
overrides `VERANTYX_BITRATE_MIN_MBPS`; raising the ceiling therefore does not
force the minimum up to 60 Mbps. The existing controller still checks every
5 seconds, lowers bitrate by 20% on link trouble and raises it by 10% after
30 clean seconds. It reacts to network delay and video loss, rather than
automatically lowering game rendering quality when the Mac is busy.

## Status (2026-10-27, follow-up): duplicate legacy face-button input

The user still reports A/X turning the world despite turning being disabled;
A also performs its expected magazine-eject action. Session
`4DFC0C08-3ADF-4E98-A1CE-7A506A056E37` records these face-button presses with
centred sticks and all three turning actions off. This rules out the previously
fixed analog deadzone as an explanation of those samples.

The native runtime was publishing raw controller buttons through
`IVRSystem::GetControllerState` while also serving the game's action manifest.
A read-only inspection of the installed ARM64 `libvr.so` confirms a legacy
controller-state consumer: at `0xaae08` it calls IVRSystem slot 37 (`0x128`),
with a 60-byte controller-state buffer, then converts its buttons and axes.
No game binary was executed or changed for this inspection. Independent
OpenVR integration reports also describe legacy controller state being
unavailable once action input is selected:
[OpenVR issue 1284](https://github.com/ValveSoftware/openvr/issues/1284).

Successful manifest loading now selects action input atomically. Legacy state
and legacy state-with-pose queries then return unavailable with cleared outputs,
so they cannot feed the same press into another engine input path. Action input
continues to deliver A/X normally, including magazine eject. A failed manifest
reload cannot re-enable the raw channel; an application that has not selected
action input retains legacy buttons, axes and poses. The first suppressed query
per hand is logged to confirm which path the user's engine polls.

A regression failed before the fix because legacy state still exposed the
face buttons. The native normal/syntax/ASan/UBSan/TSan checks now pass, including
legacy-before-manifest behavior, exclusive action input, unchanged magazine
eject and cleared legacy poses.
On 2026-10-27 the user retested the latest app on Pico 4 Ultra and reports that
everything works well. This is user confirmation of the accumulated fixes;
it does not establish a measured FPS comparison or a formal harness baseline.

## Original implementation plan

1. Loader: TLSDESC/TPREL in Klepton's image loader; glibc symbol table
   (generated forwards where signatures and layouts match, hand-written
   shims otherwise, Darwin struct layouts for stat/dirent/pthread types).
2. Launcher: load `libengine2`, call `Source2Main` with `-vr -nowindow`,
   stub X11/Wayland/SDL video, iterate until the engine reaches
   "no HMD".
3. OpenVR runtime: `IVRSystem` / `IVRCompositor` (WaitGetPoses, Submit of
   Vulkan images → readback → shared frame memory) / `IVRInput` /
   `IVRChaperone`, reusing the driver's pose logic and the capture proxy's
   frame path.
4. Connect to `MacEncoder/HardwareEncoder` and the Vision Pro app; measure.

## Layout

- `scripts/fetch_alyx_arm64.sh` — steamcmd download of depot 546564 (your account)
- `scripts/fetch_debian_libs.sh` — Debian trixie arm64 runtime libraries
- `scripts/fetch_klepton.sh` — clone Klepton at a pinned commit, apply the
  CoreAudio link fix, build `klepton-ld`
- `scripts/translate_alyx.sh` — run `klepton-ld` over every ARM64 library
- `tools/census.py` — dependency / symbol / relocation census

## Third-party code

- `vrruntime/hand_skeleton_alvr.inc`: OpenVR hand-skeleton bone poses for a
  Touch-style grip, taken unchanged from ALVR
  (`alvr/server_openvr/cpp/alvr_server/Controller.cpp`,
  https://github.com/alvr-org/ALVR), MIT license (text in the file). Used by
  `vrruntime/vr_skeleton.cpp` to give the game finger data (SteamVR does this
  for Touch controllers in the Wine path; there is no SteamVR here).
- Klepton (https://github.com/shinyquagsire23/Klepton), MIT, fetched and
  patched by `scripts/fetch_klepton.sh` / `klepton-patches/apply.py`.
- OpenVR headers (`vrruntime/openvr/`), BSD-3-Clause (Valve), see LICENSE there.
- Fossilize (https://github.com/ValveSoftware/Fossilize), MIT, built by
  `scripts/build_fossilize.sh`; `scripts/prewarm_shaders.sh` replays the
  game's shipped `vulkan_shader_cache.foz` through MoltenVK once so Metal's
  per-user shader cache is warm (first-time pipeline compiles were 250-500 ms
  each on the render thread; cached ones 2 ms).

The delivered application is checked by
`20261029T094446Z-alyx-native-a2_quarantine_entrance`, using its nested host and
its own MoltenVK. After second 30, the maximum sampled gap is 49 ms (`o5`), with
no gaps above 100 ms (`o6`), no sampled magenta clears (`o7`), no fatal diagnostics
(`o20`) and no unsupported feature errors (`o22`). Startup includes a 117 ms gap
(`o4`). The nested host SHA-256 matches that result after final resource packaging.
The particular offscreen-caster shadow report was still pending at delivery.

## Follow-up (2026-10-29): shadow report and isolated capture tools

The user now reports that the shadow flicker stopped during their session and
that the current native build looks correct. This is a user report, not a formal
playtest or a demonstrated new shadow fix. No rendering policy changed during
this investigation, and no causal explanation for the disappearance is established.

`tools/render_probe.py` can copy and load a save with `--load s0/quick`, hold the
head with `--hold-yaw` and `--head-pitch`, and retain small consecutive left-eye
captures with `--burst START:DURATION:HZ`. Burst capture requires Pillow and does
image conversion on the sampler; use it to inspect the picture rather than to
establish frame timing. The save's actual map can override `--map`, so inspect
the engine's `Spawn Server` line rather than treating the run-id suffix as proof
of the scene. Passing only `+load` was discarded during startup; the verified
probe supplies both `+map` and `+load`.

Probe `20261029T102933Z-alyx-native-a2_quarantine_entrance` loads `s0/quick` into
`a1_intro_world_2`, retains 100 burst images (`o22`–`o121`), reaches the requested
duration (`o1`) and has no fatal diagnostics (`o18`). It does not reproduce the
user's original stairway viewpoint and cannot establish that symptom's cause or
resolution. Both diagnostic runs remain sealed in the evidence ledger.

Optional `KL_VK_SHADOW_TRACE=1` logs at most 500 depth-attachment rendering
begins, including load/store operations and suspended/resumed pass flags.
`KL_VK_SHADOW_TRACE_AFTER` delays that window in seconds. This is disabled by
default and does not alter the passes or wait for pipelines.

## Follow-up (2026-10-29): cooperative native shutdown

Probe `20261029T135114Z-alyx-native-startup` completes 25 seconds without early
exit (`o1`). Its `o16` shutdown observation records `VREvent_Quit` delivery and
acknowledgement, exit code 0, force disabled and a 0.149 s wait. The frozen
engine log contains `IVRClientCore::Cleanup` and `Source2Shutdown`; there are
zero fatal diagnostics (`o12`). This verifies normal exit from the startup
scenario with the native host. It does not establish headset gameplay or
behaviour during a real save. No baseline or playtest level changed.

The host inside the delivered app, using that app's MoltenVK, is verified by
`20261029T135835Z-alyx-native-startup`: 25 seconds without early exit (`o1`),
zero fatal diagnostics (`o12`), delivered/acknowledged quit, exit code 0 and
a 0.206 s wait with force disabled (`o16`). Its recorded host SHA-256 matches
the packaged host.

Offline runtime, supervisor, probe, app and sanitizer checks cover the safe
default and explicit force on synthetic processes. The full VXRuntime check
still stops at the previously documented x86-to-ARM64 differential failures
(`cpuid`, `xgetbv`, gather instructions); Windows runtime sources are unchanged.

## Status (2026-10-29): native SDR matching to the Windows picture

The user's `Screens/1 arm.jpeg`, `1 windows.jpeg`, `2 arm.jpeg` and
`2 windows.jpeg` compare both VeRantyx modes on the same headset. All four
files carry sRGB profiles. The existing native run logs show an RGBA8 sRGB
eye image (`VkFormat 43`, `ColorSpace_Auto`); copying it to a buffer preserves
the encoded bytes. Both routes use the same encoder, sRGB transfer metadata
and BT.709 primaries/matrix. Both saved game configurations specify
`r_fullscreen_gamma=2.2`. There is no demonstrated extra HDR conversion in
the native transport. The precise source of the engine-side difference
(different Linux ARM64/Vulkan and Windows/D3D11 render paths, exposure or
tone mapping) remains unproven; changing the transport's sRGB interpretation
would not be a justified fix for this submitted format.

The second pair yields 50 locally corresponding textured patches with
normalized contrast correlation at least 0.95 and reverse matching within
two pixels at 512-square analysis resolution. Their median linear-light
exposure difference is about 1.07 stops, native brighter. The first pair
has substantially different viewpoints and only 17 such matches; it does
not support an exact global match. These are screenshot comparisons,
not game harness measurements or identical-pose captures.

The native compositor now applies a fixed display curve before publishing
both eyes to the shared encoder. In linear light it uses
`out = gain * in / (1 + (gain - 1) * in)`, with `gain = 0.5` by default,
then returns to sRGB. This gives approximately -1 EV in shadows and
midtones while retaining black and display white; it does not grey out
lamp cores with a uniform encoded-RGB multiplier. The same curve is applied
to all RGB channels; alpha, poses, frame pairing and startup clear filtering
are preserved. Loading rooms drawn by the encoder retain their own output.
`VERANTYX_NATIVE_EXPOSURE_EV=0` gives a byte-exact neutral path for comparisons;
finite values from -3 to 3 adjust the curve. It is a display calibration to
these references, not a claim that a native HDR or shader defect is repaired.

Using the actual compiled curve, median absolute RGB error in those patches
falls from 14.96 to 1.48 code values (out of 255) in pair 2 and from 17.94
to 6.56 in pair 1. This is a fit to the supplied images, not independent
headset validation. The byte lookup uses ARM NEON on the readback worker;
there are no per-pixel powers. A CPU-only synthetic 7,372,800-pixel buffer
costs about 3.1 ms median in the host's `-O1` configuration on this Mac.
This is conversion time, not a gameplay FPS result.

Offline regressions cover the publish path in both eyes, a neutral mode,
monotonicity and black/white anchors, invalid settings, every byte value,
alpha, unaligned buffers and SIMD tails. Native checks and syntax pass.
ASan, UBSan and TSan also pass. The publish regression fails against the
previous compositor and passes with the correction. The app is rebuilt
and its nested signatures verify.
The full Windows check still reaches the pre-existing translator differential
failures above.

Packaged-host probe `20261029T150612Z-alyx-native-a2_quarantine_entrance`
completes its 40-second collection without early exit (`o1`), captures five
frames (`o9`-`o13`), records zero fatal diagnostics (`o14`) and exits normally
after an acknowledged quit (`o18`, exit 0, no force). Its frozen log records
the active -1 EV curve; all observation hashes and the ledger verify, and
the host hash matches the delivered app. It does not compare the supplied
viewpoints or validate the picture in a headset. That comparison remains
required; no measured baseline or playtest level has been changed.

## Status (2026-10-29): isolate native shadow flicker to argument-buffer bindings

The user's screen recording shows large ground shadows appearing and disappearing
at a nearly unchanged viewpoint. Their latest `s0/quick` now reproduces that
viewpoint in `a2_quarantine_entrance`. Fixed-head harness captures reproduce the
same symptom after shader warmup. Synchronous pipeline creation does not remove
it, and an experimental split of suspended depth passes did not help; that
experiment was reverted.

Changing only `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` isolates the affected
resource-binding path. The native Alyx host now defaults it to `0` before loading
MoltenVK, and logs the effective mode. An explicit `1` remains available for
driver comparisons. This selects MoltenVK's direct descriptor bindings; it does
not disable shadow passes or change game files, quality or the nonblocking
pipeline policy. The exact internal driver defect is not established by these
controls; the pipeline-cache dynamic-offset issue in upstream release notes is
not treated as a proven diagnosis.

Comparison `20261029T155153Z-alyx-native-shadow-comparison` hashes its source
harness results and every source burst capture. Its rule counts adjacent 512x512
left-eye images whose mean absolute RGB difference exceeds five code values;
the retained contact sheet shows the large shadow changes this detects:

| Path and source run | Large changes / adjacent pairs | Claim |
| --- | --- | --- |
| Argument buffers, synchronous: `20261029T153856Z-alyx-native-a2_quarantine_entrance` | 123 / 269 | `d1` |
| Direct bindings, synchronous: `20261029T154256Z-alyx-native-a2_quarantine_entrance` | 0 / 257 | `d2` |
| Direct bindings, asynchronous: `20261029T154558Z-alyx-native-a2_quarantine_entrance` | 0 / 268 | `d3` |
| Argument buffers restored, asynchronous: `20261029T154802Z-alyx-native-a2_quarantine_entrance` | 74 / 268 | `d4` |
| Packaged default, copy of existing user cache: `20261029T154924Z-alyx-native-a2_quarantine_entrance` | 0 / 358 | `d5` |

These are fixed-head image comparisons, not frame-time benchmarks. The packaged
probe captures after the old archive has finished importing. It has no fatal
diagnostics (`o22`) or unsupported device-feature errors (`o24`) and acknowledges
normal quit with exit 0 and force disabled (`o385`). Its host SHA-256 matches the
signed app. The original user cache's SHA-256 remains unchanged.

Native runtime/syntax and host checks pass. The synthetic MoltenVK regression
with direct bindings verifies all 16 shadow-depth pixels and persistent shader
reuse, including a draw while an argument-buffer archive imports. The full
Windows check still fails on the previously recorded translator differential
cases; Windows runtime sources are unchanged. A headset retest and other maps
remain unverified. No gameplay baseline or playtest level changes.

The packaged default is also captured with simulated head turns in
`20261029T155213Z-alyx-native-a2_quarantine_entrance` (`o21`-`o559`). The settled
forward, left and right views were inspected without the alternating ground
shadows. This probe records zero fatal diagnostics (`o17`) and unsupported
feature errors (`o19`), then exits normally (`o560`). Newly encountered materials
can still appear progressively during a turn, as specified by the existing
pipeline policy. These captures remain an isolated diagnostic, not a headset
playtest or proof of all viewpoints.
