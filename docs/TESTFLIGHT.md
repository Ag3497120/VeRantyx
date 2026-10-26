# Distributing the Vision Pro app with TestFlight

The Vision Pro app contains no game code: it shows the picture the Mac sends and returns head and hand poses.
It can therefore be distributed on its own, unlike a port that runs the game on the headset. Players still set
up the Mac side themselves.

Needs an Apple Developer Program membership (paid) for the team that publishes. Testers need nothing but the
TestFlight app.

## Once

1. **App Store Connect > Apps > + > New App**: platform visionOS, name (for example "VeRantyx"), bundle ID
   `marimokodai.VeRantyx.vision` (or set `VERANTYX_BUNDLE_ID` and register your own), any SKU.
2. **Xcode > Settings > Accounts**: the team's Apple ID is signed in. The first export creates an Apple
   Distribution certificate in that account.
3. **TestFlight > Test Information**: a beta description, a feedback email, and a privacy policy URL (needed for
   external testers). The app collects nothing and has no tracking (see `PrivacyInfo.xcprivacy`); the policy can
   say so.

## Each build

```bash
scripts/testflight_visionos.sh --upload
```

It archives with a new build number (UTC `yyyymmddHHMM`) and uploads. The build appears under TestFlight after
processing (10-30 minutes). Builds expire after 90 days.

- **Internal testing**: up to 100 members of the App Store Connect team, no review.
- **External testing**: up to 10,000 testers through a public link, after Beta App Review (usually about a
  day for the first build).

## Notes for Beta App Review

Reviewers cannot run the Mac side, so tell them what they will see:

- The app receives a video stream from a companion program on the user's own Mac (open source, linked in the
  description) and shows it in a Full Space; without the Mac it shows "Waiting for your Mac".
- Hand tracking moves the game's hands; world sensing provides the head pose; local network receives the stream.
- Attach a short screen recording of a session.

Keep game names and artwork (for example Valve's) out of the app name, icon, screenshots and description:
describe it as a client that streams PC VR from your Mac.
