# Third-party sources

- VeRantyx and its native loading renderer: MIT, https://github.com/Ag3497120/VeRantyx. The SceneKit loading renderer is adapted from upstream revision 421b682.
- Klepton runtime: https://github.com/shinyquagsire23/Klepton, pinned in `klepton/COMMIT`; `working-runtime.patch` and `klepton/new/` reproduce the optional native runtime adaptations. Retain Klepton's own notices when distributing its compiled code.
- Khronos OpenXR SDK and loader-negotiation declarations: Apache-2.0 OR MIT, https://github.com/KhronosGroup/OpenXR-SDK, release-1.1.49.
- Fossilize: MIT, https://github.com/ValveSoftware/Fossilize, 556da9ff7d7616704dda515756219ab1dab693ec. Retain Fossilize and its dependency notices in a binary package.
- Glovebox recipe/readers: MIT, https://github.com/glovebox-vr/glovebox. The preparation directory includes `Glovebox-LICENSE` and `SOURCE.json`. The recipe contains no shader bytecode.
- MoltenVK: Apache-2.0, https://github.com/KhronosGroup/MoltenVK. Binary packages must retain its accompanying notices.

Valve game libraries, shader bytecode, textures, fonts and loading-room resources must come from the user's own installation and are not distributed here. NotProton is an optional external Steam integration prerequisite, not bundled software.
