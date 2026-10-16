# Mitten

*A mitten from the glovebox.* A mitten is a glove without fingers: this is Glovebox's Half-Life: Alyx for hands
alone, with no tracked controllers. The name is a nod to the project it comes from; it is not Glovebox.

```
           _____________
          /@@@@@@@@@@@@@\
         |@@@@@@@@@@@@@@@|
  ___    |@@@@@@@@@@@@@@@|
 /@@@\   |@@@@@@@@@@@@@@@|
|@@@@@\  |@@@@@@@@@@@@@@@|
 \@@@@@\ |@@@@@@@@@@@@@@@|
  \@@@@@\|@@@@@@@@@@@@@@@|
   \@@@@@@@@@@@@@@@@@@@@@|
    '.@@@@@@@@@@@@@@@@@.'
      |===============|
      |===============|
      '---------------'
```

(The icons are this ASCII art, drawn by `tools/ascii_icon.py` from `tools/mitten.txt`; Glovebox's artwork is not used.)

**Unofficial and experimental.** Half-Life: Alyx running on Apple Vision Pro itself (no PC, no streaming), played
**without PS VR2 Sense controllers**: with a Nintendo Switch Joy-Con in each hand, or with bare hands.

This is the `standalone` branch of [VeRantyx](https://github.com/Ag3497120/VeRantyx). It is based on
**[Glovebox](https://github.com/glovebox-vr/glovebox) 0.1.5** (commit `521e6a6`) by the Glovebox contributors, under
the MIT License, and keeps Glovebox's history so later releases can be merged. It is not Glovebox, is not made or
supported by the Glovebox contributors, and is not affiliated with Valve, Apple, Sony or Nintendo.
**If you play with Sense controllers, use [official Glovebox](https://github.com/glovebox-vr/glovebox).**

What this build adds to Glovebox:

- **Joy-Con:** a single Joy-Con (L) / (R) in each hand, or a joined pair, for the buttons, sticks and triggers;
  where each hand is comes from Vision Pro hand tracking. A hand the cameras lose keeps its last pose, so its
  buttons keep working.
- **Bare hands:** pinch is the trigger, a closed fist is the grip, and hand motion is measured from tracking, so the
  gravity gloves and throwing work.
- Play no longer waits for two Sense controllers. With Sense controllers it plays exactly as Glovebox does (a Sense
  controller always wins its hand).

The input was offered to Glovebox in [glovebox-vr/glovebox#11](https://github.com/glovebox-vr/glovebox/issues/11);
until upstream decides, it lives here.

To keep it apart from official Glovebox, this build has its own name, its own bundle identifiers and folders, no
automatic updates (official updates can't replace it), and it sends nothing to Glovebox's servers (no stutter
reports, no news sign-up).

**No game files are included.** As with Glovebox, the game comes from your own Steam account and is converted on your
own Mac, signed with your own Apple developer account, for your own headset.

**Other VeRantyx versions:** [`main`](https://github.com/Ag3497120/VeRantyx) and
[`native`](https://github.com/Ag3497120/VeRantyx/tree/native) run the game on the Mac, which renders it and streams
it to the headset (also with Joy-Con and hands).

---

*The rest is Glovebox's README, adjusted where it describes official downloads and updates.*

## You need

- **Apple Vision Pro** on visionOS 26, with about 75 GB free (tested on M5 and M2)
- **PlayStation VR2 Sense controllers**, **Nintendo Switch Joy-Con** (one in each hand), or just your hands
- **Half-Life: Alyx** in your Steam library
- **A Mac with Apple silicon**, with Xcode

## Install

There is no download of this build: build it from a checkout of this branch. From Terminal (below), or build the Mac
app yourself (`installer/README.md`, with `GLOVEBOX_SRC` set to this checkout), which then runs as
"Mitten" and puts "Mitten: HLA" on your headset. Neither updates itself: pull this branch and build
again.

<details open>
<summary>From Terminal</summary>

`build` needs the runtime kit and your game files. From a checkout, build the kit once with
`brew install cmake && zsh engine/kit/build-kit.sh glovebox-kit` (it lands where `build` looks). The game files come
from the Mac app's download, or from `GLOVEBOX_GAME` pointing at a copy of the game's `game/` folder. This build keeps
its work in `~/Library/Application Support/Mitten/alyx` (`GLOVEBOX_WORK` to use another folder).

```sh
xcrun python3 engine/glovebox.py doctor    # what this Mac and headset still need
xcrun python3 engine/glovebox.py build     # the game app, from your own game files, signed for your team
xcrun python3 engine/glovebox.py install   # onto your paired Vision Pro
```
</details>

## How it works

Steam's copy of Half-Life: Alyx includes a native 64-bit ARM Linux build. [Klepton](https://github.com/shinyquagsire23/Klepton)
converts its libraries into signed Vision Pro code on your Mac, and runs the game against Apple's frameworks:
Vulkan through MoltenVK, the displays through Compositor Services, the Sense controllers through Game Controller
and ARKit. The game's graphics pipelines are rebuilt from your own copy and compiled ahead of time, so shader
compiling stays out of play. The whole story: [How it works](docs/HOW-IT-WORKS.md).

Questions: [FAQ](docs/FAQ.md) · Problems: [Known issues](docs/KNOWN-ISSUES.md)

## Credits

Built on **[Klepton](https://github.com/shinyquagsire23/Klepton) by Max Thomas** (MIT), and on open-source work
from Valve, the Khronos Group, the SDL and LuaJIT projects and Debian. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Not affiliated with Valve or Apple

(Glovebox's own statement. Mitten is likewise free, non-commercial and contains no game files.)

Glovebox is an open-source fan project, not affiliated with or endorsed by Valve or Apple. You need your own copy of the
game. Glovebox never hosts, shares or sells game files, and never charges money. [Legal](docs/LEGAL.md)

**Valve:** we'd love to do this properly with you.
