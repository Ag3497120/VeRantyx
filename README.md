<img src="installer/App/Glovebox/Assets.xcassets/AppIcon.appiconset/icon_512x512.png" width="128" alt="Glovebox">

# Glovebox

**Play your Steam copy of Half-Life: Alyx on Apple Vision Pro.**
It runs on the headset itself: no PC, no streaming. Glovebox builds it on your Mac from the copy you own and
installs it on your headset.

## You need

- **Apple Vision Pro** on visionOS 26, with about 75 GB free (tested on M5 and M2)
- **PlayStation VR2 Sense controllers**
- **Half-Life: Alyx** in your Steam library
- **A Mac with Apple silicon**, with Xcode

## Install

1. **[Download the Glovebox app](https://github.com/glovebox-vr/glovebox/releases/latest/download/Glovebox.dmg)** and open it.
2. **Follow along.** It sets up Xcode, your headset and Steam, then builds the game and installs it on your
   headset. You only step in where Apple or Steam need a person, and it shows you exactly where.
3. **Put on your headset and open the game.** The rest of the game downloads straight to the headset, with your
   Steam Cloud saves.

The app keeps itself and the game up to date.

<details>
<summary>From Terminal</summary>

`build` needs the runtime kit and your game files. From a checkout, build the kit once with
`brew install cmake && zsh engine/kit/build-kit.sh glovebox-kit` (it lands where `build` looks). The game files come
from the Mac app's download, or from `GLOVEBOX_GAME` pointing at a copy of the game's `game/` folder.

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

Glovebox is an open-source fan project, not affiliated with or endorsed by Valve or Apple. You need your own copy of the
game. Glovebox never hosts, shares or sells game files, and never charges money. [Legal](docs/LEGAL.md)

**Valve:** we'd love to do this properly with you.
