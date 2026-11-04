#!/usr/bin/env python3
"""Isolated native render diagnostic. Uses an installed, licensed ARM64 game.

No encoder/headset. Clone game content into a private APFS overlay, replace
mutable links, synthesize head turns, sample coherent shared frames and retain
hashed evidence. Does not establish headset performance or playability.
"""
import argparse
import hashlib
import json
import math
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from native_quit import positive_seconds, stop_signals, wait_for_game
import native_overlay

REPO = Path(__file__).resolve().parents[2]
BLOCK = 88 + 4096*4096*8
HANDS = 64 + 2*BLOCK
SIZE = HANDS + 312 + 1024


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def controller_input(packet, elapsed, menu_times, menu_hold=.25):
    # Continue loading with trigger pulses, then leave menu interaction alone.
    trigger = 255 if elapsed % 15 < .2 and all(elapsed < at for at in menu_times) else 0
    packet[192:196] = bytes([1,1,0,trigger])
    left = 2 << 8 if any(at <= elapsed < at+menu_hold for at in menu_times) else 0
    struct.pack_into('<II',packet,196,0,left)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--installation', type=Path, required=True, help='version with lib/ and game-root/')
    ap.add_argument('--host', type=Path, default=REPO/'NativeARM/build/host/alyx_host')
    ap.add_argument('--map', default='startup')
    ap.add_argument('--seconds', type=int, default=100)
    ap.add_argument('--quit-timeout', type=positive_seconds, default=60,
                    help='seconds before reporting a slow quit; continues waiting by default')
    ap.add_argument('--allow-force-kill', action='store_true',
                    help='explicitly allow SIGKILL after the normal quit timeout')
    ap.add_argument('--sample', action='store_true', help='sample native stacks across late head turns')
    ap.add_argument('--audio', action='store_true', help='enable game audio through the Mac output')
    ap.add_argument('--load', help='save name to copy into the private overlay and load')
    ap.add_argument('--hold-yaw', type=float, help='hold a fixed yaw in radians')
    ap.add_argument('--head-pitch', type=float, default=0, help='head pitch in degrees, negative looks down')
    ap.add_argument('--head-motion', choices=('yaw', 'translation', 'still'), default='yaw')
    ap.add_argument('--read-only-assets', action='store_true',
                    help='read installed assets through links; OS-restrict every game write to this run (cross-volume probes)')
    ap.add_argument('--menu-at', type=positive_seconds, action='append', default=[],
                    help='pulse the left secondary button at this elapsed second (repeat to open/close the game menu)')
    ap.add_argument('--menu-hold', type=positive_seconds, default=.25, help='duration of each menu-button press in seconds')
    ap.add_argument('--burst', help='START:DURATION:HZ of small consecutive frame captures')
    ap.add_argument('--set', action='append', default=[])
    args = ap.parse_args()
    source = args.installation.resolve()
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-alyx-native-'+args.map
    run = REPO/'runs'/run_id
    run.mkdir(parents=True)
    result = dict(run_id=run_id, backend='nativeARM64', scenario=args.map, observed=[], unknown=[
        'headset picture, audio, physical controls and playability',
        'exact frame-to-frame GPU timing: shared frame publication is sampled',
        'shadow correctness requires inspection at the same scene and head pose'])
    def observe(metric, value, artifact):
        result['observed'].append(dict(id='o'+str(len(result['observed'])+1), metric=metric, value=value,
                                      artifact=str(artifact.relative_to(REPO)), sha256=digest(artifact)))
    def save():
        (run/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        sys.path.insert(0,str(REPO/'tools'))
        import vxrt_ledger
        vxrt_ledger.append(dict(run_id=run_id, backend='nativeARM64', result=str((run/'result.json').relative_to(REPO)),
                                result_sha256=digest(run/'result.json')))
        print(json.dumps(result, indent=2), flush=True)
    processes = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True)
    busy = [line.strip() for line in processes.splitlines()
            if re.search(r'/(vxrt|alyx_host|native-session|vrserver|vrcompositor)$', line)]
    steam = subprocess.run(['pgrep', '-x', 'steam_osx'], capture_output=True).returncode == 0
    if busy or not steam:
        result.update(status='refused', reason='user_not_playing' if busy else 'steam_running')
        save(); return 2
    if not (source/'lib/libengine2.so').is_file() or not (source/'game-root/game/hlvr').is_dir():
        result.update(status='refused', reason='game_installed'); save(); return 2
    # APFS clones are independent files. Never run through the installation's
    # cfg/save/cache links; they point at the player's persistent state.
    overlay = run/'overlay'
    native_overlay.prepare(source/'game-root', overlay, bool(args.load), args.read_only_assets)
    vr = run/'openvr'
    (vr/'bin/linuxarm64').mkdir(parents=True)
    (vr/'bin/linuxarm64/vrclient.so').touch()
    registry = vr/'openvrpaths.vrpath'
    registry.write_text(json.dumps(dict(config=[str(vr/'config')], log=[str(run)], runtime=[str(vr)],
                                        jsonid='vrpathreg', version=1)))
    frame = run/'frame.dat'
    with frame.open('wb') as f: f.truncate(SIZE)
    (run/'vr_verantyx_tangents.txt').write_text(' '.join(['-1.2798622 1.2798622 -1.2798622 1.2798622']*2))
    env = dict(os.environ)
    for name in list(env):
        if name.startswith(('VERANTYX_', 'KL_', 'MVK_CONFIG_')): env.pop(name)
    env.update(VERANTYX_SHARED_FRAME=str(frame), VERANTYX_BENCH='1', VERANTYX_BENCH_SWAY='0',
               VERANTYX_DISPLAY_HZ='72', VERANTYX_EYE_PIXELS='2200000',
               VR_PATHREG_OVERRIDE=str(registry), VERANTYX_OPENVR_ROOT=str(vr),
               VERANTYX_STEAM_STATE=str(run/'steam'), XDG_CACHE_HOME=str(overlay/'cache'),
               KL_VK_PIPELINE_ARCHIVE=str(run/'pipelines.bin'),
               KL_VK_PRIVATE_CACHE='1',
               VERANTYX_GAME_PROMPT_FILE=str(run/'game-prompt.txt'),
               VERANTYX_NATIVE_LOADING_FILE=str(run/'loading-map.json'),
               VERANTYX_NATIVE_STAGE_FILE=str(run/'loading-stage.json'))
    for setting in args.set:
        key, value = setting.split('=', 1)
        if not key.startswith(('KL_', 'VERANTYX_', 'MVK_CONFIG_')): raise ValueError(key)
        env[key] = value
    if args.read_only_assets:
        (run/'tmp').mkdir()
        env['TMPDIR'] = str(run/'tmp')
    quit_request = run/'quit.request'
    env['VERANTYX_NATIVE_QUIT_FILE'] = str(quit_request)
    command = [str(args.host.resolve()), '--libdir', str(source/'lib'), '--game', str(overlay), '--',
               '-vr', '-steam', '-noasserts', '-nopassiveasserts', '-vulkan', '-nowindow',
               '-noautofidelity', '+vr_fidelity_level', '3', '+map', args.map,
               '+hlvr_pause_after_realmap', '0']
    if not args.audio: command.append('-nosound')
    if args.load:
        if not re.fullmatch(r'[A-Za-z0-9_/-]+',args.load) or args.load.startswith('/'):
            raise ValueError('save name must stay inside the private save directory')
        command+=['+load',args.load]
        result['load_save']=args.load
        # A named quick/quit save can change between probes after headset play.
        # Hash the private input before the game can overwrite it on exit.
        save_input=overlay/'game/hlvr/save'/(args.load+'.sav')
        result['load_save_sha256']=digest(save_input) if save_input.is_file() else None
    if args.read_only_assets: command = native_overlay.confine(command, run)
    result.update(command=command, seconds=args.seconds, head_motion=args.head_motion,
                  read_only_assets=args.read_only_assets,
                  menu_press_schedule_s=args.menu_at, menu_press_duration_s=args.menu_hold, environment={k:v for k,v in env.items()
                  if k.startswith(('VERANTYX_', 'KL_', 'MVK_CONFIG_', 'XDG_'))},
                  commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(),
                  host_sha256=digest(args.host))
    (run/'command.json').write_text(json.dumps(result, indent=2))
    def matrix(yaw, x, y, z, pitch=0):
        c, s = math.cos(yaw), math.sin(yaw)
        cp,sp=math.cos(pitch),math.sin(pitch)
        return [c,0,-s,0, s*sp,cp,c*sp,0, s*cp,-sp,c*cp,0, x,y,z,1]
    samples, snapshots, images, last_seq, last_seen = [], [], [], 0, 0
    burst_frames=[]; burst_start,burst_duration,burst_hz=map(float,args.burst.split(':')) if args.burst else (0,0,1)
    burst_last=-1
    if args.burst:
        from PIL import Image
    profiler = None
    log = run/'alyx_host.log'
    series = run/'frames.json'
    captured = False
    def checkpoint(shutdown):
        nonlocal captured
        # Collection has ended. Retain the samples even if normal quit takes
        # indefinitely long; only final, closed logs are hashed into the ledger.
        if not captured:
            for ppm,w,h,fmt,data in images:
                rgb = bytearray(w*h*3)
                rgb[0::3] = data[2::4] if fmt==87 else data[0::4]
                rgb[1::3] = data[1::4]
                rgb[2::3] = data[0::4] if fmt==87 else data[2::4]
                ppm.write_bytes(f'P6\n{w} {h}\n255\n'.encode()+rgb)
            for sequence,elapsed,picture in burst_frames:
                picture.save(run/f'burst-{sequence:06d}.png')
            series.write_text(json.dumps(samples, indent=2))
            captured = True
        (run/'shutdown.json').write_text(json.dumps(shutdown, indent=2)+'\n')
        (run/'pending.json').write_text(json.dumps(dict(status='waiting_for_game', shutdown=shutdown,
            samples=series.name, collection_duration_s=measurement_duration), indent=2)+'\n')
    collection_error = None
    with stop_signals() as stopped, frame.open('r+b') as f, mmap.mmap(f.fileno(), SIZE) as shared, log.open('wb') as output:
        start = time.monotonic()
        process = subprocess.Popen(command, cwd=overlay, env=env, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        packet = bytearray(312)
        def publish_pose():
            struct.pack_into('<d', packet, 244, time.monotonic())
            seq = struct.unpack_from('<I', shared, 20)[0]
            struct.pack_into('<I', shared, 20, seq|1)
            shared[HANDS:HANDS+312] = packet
            struct.pack_into('<I', shared, 20, (seq+2)&0xfffffffe)
        try:
            while process.poll() is None and not stopped and time.monotonic()-start < args.seconds:
                elapsed = time.monotonic()-start
                if args.sample and profiler is None and elapsed >= 40:
                    profiler = subprocess.Popen(['sample',str(process.pid),'40','1','-file',str(run/'head-turn-sample.txt')],
                                                stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
                yaw = [0, .9, -.9, 0][int(max(0,elapsed-30)//8)%4]
                head_x = .15*math.sin(2*math.pi*.35*max(0,elapsed-30)) if args.head_motion == 'translation' else 0
                if args.head_motion != 'yaw': yaw = 0
                if args.hold_yaw is not None: yaw=args.hold_yaw
                packet = bytearray(312)
                struct.pack_into('<48f', packet, 0, *(matrix(yaw,head_x,1.6,0,math.radians(args.head_pitch))+matrix(yaw,head_x-.2,1.2,-.4)+matrix(yaw,head_x+.2,1.2,-.4)))
                controller_input(packet,elapsed,args.menu_at,args.menu_hold)
                struct.pack_into('<d8f', packet, 244, time.monotonic(), *([-1.2798622,1.2798622,-1.2798622,1.2798622]*2))
                packet[286] = 1
                publish_pose()
                idx = struct.unpack_from('<i', shared)[0]&1
                offset = 64+idx*BLOCK
                frame_seq,w,h,fmt = struct.unpack_from('<4I', shared, offset)
                if frame_seq and frame_seq != last_seq and 0<w<=8192 and 0<h<=4096 and w*h*4<=BLOCK-88:
                    current = dict(t=elapsed, sequence=frame_seq, gap_s=elapsed-last_seen, yaw=yaw, head_x=head_x,
                                   render_head_x=struct.unpack_from('<f',shared,offset+64)[0],
                                   render_pose_timestamp=struct.unpack_from('<d',shared,offset+80)[0],
                                   width=w, height=h, format=fmt)
                    pixels = offset+88
                    grid = [pixels+(((2*y+1)*h//32)*w+(2*x+1)*w//32)*4 for y in range(16) for x in range(16)]
                    current['sampled_clear_magenta'] = all(shared[p]>=250 and shared[p+1]<=5 and shared[p+2]>=250 for p in grid)
                    if not snapshots or elapsed >= 5+len(snapshots)*8:
                        data = shared[offset+88:offset+88+w*h*4]
                        if struct.unpack_from('<I', shared, offset)[0] != frame_seq: continue
                        ppm = run/f'frame-{len(snapshots):03d}.ppm'
                        current['snapshot'] = ppm.name; snapshots.append(ppm)
                        images.append((ppm,w,h,fmt,data))
                    if struct.unpack_from('<I', shared, offset)[0] == frame_seq:
                        if burst_start<=elapsed<burst_start+burst_duration and elapsed-burst_last>=1/burst_hz:
                            raw=shared[offset+88:offset+88+w*h*4]
                            if struct.unpack_from('<I',shared,offset)[0]==frame_seq:
                                picture=Image.frombytes('RGBA',(w,h),raw,'raw','BGRA' if fmt==87 else 'RGBA')
                                picture=picture.crop((0,0,w//2,h)).resize((512,512)).convert('RGB')
                                burst_frames.append((frame_seq,elapsed,picture));burst_last=elapsed
                        samples.append(current); last_seq=frame_seq; last_seen=elapsed
                time.sleep(.005)
        except Exception as error:
            collection_error = f'{type(error).__name__}: {error}'
        finally:
            measurement_duration = time.monotonic()-start
            natural_exit = process.poll()
            packet[194:196] = bytes(2)
            packet[196:204] = bytes(8)
            shutdown = wait_for_game(process, quit_request, args.quit_timeout, args.allow_force_kill,
                                     progress=checkpoint, tick=publish_pose)
            if profiler is not None:
                try: profiler.wait(timeout=5)
                except subprocess.TimeoutExpired: profiler.terminate(); profiler.wait()
    # Retain a closed snapshot even if a guest helper inherited the live log fd.
    final_log = run/'alyx_host-final.log'
    shutil.copyfile(log, final_log)
    log = final_log
    observe('exited_early', natural_exit is not None, log)
    observe('published_frame_samples', len(samples), series)
    if args.menu_at: observe('menu_press_schedule_s',args.menu_at,run/'command.json')
    observe('last_frame_sequence', last_seq, series)
    observe('final_frame_age_s', measurement_duration-samples[-1]['t'] if samples else None, series)
    observe('sampled_publication_max_gap_s', max((s['gap_s'] for s in samples[1:]),default=None),series)
    observe('head_turn_window_30s_max_sample_gap_s',max((s['gap_s'] for s in samples if s['t']>=30),default=None),series)
    observe('head_turn_window_30s_sample_gaps_over_100ms',sum(s['gap_s']>.1 for s in samples if s['t']>=30),series)
    observe('sampled_magenta_clear_frames', sum(s['sampled_clear_magenta'] for s in samples),series)
    observe('snapshot_files', [p.name for p in snapshots],series)
    for snapshot in snapshots: observe('frame_snapshot', snapshot.name, snapshot)
    observe('fatal_diagnostics', len(re.findall(r'UNHANDLED|SIGSEGV|graphics pipeline compilation failed', log.read_text(errors='replace'))),log)
    observe('startup_clear_pairs_replaced', [int(n) for n in re.findall(r'first scene pair; (\d+) clear-only pairs replaced',log.read_text(errors='replace'))],log)
    observe('unsupported_device_feature_errors', log.read_text(errors='replace').count('VK_ERROR_FEATURE_NOT_PRESENT: vkCreateDevice'),log)
    observe('linux_memory_budget_mib', [int(n) for n in re.findall(r'Linux memory budget: (\d+) MiB',log.read_text(errors='replace'))],log)
    observe('guest_sysinfo_mib', [dict(total=int(total), free=int(free)) for total,free in re.findall(r'guest sysinfo: total=(\d+) MiB free=(\d+) MiB',log.read_text(errors='replace'))],log)
    observe('submitted_pose_overrides', re.findall(r'submitted render pose overrides WaitGetPoses: [^\n]+',log.read_text(errors='replace')),log)
    observe('pose_query_samples', re.findall(r'pose query: [^\n]+',log.read_text(errors='replace')),log)
    if samples:
        observe('render_head_translation_range_m', max(s['render_head_x'] for s in samples)-min(s['render_head_x'] for s in samples),series)
    if args.read_only_assets: observe('game_write_scope', 'all game file writes confined to this run root',run/'game-write-scope.sb')
    observe('engine_cache_exports_over_8ms',[float(ms) for ms in re.findall(r'engine cache export: \d+ bytes, ([\d.]+) ms',log.read_text(errors='replace'))],log)
    if (run/'head-turn-sample.txt').exists(): observe('head_turn_cpu_profile','native stacks',run/'head-turn-sample.txt')
    for sequence,elapsed,picture in burst_frames:
        observe('burst_frame',dict(sequence=sequence,t=elapsed),run/f'burst-{sequence:06d}.png')
    observe('host_shutdown', shutdown, run/'shutdown.json')
    result.update(status='error' if collection_error else 'interrupted' if stopped else 'recorded',
                  duration_s=measurement_duration, total_duration_s=time.monotonic()-start, shutdown=shutdown)
    if collection_error: result['collection_error'] = collection_error
    if stopped: result['stop_signal'] = stopped[0]
    save()
    (run/'pending.json').unlink(missing_ok=True)
    # The evidence is logs, frame samples and images, not another game install.
    shutil.rmtree(overlay)
    return 1 if collection_error else 128+stopped[0] if stopped else 0


if __name__ == '__main__':
    raise SystemExit(main())
