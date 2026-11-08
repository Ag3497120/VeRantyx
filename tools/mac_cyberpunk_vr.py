#!/usr/bin/env python3
"""
Prepare a lightweight runnable macOS Cyberpunk 2077 wrapper bundle (with symlinked Data)
and VR-optimized UserSettings.json for VeRantyx MacVR streaming to Apple Vision Pro.
Never modifies the original Mac App Store / GOG / Steam application bundle.
"""

import argparse
import json
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_SOURCE_CANDIDATES = [
    "/Applications/Cyberpunk2077.app",
    "/Applications/Cyberpunk 2077.app",
    "/Volumes/PREDATOR GM7000 4TB/Applications/Cyberpunk2077.app",
    "/Volumes/PREDATOR GM7000 4TB/Applications/Cyberpunk 2077.app",
]

DEFAULT_WRAPPER_CANDIDATES = [
    os.path.expanduser("~/Verantyx_VR_Drive/avp_mac/macstream/Cyberpunk2077MS.app"),
    os.path.expanduser("~/Verantyx_VR_Drive/mac_vr/Cyberpunk2077MS.app"),
]


def find_source_app(explicit_path: str | None = None) -> Path | None:
    if explicit_path:
        p = Path(explicit_path).expanduser()
        if (p / "Contents" / "MacOS" / "Cyberpunk2077").exists():
            return p
    for cand in DEFAULT_SOURCE_CANDIDATES:
        p = Path(cand).expanduser()
        if (p / "Contents" / "MacOS" / "Cyberpunk2077").exists():
            return p
    return None


def find_existing_wrapper(explicit_wrapper: str | None = None) -> Path | None:
    if explicit_wrapper:
        p = Path(explicit_wrapper).expanduser()
        if (p / "Contents" / "MacOS" / "Cyberpunk2077").exists() and (p / "Contents" / "Data").exists():
            return p
    for cand in DEFAULT_WRAPPER_CANDIDATES:
        p = Path(cand).expanduser()
        if (p / "Contents" / "MacOS" / "Cyberpunk2077").exists() and (p / "Contents" / "Data").exists():
            return p
    return None


def prepare_wrapper_bundle(source_app: Path, wrapper_app: Path) -> Path:
    """Create or update a lightweight wrapper bundle with Contents/Data symlinked."""
    exe = wrapper_app / "Contents" / "MacOS" / "Cyberpunk2077"
    data_link = wrapper_app / "Contents" / "Data"
    if exe.exists() and data_link.exists():
        return wrapper_app

    print(f"[MacVR] Preparing lightweight wrapper bundle at: {wrapper_app}", file=sys.stderr)
    contents = wrapper_app / "Contents"
    contents.mkdir(parents=True, exist_ok=True)

    src_contents = source_app / "Contents"
    for item in src_contents.iterdir():
        dst = contents / item.name
        if item.name == "Data":
            if dst.exists() or dst.is_symlink():
                if dst.is_symlink():
                    dst.unlink()
            dst.symlink_to(item.resolve())
        elif item.name == "_CodeSignature" or item.name == "embedded.provisionprofile":
            continue
        else:
            if dst.exists():
                if dst.is_dir() and not dst.is_symlink():
                    shutil.rmtree(dst)
                else:
                    dst.unlink()
            if item.is_dir():
                shutil.copytree(item, dst, symlinks=True)
            else:
                shutil.copy2(item, dst)

    # Ad-hoc sign frameworks and main executable so DYLD_INSERT_LIBRARIES is permitted on macOS
    fw_dir = contents / "Frameworks"
    if fw_dir.exists():
        for dylib in fw_dir.glob("*.dylib"):
            subprocess.run(["codesign", "-s", "-", "-f", str(dylib)], check=True)
    subprocess.run(["codesign", "--remove-signature", str(exe)], check=False)
    subprocess.run(["codesign", "-s", "-", "-f", str(wrapper_app)], check=True)
    return wrapper_app


# VR-friendly settings adjustments (referenced from VXpro/tools/cp_settings.py)
VR_SETTINGS_OVERRIDES = {
    ("/accessibility/hud", "HudSafezoneHor"): 0.0,
    ("/accessibility/hud", "HudSafezoneVer"): 0.0,
    ("/accessibility/interface", "SubtitleFontSize"): 36,
    ("/gameplay/misc", "CameraHeadBobbing"): 0.0,
    ("/graphics/basic", "ChromaticAberration"): False,
    ("/graphics/basic", "DepthOfField"): False,
    ("/graphics/basic", "FilmGrain"): False,
    ("/graphics/basic", "LensFlares"): False,
    ("/graphics/basic", "MotionBlur"): "Off",
    ("/graphics/basic", "FieldOfView"): 100.0,
    ("/graphics/performance", "CrowdDensity"): "Low",
    ("/graphics/advanced", "VolumetricFogResolution"): "Medium",
    ("/graphics/advanced", "VolumetricCloudsQuality"): "Medium",
    ("/graphics/advanced", "DistantShadowsResolution"): "Low",
    ("/graphics/advanced", "ScreenSpaceReflectionQuality"): "Low",
    ("/graphics/advanced", "SubsurfaceScatteringQuality"): "Low",
    ("/graphics/advanced", "AmbientOcclusion"): "Low",
    ("/graphics/advanced", "ColorPrecision"): "Medium",
    ("/graphics/advanced", "MirrorQuality"): "Low",
    ("/graphics/raytracing", "RayTracing"): False,
    ("/graphics/raytracing", "RayTracedReflections"): False,
    ("/graphics/raytracing", "RayTracedSunShadows"): False,
    ("/graphics/raytracing", "RayTracedLocalShadows"): False,
    ("/graphics/raytracing", "RayTracedLighting"): "Off",
    ("/graphics/raytracing", "RayTracedPathTracing"): False,
    ("/graphics/drs", "DynamicResolutionScaling"): False,
    ("/graphics/upscaling", "UpscalingType"): "MetalFX",
    ("/graphics/upscaling", "MetalFXPreset"): "Balanced",
    # Critical for Cyberpunk 2.5.0: disable FrameGeneration so every presented frame is a real camera frame
    ("/graphics/framegen", "FrameGeneration"): "Off",
    ("/video/display", "VSync"): "Off",
    ("/video/display", "WindowMode"): "Windowed",
}


def apply_vr_settings(settings_file: Path, resolution: str = "1600x900", fov: float = 100.0) -> None:
    if not settings_file.exists():
        return
    try:
        doc = json.loads(settings_file.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"[MacVR] Warning: could not parse {settings_file}: {exc}", file=sys.stderr)
        return

    overrides = dict(VR_SETTINGS_OVERRIDES)
    overrides[("/video/display", "Resolution")] = resolution
    overrides[("/graphics/basic", "FieldOfView")] = float(fov)

    changed = 0
    for group in doc.get("data", []):
        gname = group.get("group_name")
        for var in group.get("vars", []):
            key = (gname, var.get("var_name"))
            if key in overrides:
                val = overrides[key]
                if var.get("value") != val:
                    var["value"] = val
                    changed += 1

    if changed > 0:
        settings_file.write_text(json.dumps(doc, indent=4) + "\n", encoding="utf-8")
        print(f"[MacVR] Updated {changed} VR setting(s) in {settings_file} (res={resolution}, fov={fov})", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description="Prepare macOS Cyberpunk 2077 for VeRantyx VR streaming")
    ap.add_argument("--source", help="Path to original Cyberpunk2077.app")
    ap.add_argument("--wrapper", help="Path to wrapper Cyberpunk2077MS.app")
    ap.add_argument("--home", help="Container HOME directory whose UserSettings.json should be tuned for VR")
    ap.add_argument("--resolution", default="1600x900", help="Render resolution (e.g. 1600x900 or 1280x720)")
    ap.add_argument("--fov", type=float, default=100.0, help="Horizontal field of view in degrees (80..110)")
    ap.add_argument("--print-bundle", action="store_true", help="Print the resolved runnable bundle path to stdout")
    args = ap.parse_args()

    wrapper = find_existing_wrapper(args.wrapper)
    if wrapper is None:
        src = find_source_app(args.source)
        if src is None:
            print("[MacVR] Error: Cyberpunk2077.app not found in /Applications or external drives.", file=sys.stderr)
            return 1
        target_wrapper = Path(args.wrapper).expanduser() if args.wrapper else Path(DEFAULT_WRAPPER_CANDIDATES[1])
        wrapper = prepare_wrapper_bundle(src, target_wrapper)

    if args.home:
        home_dir = Path(args.home).expanduser()
        settings_candidates = [
            home_dir / "Library" / "Application Support" / "CD Projekt Red" / "Cyberpunk 2077" / "UserSettings.json",
            home_dir / "Library" / "Containers" / "com.cdprojektred.cyberpunk" / "Data" / "Library" / "Application Support" / "CD Projekt Red" / "Cyberpunk 2077" / "UserSettings.json",
        ]
        # Seed UserSettings.json from existing user settings if the container doesn't have one yet
        real_user_settings = Path.home() / "Library" / "Containers" / "com.cdprojektred.cyberpunk" / "Data" / "Library" / "Application Support" / "CD Projekt Red" / "Cyberpunk 2077" / "UserSettings.json"
        avp_mac_settings = Path.home() / "Verantyx_VR_Drive" / "avp_mac" / "macstream" / "home" / "Library" / "Application Support" / "CD Projekt Red" / "Cyberpunk 2077" / "UserSettings.json"

        target_settings = settings_candidates[0]
        if not target_settings.exists():
            target_settings.parent.mkdir(parents=True, exist_ok=True)
            if avp_mac_settings.exists():
                shutil.copy2(avp_mac_settings, target_settings)
            elif real_user_settings.exists():
                shutil.copy2(real_user_settings, target_settings)

        for sc in settings_candidates:
            if sc.exists():
                apply_vr_settings(sc, resolution=args.resolution, fov=args.fov)

    if args.print_bundle:
        print(str(wrapper))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
