# Keychain prompts on a first build, and the warm-up signature

## What happens

macOS sometimes asks before `/usr/bin/codesign` may use a tester's Apple Development signing key: "codesign wants to access key …", which takes the Mac password plus Always Allow.

A Glovebox build signs 94 things: the app and about 93 embedded guest frameworks. xcodebuild runs one codesign process for each of them, many in parallel.

On 2026-10-07 at 16:35:05 UTC, 36 of those processes reached a new free-team key in the same second. SecurityAgent logged 36 "displaying keychain prompt for /usr/bin/codesign" lines, so 36 windows opened at once.

Always Allow did stick: no prompt started after that second. But every window already open needed its own answer. One window ended without access, codesign failed with `errSecInternalComponent`, and the build failed.

## Why some keys ask and others don't

It depends on which program made the key, not on the account.

A private key in the login keychain gets a partition list from the code signature of the process that adds it. codesign signs silently only when `apple:` is on that list.

| Who made the key | Its signature | Partition | codesign |
|---|---|---|---|
| `xcodebuild` (a Glovebox build), or the Xcode app from developer.apple.com (.xip) | Software Signing, `anchor apple` | `apple:` | silent |
| The Mac App Store copy of the Xcode app | App Store signing, team 59GAB85EFG | not `apple:` | asks |

- On the maintainer's Mac, the paid team's key was made by `xcodebuild` on 2026-09-28 and has never asked.
- The free team's key was made by the App Store Xcode app on 2026-10-07 and asked on every signature.
- Always Allow adds `apple:` to the key for good. Plain Allow lets one signature through and changes nothing.
- Signing in is enough for the App Store Xcode app to make one. On 2026-10-07 the free team's certificate and key appeared when the maintainer signed in to Xcode's Apple Accounts settings: no project was open, and Manage Certificates was never used. Apple's docs and a strings scan of Xcode 27 name only Manage Certificates "+", a project's signing repairs and distribution, so the exact path inside Xcode is not known.
- A free account can't swap that key for one that doesn't ask: for a Personal Team, Manage Certificates offers no Revoke (only Export, a greyed-out Delete and Email Creator), and Apple refuses a second development certificate while one is current ("You already have a current Development certificate"). Checked on 2026-10-07.
- So with the App Store Xcode, macOS asking once is the normal case for a new player. The installer shows a card first ("macOS will ask for your Mac password once", and why). macOS asks only after Continue, a guide under its window points at Always Allow, and it never asks again. A key Glovebox's build made never asks. Xcode from developer.apple.com is signed as Apple's own tool, so its keys most likely don't ask either; that is not yet tested with a sign-in.

Reading a key's access lists (`SecKeychainItemCopyAccess`, as `security find-identity` does) never prompts, so the installer checks the key before it builds.

## The warm-up

Before xcodebuild, the engine signs one plain scratch file with the team's signing identity, alone and with a timeout (`warm_up_signing` in engine/gb/build.py):

```sh
printf 'Glovebox\n' > "$WORK/warmup/signing-check"
/usr/bin/codesign --force --sign "$IDENTITY_SHA1" --timestamp=none "$WORK/warmup/signing-check"
```

- **Key that allows codesign:** it finishes in under a second with no prompt. On 2026-10-07 the rebuild after it showed 0 prompts.
- **Key that asks:** exactly one prompt. Always Allow then covers the parallel signing in xcodebuild.
- **No key yet:** skipped. xcodebuild makes the key, and that key doesn't ask.
- **Timeout or failure:** stop with a clear message rather than starting the build.

The scratch file is not a program on purpose: a universal program carries one signature per architecture, and plain Allow covers only one, so the second would ask again.

`$IDENTITY_SHA1` is the newest valid "Apple Development" identity whose certificate's subject OU is the build's team.

Testers must click Always Allow with the mouse. Enter picks the default button, which is Allow (once).
