#!/usr/bin/env python3
"""Run tests/aot_fpcr.exe under strict AOT twice: with the lazy FPCR/FPSR stubs and with VXRT_AOT_FPCR_ALWAYS=1.
Both must print 'aot_fpcr: 0 failures', enter recompiled code and fall back to the translator never."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile


def run(runtime, exe, modules, out, name, extra):
    root = out / name
    root.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith(('VXRT_', 'VERANTYX_'))}
    env.update(VXRT_ROOT=str(root / 'root'), VXRT_AOT_DIR=str(modules), VXRT_AOT_STRICT='1', VXRT_NO_WINE='1',
               VXRT_STEAM_BRIDGE='0', VXRT_NO_ACTIVATE='1', VXRT_MUTE='1', **extra)
    with (root / 'runtime.log').open('wb') as log:
        p = subprocess.Popen(['/usr/bin/arch', '-arm64', str(runtime), str(exe)], cwd=exe.parent, env=env, stdout=log,
                             stderr=subprocess.STDOUT, start_new_session=True)
        try:
            rc = p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            raise AssertionError(f'{name}: timed out')
    text = (root / 'runtime.log').read_text()
    assert rc == 0, f'{name}: exit {rc}\n{text[-2000:]}'
    assert 'aot_fpcr: 0 failures' in text, f'{name}: {text[-2000:]}'
    s = re.findall(r'aot: (\d+) entries into recompiled code,.*? (\d+) fallbacks to the translator', text)
    assert s and int(s[-1][0]) > 0 and int(s[-1][1]) == 0, f'{name}: bad AOT summary'
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--runtime', required=True, type=Path)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--aot-dir', required=True, type=Path)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as t:
        out = Path(t)
        run(a.runtime.resolve(), a.exe.resolve(), a.aot_dir.resolve(), out, 'lazy', {})
        run(a.runtime.resolve(), a.exe.resolve(), a.aot_dir.resolve(), out, 'always', {'VXRT_AOT_FPCR_ALWAYS': '1'})
    print('aot_fpcr: lazy FPCR/FPSR stubs and VXRT_AOT_FPCR_ALWAYS=1, guest rounding/FTZ/flags and native calls -> ok')


if __name__ == '__main__':
    main()
