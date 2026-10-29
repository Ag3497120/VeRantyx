#!/usr/bin/env python3
"""Cooperative shutdown for an owned native host. Force killing is opt-in."""
import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


@contextmanager
def stop_signals():
    """Signals stop collection, never get forwarded to the game or escalate."""
    received = []
    previous = {}
    def receive(number, _frame):
        if not received:
            received.append(number)
    try:
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, receive)
        yield received
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def request_quit(path):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix=path.name+'.', delete=False) as output:
            temporary = Path(output.name)
            output.write('quit\n')
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def wait_for_game(process, request, timeout=60, allow_force_kill=False, progress=None, tick=None):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('quit timeout must be finite and positive')
    began = time.monotonic()
    result = dict(pid=process.pid, method='natural', timeout_s=timeout, timed_out=False,
                  force_allowed=allow_force_kill, delivered=False, acknowledged=False)
    def report():
        if progress is not None:
            try:
                progress(dict(result, wait_s=time.monotonic()-began))
            except Exception as error:
                # Losing a diagnostic file must not abandon the live game or
                # unwind a caller's overlay/map cleanup around it.
                result['report_error'] = str(error)
                print(f'Cannot save shutdown progress: {error}', file=sys.stderr, flush=True)
    if process.poll() is None:
        result['method'] = 'requested'
        try:
            request_quit(request)
        except OSError as error:
            result['request_error'] = str(error)
            print(f'Cannot request a normal quit: {error}. Waiting without sending a signal.', file=sys.stderr, flush=True)
        report()
        while process.poll() is None:
            changed = False
            for key, suffix in (('delivered', '.delivered'), ('acknowledged', '.ack')):
                value = Path(str(request)+suffix).is_file()
                if result[key] != value:
                    result[key] = value
                    changed = True
            if not result['timed_out'] and time.monotonic()-began >= timeout:
                result['timed_out'] = True
                changed = True
                print('The game is still closing. Continuing to wait; force killing requires --allow-force-kill.'
                      if not allow_force_kill else 'Quit timed out; force killing was explicitly enabled.',
                      file=sys.stderr, flush=True)
                if allow_force_kill and process.poll() is None:
                    try:
                        # Both callers start a fresh session. Never signal a shared group.
                        if os.getpgid(process.pid) == process.pid:
                            os.killpg(process.pid, signal.SIGKILL)
                        else:
                            process.kill()
                        result['method'] = 'forced'
                    except ProcessLookupError:
                        pass
            if changed:
                report()
            if tick is not None:
                try:
                    tick()
                except Exception as error:
                    result['pose_update_error'] = str(error)
                    tick = None
                    report()
            time.sleep(.05)
    result.update(exit_code=process.wait(), wait_s=time.monotonic()-began,
                  delivered=Path(str(request)+'.delivered').is_file(),
                  acknowledged=Path(str(request)+'.ack').is_file())
    report()
    return result


def positive_seconds(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('seconds must be finite and positive')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quit-timeout', type=positive_seconds, default=60)
    parser.add_argument('--allow-force-kill', action='store_true')
    parser.add_argument('seconds', type=positive_seconds)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error('a command is required')
    build = Path(__file__).resolve().parents[1]/'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='native-timeout-', dir=build) as directory, stop_signals() as stopped:
        request = Path(directory)/'quit.request'
        env = dict(os.environ, VERANTYX_NATIVE_QUIT_FILE=str(request))
        process = subprocess.Popen(args.command, env=env, start_new_session=True)
        deadline = time.monotonic()+args.seconds
        while process.poll() is None and not stopped and time.monotonic() < deadline:
            time.sleep(.05)
        expired = process.poll() is None and not stopped
        result = wait_for_game(process, request, args.quit_timeout, args.allow_force_kill)
        print('native shutdown: '+json.dumps(result), file=sys.stderr, flush=True)
        if stopped:
            return 128+stopped[0]
        return 124 if expired else result['exit_code'] if result['exit_code'] >= 0 else 128-result['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
