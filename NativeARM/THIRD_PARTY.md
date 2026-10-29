# Native ARM64 components

Klepton: https://github.com/shinyquagsire23/Klepton, MIT, pinned to
`ce74582cb1e434db210014dbcd0ca0aad0e46ba2`. The build applies the source changes
in `klepton-patches/`. The application includes the upstream licence.
The native host excludes Klepton's optional embedded Android font and Valve
HRTF dataset; fonts and audio data come from the user's own game content.

MoltenVK: https://github.com/KhronosGroup/MoltenVK, Apache-2.0, v1.4.2.
The upstream release SHA-256 is checked by Klepton's pinned fetch script.
The application includes its licence and notices.

ALVR / OpenVR: see the application’s ALVR, OpenVR and Rust crate notices.

The application contains no Valve game content, Valve ARM64 libraries or Debian
shared libraries. Native installation fetches the user's licensed Alyx depots
through Valve's SteamCMD and the separately versioned Debian ARM64 packages in
`dependencies.json`. Each support archive is checked against its SHA-256 before
extraction; its copyright files are preserved under the native download cache's
`licenses` directory. These packages are used as guest libraries, never copied
into VeRantyx's own sources or linked into the native host. Their source packages
are available from Debian: https://www.debian.org/distrib/packages.
