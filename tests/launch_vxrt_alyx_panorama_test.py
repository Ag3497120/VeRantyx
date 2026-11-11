#!/usr/bin/env python3
"""Verify Panorama video directory restoration in launch_vxrt_alyx.sh and vxrt_steam.py."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import vxrt_steam  # noqa: E402


class LaunchVxrtAlyxPanoramaTest(unittest.TestCase):
    def extract_snippet(self) -> str:
        source = (REPO / "scripts/launch_vxrt_alyx.sh").read_text()
        start = source.index('ALYX_PANORAMA="$ALYX_STEAM/steamapps/common/Half-Life Alyx/game/hlvr/panorama"')
        end = source.index("# Poses at the display's rate", start)
        return source[start:end]

    def run_snippet(self, alyx_steam: Path, overlay: Path) -> None:
        snippet = self.extract_snippet()
        subprocess.run(
            ["bash", "-euo", "pipefail", "-c", snippet],
            env=dict(os.environ, ALYX_STEAM=str(alyx_steam), VXRT_VR_OVERLAY=str(overlay)),
            check=True,
        )

    def test_overlay_exposes_videos_bak_without_modifying_steam_library(self):
        with tempfile.TemporaryDirectory(prefix="alyx panorama ") as tmp:
            root = Path(tmp)
            steam = root / "vxrt_steam"
            overlay = root / "vxrt_steam_overlay"
            bak = steam / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos.bak"
            bak.mkdir(parents=True)
            (bak / "intro_world_heist.webm").write_bytes(b"webm")

            self.run_snippet(steam, overlay)

            steam_videos = steam / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos"
            ov_videos = overlay / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos"
            self.assertFalse(steam_videos.exists())
            self.assertTrue(ov_videos.is_symlink())
            self.assertEqual((ov_videos / "intro_world_heist.webm").read_bytes(), b"webm")

            # If the Steam library moves, the overlay symlink updates cleanly.
            steam2 = root / "vxrt_steam_moved"
            bak2 = steam2 / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos.bak"
            bak2.mkdir(parents=True)
            (bak2 / "intro_world_heist.webm").write_bytes(b"webm2")
            self.run_snippet(steam2, overlay)
            self.assertEqual((ov_videos / "intro_world_heist.webm").read_bytes(), b"webm2")

            # If Steam later restores panorama/videos as a real directory, the stale overlay symlink is removed.
            real_videos = steam2 / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos"
            real_videos.mkdir(parents=True)
            (real_videos / "intro_world_heist.webm").write_bytes(b"verified")
            self.run_snippet(steam2, overlay)
            self.assertFalse(ov_videos.is_symlink())
            self.assertFalse(ov_videos.exists())

    def test_vxrt_steam_import_restores_videos_bak_symlink(self):
        with tempfile.TemporaryDirectory(prefix="vxrt steam import ") as tmp:
            root = Path(tmp)
            src = root / "source_steam"
            dst = root / "dest_steam"
            bak = src / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos.bak"
            bak.mkdir(parents=True)
            (bak / "intro_world_heist.webm").write_bytes(b"webm")
            acf = src / "steamapps/appmanifest_546560.acf"
            acf.write_text('"AppState"\n{\n\t"appid"\t"546560"\n\t"name"\t"Half-Life: Alyx"\n\t"installdir"\t"Half-Life Alyx"\n}\n')

            old_steam = vxrt_steam.STEAM
            try:
                vxrt_steam.STEAM = str(dst)
                vxrt_steam.import_apps(str(src), ["546560"])
            finally:
                vxrt_steam.STEAM = old_steam

            src_videos = src / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos"
            dst_videos = dst / "steamapps/common/Half-Life Alyx/game/hlvr/panorama/videos"
            self.assertFalse(src_videos.exists())
            self.assertTrue(dst_videos.is_symlink())
            self.assertEqual(os.readlink(dst_videos), "videos.bak")
            self.assertEqual((dst_videos / "intro_world_heist.webm").read_bytes(), b"webm")


if __name__ == "__main__":
    unittest.main()
