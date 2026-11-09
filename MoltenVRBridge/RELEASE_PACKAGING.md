# Optional MoltenVR runtime packaging

Build with `build-release.sh`; take the optional host from `NativeARM/build/host-fossilize/`. Resolve the source-tree preparation/UI/save-tool symlinks when staging a standalone package; Fossilize is pinned and linked into the native host. The complete Klepton patch plus NativeARM refinements reproduce the tested source. Strip debug symbols and ad-hoc sign the ARM64 executables before packaging.

Allowlist: ARM64 host, native bridge and loading-renderer library, own warp SPIR-V shaders, validated MoltenVK libraries,4x fidelity table, preparation helpers and no-code Glovebox recipe, installer/UI generation, Steam integration, README/release notes and licenses.

Never include generated `.foz` files, game UI resources, game/depot libraries, saves, preferences, cache identities, shader caches, frame mappings, logs, backups, Android APKs or credentials. The installer reconstructs the recipe from the owner's VPK files and generates UI compatibility locally. The recipe checker must confirm no shader code before every package.

Validate source/runtime tests, synthetic installer updates, actual recipe reconstruction and package content/checksums. Download the published asset again and check its SHA256 after upload. Performance results from the maintainer's Mac are not universal FPS/latency claims.
