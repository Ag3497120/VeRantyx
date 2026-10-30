#!/usr/bin/env python3
"""Exercise the launcher's encoder supervisor without SteamVR, a game or ALVR."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]


class EncoderSupervisorTest(unittest.TestCase):
    def run_encoder(self, exits):
        source = (REPO / "scripts/launch_vxrt_steamvr.sh").read_text()
        end = source.index("\nENCODER_PID=$!")
        start = source.rfind("\n(", 0, end) + 1
        supervisor = source[start:end].rstrip()
        self.assertTrue(supervisor.endswith("&"))
        (REPO / "build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="encoder supervisor ", dir=REPO / "build") as folder:
            root = Path(folder)
            encoder = root / "fake encoder"
            encoder.write_text('''#!/bin/bash
set -eu
n=0
[[ ! -f "$COUNT_FILE" ]] || read -r n < "$COUNT_FILE"
n=$((n+1))
printf '%s\\n' "$n" > "$COUNT_FILE"
printf 'frame=%s\\n' "$VERANTYX_SHARED_FRAME"
read -r -a codes <<< "$EXIT_CODES"
exit "${codes[$((n-1))]:-0}"
''')
            encoder.chmod(0o755)
            frame = str(root / "shared frame.dat")
            result = subprocess.run(
                ["bash", "-euo", "pipefail", "-c", supervisor[:-1]],
                env=dict(os.environ, ENCODER=str(encoder), SHARED_FRAME=frame,
                         LOG_DIR=str(root), COUNT_FILE=str(root / "starts"),
                         EXIT_CODES=" ".join(map(str, exits)), TMPDIR=str(root)),
                capture_output=True, text=True, timeout=5)
            starts = int((root / "starts").read_text())
            log = (root / "hardware-encoder.log").read_text().splitlines()
            self.assertEqual(log, ["frame=" + frame] * starts)
            return result.returncode, starts

    def test_restarts_after_handshake_or_display_change(self):
        self.assertEqual(self.run_encoder([75, 75, 0]), (0, 3))

    def test_normal_exit_stops(self):
        self.assertEqual(self.run_encoder([0]), (0, 1))

    def test_fatal_exit_is_preserved_without_retry(self):
        for code in [1, 7, 137]:
            with self.subTest(code=code):
                self.assertEqual(self.run_encoder([code, 0]), (code, 1))


if __name__ == "__main__":
    unittest.main()
