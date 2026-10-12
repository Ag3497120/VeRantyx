# GloveboxReports

The headset side of the opt-in stutter reports. A Swift package the headset launcher links.

- `ReportUploader(documents:)`:
  - `.consent` is yes, no or ask: the newest of two answers. The Mac app's answer comes with the build
    (`GLOVEBOX_SHARE_STUTTERS=1|0` and `GLOVEBOX_SHARE_STUTTERS_AT` in `alyx-demo/alyx-env.txt`); the headset's is
    `Documents/klfz/report-consent.json` (`{"answer": true|false, "at": <unix time>}`, the launcher's format). Neither:
    ask.
  - `try .setConsent(_:)` keeps the headset's answer, timed now; `.ask` forgets it.
  - `await .uploadIfAllowed(maxBatches:)` sends what's new, in up to that many reports (default 20); the rest waits for
    the next call.
- What a report holds:
  - every pipeline the app recorded (`Documents/klfz/recorded.foz`), with each shader module replaced by a SHA-256
    fingerprint and its size;
  - the hitch-log lines (`Documents/klfz/slow.txt`) not sent yet;
  - the "Preparing graphics" runs not sent yet (`Documents/klfz/prewarm.log`: when, how long, pipelines, over 100 ms);
  - how smoothly each minute of play ran, not sent yet (`Documents/klfz/perf.txt`, written by Klepton: seconds into
    the session, game frame rate, display rate, thermal state, memory left and used), to see how long sessions hold
    up. Only how the game ran, as the opt-in card promises;
  - the headset model, visionOS version, Glovebox and kit versions;
  - an anonymous install id.
- Uploads go to `https://glovebox-reports.vercel.app/api/report` (`reports/`).
- `swift test` checks the consent rules. `GLOVEBOX_LIVE_UPLOAD=1 GLOVEBOX_TEST_FOZ=<recorded.foz> swift test` also sends
  test reports (glovebox "uploader-test") to the live endpoint and checks that only what's new goes up.
- The game app calls it from `launcher/App/AlyxLauncher/StutterReports.swift`: one report per launch, since the
  endpoint keeps only a few an hour (`reports/`).
- `reportctl <recorded.foz> <slow.txt> <out dir> [--upload]` does the same on a Mac, for testing.
