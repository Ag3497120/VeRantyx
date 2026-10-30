#!/usr/bin/env python3
"""Loading states and real HEVC heartbeats with no game frames or headset."""
import json
import mmap
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
BUILD = REPO / "build"
ENCODER = BUILD / "HardwareEncoder"
HELPER = BUILD / "vxrt-helper"


def environment(**values):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("VERANTYX_", "VXRT_"))}
    return dict(env, **{k: str(v) for k, v in values.items()})


class LoadingProgressTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        BUILD.mkdir(exist_ok=True)
        for source, output in [(REPO / "MacEncoder/HardwareEncoder.swift", ENCODER),
                               (REPO / "scripts/vxrt_helper.swift", HELPER)]:
            if not output.exists() or source.stat().st_mtime > output.stat().st_mtime:
                subprocess.run(["swiftc", "-O", str(source), "-o", str(output)], check=True, cwd=REPO, timeout=180)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="loading test ", dir=BUILD)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.state = self.root / "game-state.json"
        self.progress = self.root / "shader-progress.json"

    def write_state(self, phase):
        self.state.write_text(json.dumps(dict(version=1, phase=phase, message=None)))

    def write_progress(self, pending=0, pipelines=0, age=0, activity_age=2, pid=None, libraries_completed=10):
        now = time.time()
        counts = lambda p, completed=10: dict(requested=completed + p, completed=completed, pending=p, failed=0)
        data = dict(version=1, pid=os.getpid() if pid is None else pid, updated_at=now-age,
                    last_activity_at=now-activity_age, libraries=counts(pending, libraries_completed), pipelines=counts(pipelines))
        self.progress.write_text(json.dumps(data))
        return data

    def render(self, expected, language="en", preview=None, frame_age=None, uptime=2, gameplay=False, tangents=None):
        out = preview or self.root / "frame.png"
        env = environment(
            VERANTYX_LOADING_SELFTEST=out, VERANTYX_LOADING_SELFTEST_EXPECT=expected,
            VERANTYX_LOADING_SELFTEST_LANGUAGE=language, VERANTYX_PROMPT_SELFTEST_SIZE="1280x720",
            VERANTYX_LOADING_SELFTEST_UPTIME=uptime, VERANTYX_GAME_STATE_FILE=self.state,
            VERANTYX_SHADER_PROGRESS_FILE=self.progress)
        if frame_age is not None:
            env["VERANTYX_LOADING_SELFTEST_GAME_FRAME_AGE"] = str(frame_age)
        if gameplay:
            env["VERANTYX_LOADING_SELFTEST_GAMEPLAY"] = "1"
        if tangents is not None:
            env["VERANTYX_PROMPT_SELFTEST_TANGENTS"] = tangents
        result = subprocess.run([str(ENCODER)], env=env,
            capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        if expected != "(none)":
            self.assertEqual(struct.unpack(">II", out.read_bytes()[16:24]), (1280, 720))

    def test_trigger_requires_explicit_wait_and_idle_compiler(self):
        self.write_progress()
        for phase, expected in [("loading", "Loading game\nPlease wait"), ("running", "(none)"),
                                ("gone", "(none)"), ("waiting", "Hold the trigger to start")]:
            with self.subTest(phase=phase):
                self.write_state(phase)
                self.render(expected)

    def test_compilation_overrides_trigger_and_pipeline_work_counts(self):
        self.write_state("waiting")
        for libraries, pipelines in [(3, 0), (0, 3), (1, 2)]:
            with self.subTest(libraries=libraries, pipelines=pipelines):
                self.write_progress(pending=libraries, pipelines=pipelines)
                self.render(f"Preparing graphics\n10/{10 + libraries}")
        self.write_progress(activity_age=0)
        self.render("Preparing graphics\n10/10")

    def test_background_compilation_does_not_cover_a_running_game(self):
        self.write_state("running")
        self.write_progress(pending=3)
        self.render("(none)", frame_age=0)
        self.render("(none)", frame_age=1)
        self.render("Preparing graphics\n10/13", frame_age=1.6)
        self.render("(none)", uptime=0.5)
        self.render("(none)", gameplay=True)

    def test_activity_animation_and_compilation_counts_update(self):
        self.write_state("waiting")
        self.write_progress(pending=3)
        self.render("Preparing graphics\n10/13", uptime=2)
        first = (self.root / "frame.png").read_bytes()
        self.render("Preparing graphics\n10/13", uptime=2.5)
        self.assertNotEqual(first, (self.root / "frame.png").read_bytes())
        self.write_progress(pending=2, libraries_completed=11)
        self.render("Preparing graphics\n11/13", uptime=2.5)

    def test_unknown_stale_dead_and_invalid_counters_cannot_enable_trigger(self):
        self.write_state("waiting")
        self.render("Loading game\nPlease wait")
        for changes in [dict(age=10, activity_age=12), dict(pid=2147483647)]:
            self.write_progress(**changes)
            self.render("Loading game\nPlease wait")
        data = self.write_progress()
        data["libraries"]["completed"] = 11
        self.progress.write_text(json.dumps(data))
        self.render("Loading game\nPlease wait")
        self.progress.write_text("{truncated")
        self.render("Loading game\nPlease wait")

    def test_russian_progress_preview(self):
        self.write_state("waiting")
        self.write_progress(pending=2551, libraries_completed=1593)
        self.render("Подготовка графики\n1593/4144", "ru", BUILD / "loading-progress-preview.png")
        self.render("Preparing graphics\n1593/4144", preview=BUILD / "loading-progress-preview-en.png")
        self.render("グラフィックスを準備中\n1593/4144", "ja", BUILD / "loading-progress-preview-ja.png")
        self.write_progress()
        self.render("Нажмите и удерживайте триггер", "ru", BUILD / "trigger-prompt-preview.png")
        self.render("Hold the trigger to start", preview=BUILD / "trigger-prompt-preview-en.png")

    def test_asymmetric_eyes_keep_the_notice_in_the_shared_field(self):
        self.write_state("waiting")
        self.write_progress(pending=3)
        self.render("Preparing graphics\n10/13", tangents="-0.7 1.3 -1.05 1.1 -1.3 0.7 -1.05 1.1",
                    preview=BUILD / "loading-progress-preview-asymmetric.png")

    def test_helper_clears_pause_on_continue_new_load_and_disconnect(self):
        prompt = self.root / "prompt.txt"
        child = subprocess.Popen([str(HELPER), "webui-replay", str(prompt), str(self.state)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            for message, phase in [('{"type":"begin_loading"}', "loading"),
                                   ('{"type":"show_message","text":"Press Trigger To Start"}', "waiting"),
                                   ('{"type":"end_loading"}', "running"),
                                   ('{"type":"show_message","text":"Saving..."}', "running"),
                                   ('{"type":"show_message","text":""}', "running"),
                                   ('{"type":"begin_loading"}', "loading"),
                                   ('{"type":"show_message","text":"Saving..."}', "loading"),
                                   ("gone", "gone")]:
                child.stdin.write(message + "\n")
                child.stdin.flush()
                self.assertTrue(child.stdout.readline())
                self.assertEqual(json.loads(self.state.read_text())["phase"], phase)
        finally:
            child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0)

    def test_compiler_children_do_not_overwrite_game_status(self):
        binary = REPO / "VXRuntime/build/shader_progress_test"
        self.progress.write_text("sentinel")
        result = subprocess.run([str(binary), "vrmonitor.exe"], env=environment(
            VXRT_SHADER_PROGRESS_FILE=self.progress, VXRT_SHADER_PROGRESS_EXE="hlvr.exe"),
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.progress.read_text(), "sentinel")
        result = subprocess.run([str(binary), "hlvr.exe"], env=environment(
            VXRT_SHADER_PROGRESS_FILE=self.progress, VXRT_SHADER_PROGRESS_EXE="hlvr.exe"),
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(self.progress.read_text())
        self.assertEqual(data["libraries"]["requested"], 1003)
        self.assertEqual(data["libraries"]["pending"], 0)
        self.assertEqual(data["unique_sources"], 2)

    def test_windows_exit_flushes_real_metal_counts(self):
        runtime = REPO / "VXRuntime/build/vxrt"
        draw = REPO / "VXRuntime/build/draw.exe"
        result = subprocess.run([str(runtime), str(draw)], env=environment(
            VXRT_ROOT=self.root / "windows", VXRT_NO_ACTIVATE=1,
            VXRT_SHADER_CACHE_DIR=self.root / "shaders",
            VXRT_SHADER_PROGRESS_FILE=self.progress, VXRT_SHADER_PROGRESS_EXE="draw.exe"),
            cwd=REPO / "VXRuntime", capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(self.progress.read_text())
        self.assertGreater(data["libraries"]["requested"], 0)
        self.assertGreater(data["pipelines"]["completed"], 0)
        self.assertEqual(data["libraries"]["pending"], 0)
        self.assertGreater(data["unique_sources"], 0)

    def test_encoded_loading_frames_without_a_single_game_frame(self):
        probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        try:
            probe.bind(("::", 9999))
        except OSError:
            self.skipTest("encoder port 9999 is in use")
        finally:
            probe.close()
        frame = self.root / "shared-frame.dat"
        slot_size = 88 + 4096 * 4096 * 8
        with frame.open("wb") as f:
            f.truncate(64 + 2 * slot_size + 312 + 1024)
            f.seek(0)
            f.write(struct.pack("<I", 2))  # Neither game buffer has been published.
            f.seek(64 + 2 * slot_size)
            f.write(struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1))
            f.seek(64 + 2 * slot_size + 244)
            f.write(struct.pack("<d", 123.456))
        self.write_state("waiting")
        self.write_progress(pending=3)
        log_path = self.root / "encoder.log"
        with log_path.open("w") as log:
            child = subprocess.Popen([str(ENCODER)], stdout=log, stderr=subprocess.STDOUT, env=environment(
                VERANTYX_SHARED_FRAME=frame, VERANTYX_GAME_STATE_FILE=self.state,
                VERANTYX_SHADER_PROGRESS_FILE=self.progress, VERANTYX_AUDIO=0, VERANTYX_ALVR_AUDIO=0,
                VERANTYX_STREAM_WIDTH=1280, VERANTYX_STREAM_HEIGHT=720, VERANTYX_STREAM_FIXED_SIZE=1))
            sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            sock.settimeout(0.15)
            chunks, complete, capture_rates = {}, set(), []
            deadline, hello = time.monotonic() + 8, 0
            try:
                while time.monotonic() < deadline and (len(complete) < 3 or not capture_rates):
                    self.assertIsNone(child.poll(), log_path.read_text())
                    if time.monotonic() - hello > 0.5:
                        sock.sendto(b"HELLO_VR", ("::1", 9999))
                        self.write_progress(pending=3)
                        hello = time.monotonic()
                    try:
                        packet, _ = sock.recvfrom(65536)
                    except socket.timeout:
                        continue
                    if packet[:4] == b"STAT":
                        capture_rates.append(struct.unpack_from("<f", packet, 4)[0])
                    if len(packet) >= 96 and packet[:4] == b"NARV":
                        seq, index, total = struct.unpack_from("<III", packet, 4)
                        chunks.setdefault(seq, set()).add(index)
                        if len(chunks[seq]) == total:
                            complete.add(seq)
                self.assertGreaterEqual(len(complete), 3, log_path.read_text())
                self.assertTrue(capture_rates, log_path.read_text())
                self.assertEqual(set(capture_rates), {0.0})
                self.assertIn("Queued loading frame", log_path.read_text())
                # Continuing must clear the old overlay even if the first game frame is still unavailable.
                self.write_progress()
                self.write_state("running")
                clear_deadline = time.monotonic() + 2
                while time.monotonic() < clear_deadline and "Queued loading clear frame, status=0" not in log_path.read_text():
                    time.sleep(0.02)
                self.assertIn("Queued loading clear frame, status=0", log_path.read_text())
                # Real game submissions must hide background compilation; generated frames must not.
                with frame.open("r+b") as shared, mmap.mmap(shared.fileno(), 0) as memory:
                    sequence = 0

                    def publish_game_frame():
                        nonlocal sequence
                        sequence += 1
                        index = sequence % 2
                        at = 64 + index * slot_size
                        pose = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
                        memory[at:at + 88] = struct.pack("<4I16fd", sequence, 1280, 720, 87, *pose, 123.456)
                        struct.pack_into("<I", memory, 16, 2)
                        struct.pack_into("<I", memory, 0, index)

                    self.write_progress(pending=3)
                    flowing_until = time.monotonic() + 1.2
                    while time.monotonic() < flowing_until:
                        publish_game_frame()
                        self.write_progress(pending=3)
                        time.sleep(0.05)
                    shown = log_path.read_text().count("Game prompt drawn into the picture:")
                    short_hitch_until = time.monotonic() + 0.8
                    while time.monotonic() < short_hitch_until:
                        self.write_progress(pending=3)
                        time.sleep(0.05)
                    self.assertEqual(log_path.read_text().count("Game prompt drawn into the picture:"), shown)
                    stall_deadline = time.monotonic() + 2
                    while time.monotonic() < stall_deadline and log_path.read_text().count("Game prompt drawn into the picture:") == shown:
                        self.write_progress(pending=3)
                        time.sleep(0.05)
                    self.assertGreater(log_path.read_text().count("Game prompt drawn into the picture:"), shown)
                    hidden = log_path.read_text().count("Game prompt taken away")
                    publish_game_frame()
                    resume_deadline = time.monotonic() + 0.5
                    while time.monotonic() < resume_deadline and log_path.read_text().count("Game prompt taken away") == hidden:
                        time.sleep(0.01)
                    self.assertGreater(log_path.read_text().count("Game prompt taken away"), hidden)
            finally:
                sock.close()
                child.terminate()
                child.wait(timeout=5)
        with frame.open("rb") as f:
            f.seek(64)
            self.assertGreater(struct.unpack("<I", f.read(4))[0], 0)
            f.seek(64 + slot_size)
            self.assertGreater(struct.unpack("<I", f.read(4))[0], 0)


if __name__ == "__main__":
    unittest.main()
