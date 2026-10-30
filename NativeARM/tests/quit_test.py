#!/usr/bin/env python3
"""Offline shutdown regressions: only synthetic processes and private repo files."""
from contextlib import contextmanager, redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE/'tools'))
from native_quit import stop_signals, wait_for_game
import render_probe

PEER = r'''
import os
from pathlib import Path
import signal
import time
root = Path(os.environ['NATIVE_QUIT_TEST_ROOT'])
mode = os.environ['NATIVE_QUIT_TEST_MODE']
request = Path(os.environ['VERANTYX_NATIVE_QUIT_FILE'])
def received(number, frame):
    with (root/'signals').open('a') as output: output.write(str(number)+'\n')
for number in (signal.SIGINT, signal.SIGTERM): signal.signal(number, received)
(root/'ready').touch()
print('synthetic host ready', flush=True)
if mode == 'natural': raise SystemExit(3)
deadline = time.monotonic()+4
while not (root/'release').exists() and time.monotonic() < deadline:
    if mode == 'slow' and request.exists():
        Path(str(request)+'.delivered').write_text('1\n')
        Path(str(request)+'.ack').write_text('1\n')
        time.sleep(.35)
        break
    time.sleep(.01)
(root/'saved').touch()
print('synthetic host exited normally', flush=True)
'''


class QuitTests(unittest.TestCase):
    def setUp(self):
        (NATIVE/'build/tests').mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix='quit-', dir=NATIVE/'build/tests')
        self.root = Path(self.directory.name)
        self.peer = self.root/'peer.py'
        self.peer.write_text('#!'+sys.executable+'\n'+PEER)
        self.peer.chmod(0o755)

    def tearDown(self):
        self.directory.cleanup()

    def environment(self, root, mode):
        return dict(os.environ, NATIVE_QUIT_TEST_ROOT=str(root), NATIVE_QUIT_TEST_MODE=mode,
                    VERANTYX_NATIVE_QUIT_FILE=str(root/'quit.request'))

    @contextmanager
    def running_peer(self, mode, name='host'):
        root = self.root/name
        root.mkdir()
        process = subprocess.Popen([str(self.peer)], env=self.environment(root, mode), start_new_session=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic()+2
            while not (root/'ready').exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((root/'ready').exists())
            yield process, root
        finally:
            (root/'release').touch()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()

    def test_natural_exit(self):
        with self.running_peer('natural') as (process, root):
            process.wait(timeout=2)
            result = wait_for_game(process, root/'quit.request', .1)
            self.assertEqual((result['method'], result['exit_code']), ('natural', 3))
            self.assertFalse((root/'quit.request').exists())

    def test_slow_quit_exceeds_timeout_without_a_signal(self):
        with self.running_peer('slow') as (process, root):
            states = []
            result = wait_for_game(process, root/'quit.request', .1, progress=states.append)
            self.assertEqual((result['method'], result['exit_code']), ('requested', 0))
            self.assertTrue(result['timed_out'] and result['delivered'] and result['acknowledged'])
            self.assertTrue(any(s['timed_out'] and 'exit_code' not in s for s in states))
            self.assertTrue((root/'saved').exists())
            self.assertFalse((root/'signals').exists())

    def test_repeated_cancellation_does_not_escalate(self):
        with self.running_peer('ignore') as (process, root), stop_signals() as stopped:
            alive_at_timeout = []
            def progress(state):
                if state['timed_out'] and not stopped:
                    alive_at_timeout.append(process.poll() is None)
                    signal.raise_signal(signal.SIGINT)
                    signal.raise_signal(signal.SIGTERM)
                    (root/'release').touch()
            result = wait_for_game(process, root/'quit.request', .1, progress=progress)
            self.assertEqual(stopped, [signal.SIGINT])
            self.assertEqual(alive_at_timeout, [True])
            self.assertEqual((result['method'], result['exit_code']), ('requested', 0))
            self.assertFalse((root/'signals').exists())

    def test_explicit_force_is_scoped_to_owned_group(self):
        with self.running_peer('ignore') as (process, root), self.running_peer('ignore', 'unrelated') as (other, _):
            result = wait_for_game(process, root/'quit.request', .1, allow_force_kill=True)
            self.assertEqual((result['method'], result['exit_code']), ('forced', -signal.SIGKILL))
            self.assertIsNone(other.poll())
            self.assertFalse((root/'saved').exists())

    def test_failed_progress_write_keeps_waiting_for_cleanup(self):
        with self.running_peer('slow') as (process, root):
            def failure(_state): raise OSError('synthetic disk error')
            result = wait_for_game(process, root/'quit.request', .1, progress=failure)
            self.assertEqual(result['exit_code'], 0)
            self.assertTrue((root/'saved').exists())
            self.assertIn('synthetic disk error', result['report_error'])

    def test_timeout_wrapper_defaults_to_normal_quit(self):
        result = subprocess.run(['bash', str(NATIVE/'scripts/run_timeout.sh'), '--quit-timeout', '.1', '.1', str(self.peer)],
                                env=self.environment(self.root, 'slow'), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertIn('"method": "requested"', result.stderr)
        self.assertTrue((self.root/'saved').exists())
        self.assertFalse((self.root/'signals').exists())

    def test_timeout_wrapper_cancellation(self):
        process = subprocess.Popen(['bash', str(NATIVE/'scripts/run_timeout.sh'), '--quit-timeout', '.1', '10', str(self.peer)],
                                   env=self.environment(self.root, 'slow'), start_new_session=True,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic()+2
            while not (self.root/'ready').exists() and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue((self.root/'ready').exists())
            process.send_signal(signal.SIGTERM)
            _, error = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143, error)
            self.assertIn('"method": "requested"', error)
            self.assertFalse((self.root/'signals').exists())
        finally:
            (self.root/'release').touch()
            process.wait(timeout=5)
            process.stderr.close()

    def test_timeout_wrapper_force_requires_the_flag(self):
        result = subprocess.run(['bash', str(NATIVE/'scripts/run_timeout.sh'), '--quit-timeout', '.1',
                                 '--allow-force-kill', '.1', str(self.peer)],
                                env=self.environment(self.root, 'ignore'), capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 124, result.stderr)
        self.assertIn('"method": "forced"', result.stderr)
        self.assertFalse((self.root/'saved').exists())

    def probe(self, interrupted=False, broken=False):
        installation = self.root/'installation'
        (installation/'lib').mkdir(parents=True)
        (installation/'lib/libengine2.so').touch()
        (installation/'game-root/game/hlvr/cfg').mkdir(parents=True)
        (self.root/'runs').mkdir()
        check_output, run = subprocess.check_output, subprocess.run
        def output(command, *args, **kwargs):
            return '' if command[0] == 'ps' else check_output(command, *args, **kwargs)
        def checked_run(command, *args, **kwargs):
            return subprocess.CompletedProcess(command, 0) if command[0] == 'pgrep' else run(command, *args, **kwargs)
        argv = ['render_probe.py', '--installation', str(installation), '--host', str(self.peer),
                '--seconds', '2' if interrupted or broken else '0', '--quit-timeout', '.1']
        sealed = []
        pending_resources = []
        wait = render_probe.wait_for_game
        def observed_wait(process, request, timeout, allow_force_kill, progress, tick):
            def checkpoint(state):
                progress(state)
                if state['timed_out'] and 'exit_code' not in state:
                    pending_resources.append(process.poll() is None and (request.parent/'overlay').is_dir() and
                        (request.parent/'frame.dat').exists() and (request.parent/'frames.json').exists() and
                        (request.parent/'pending.json').exists() and not sealed)
            return wait(process, request, timeout, allow_force_kill, progress=checkpoint, tick=tick)
        cancel = None
        with patch.object(render_probe, 'REPO', self.root), patch.object(sys, 'argv', argv), \
             patch.object(subprocess, 'check_output', side_effect=output), patch.object(subprocess, 'run', side_effect=checked_run), \
             patch.object(render_probe, 'wait_for_game', side_effect=observed_wait), \
             patch.dict(os.environ, self.environment(self.root, 'slow')), \
             patch.dict(sys.modules, {'vxrt_ledger': SimpleNamespace(append=sealed.append)}), redirect_stdout(io.StringIO()):
            if interrupted:
                def interrupt():
                    deadline = time.monotonic()+2
                    while not (self.root/'ready').exists() and time.monotonic() < deadline: time.sleep(.01)
                    if (self.root/'ready').exists(): os.kill(os.getpid(), signal.SIGINT)
                cancel = threading.Thread(target=interrupt)
                cancel.start()
            try:
                if broken:
                    with patch.object(render_probe.math, 'cos', side_effect=RuntimeError('synthetic collection error')):
                        status = render_probe.main()
                else:
                    status = render_probe.main()
            finally:
                if cancel is not None: cancel.join(timeout=3)
        path = next((self.root/'runs').iterdir())
        result = json.loads((path/'result.json').read_text())
        self.assertEqual(len(sealed), 1)
        self.assertEqual(sealed[0]['result_sha256'], hashlib.sha256((path/'result.json').read_bytes()).hexdigest())
        for observation in result['observed']:
            artifact = self.root/observation['artifact']
            self.assertEqual(observation['sha256'], hashlib.sha256(artifact.read_bytes()).hexdigest())
        self.assertFalse((path/'overlay').exists())
        self.assertFalse((path/'pending.json').exists())
        self.assertTrue((self.root/'saved').exists())
        self.assertFalse((self.root/'signals').exists())
        self.assertEqual(result['shutdown']['method'], 'requested')
        self.assertTrue(result['shutdown']['timed_out'])
        self.assertTrue(pending_resources and all(pending_resources))
        return status, result

    def test_probe_seals_after_normal_quit_and_separates_measurement(self):
        status, result = self.probe()
        self.assertEqual((status, result['status']), (0, 'recorded'))
        self.assertGreater(result['total_duration_s']-result['duration_s'], .3)

    def test_probe_cancellation_retains_evidence(self):
        status, result = self.probe(interrupted=True)
        self.assertEqual((status, result['status'], result['stop_signal']), (130, 'interrupted', signal.SIGINT))

    def test_probe_collection_failure_still_quits_normally(self):
        status, result = self.probe(broken=True)
        self.assertEqual((status, result['status']), (1, 'error'))
        self.assertIn('synthetic collection error', result['collection_error'])


if __name__ == '__main__':
    unittest.main()
