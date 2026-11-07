#!/usr/bin/env python3
"""Prepare native Metal pipelines without a headset or changing game preferences."""
from pathlib import Path
import json,os,shutil,subprocess,sys,tempfile,time,signal
def stop_requested(signum, frame):
 raise KeyboardInterrupt
signal.signal(signal.SIGTERM,stop_requested)
root=Path(__file__).resolve().parent;base=root.parent
if os.getenv('ALYX_SESSION_LOCKED')!='1':
 os.execv(sys.executable,[sys.executable,str(root/'session_runner.py'),str(Path(__file__).resolve()),*sys.argv[1:]])
if not (base/'alyx_host').is_file():raise SystemExit('Alyx host is missing.')
if subprocess.run(['/usr/bin/pgrep','-f',str(base/'alyx_host')],stdout=subprocess.DEVNULL).returncode==0:
 raise SystemExit('Close Alyx before preparing its shaders.')
if os.getenv('STEAM_COMPAT_APP_ID',os.getenv('SteamAppId',''))!='546560' and subprocess.run(['/usr/bin/pgrep','-x','steam_osx'],stdout=subprocess.DEVNULL).returncode:
 raise SystemExit('Start native Steam first.')
subprocess.run(['/usr/bin/python3',str(root/'manage_cache.py'),'verify'],check=True)
# Keep compiler/cache directories stable across preparation and playable sessions.
for folder in ('cache/tmp','cache/xdg'):(root/folder).mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='preparation-',dir=root) as directory:
 work=Path(directory);game=work/'game-root';(game/'game').mkdir(parents=True)
 source=base/'native-game'
 for child in (source/'game').iterdir():
  dest=game/'game'/child.name
  if child.name!='hlvr':dest.symlink_to(child,target_is_directory=True);continue
  dest.mkdir()
  for item in child.iterdir():
   target=dest/item.name
   if item.name=='cfg':shutil.copytree(item,target,symlinks=False)
   elif item.name.lower() in ('save','shadercache'):target.mkdir(exist_ok=True)
   elif item.name=='console.log':continue
   else:target.symlink_to(item,target_is_directory=item.is_dir())
 vr=work/'openvr';(vr/'bin/linuxarm64').mkdir(parents=True);(vr/'bin/linuxarm64/vrclient.so').touch()
 registry=vr/'openvrpaths.vrpath';registry.write_text(json.dumps(dict(config=[str(vr/'config')],log=[str(work)],runtime=[str(vr)],jsonid='vrpathreg',version=1)))
 env=dict(os.environ)
 for key in list(env):
  if key.startswith(('VERANTYX_','KL_','MVK_','ALYX_')):env.pop(key)
 replay=[root/'alyx.foz']+[p for p in (root/'supplemental.foz',root/'recorded.foz') if p.exists()]
 progress=root/'progress.json';progress.unlink(missing_ok=True)
 env.update(VERANTYX_SHARED_FRAME=str(work/'frame.dat'),VERANTYX_BENCH='1',VERANTYX_BENCH_SWAY='0',VERANTYX_DISPLAY_HZ='72',VERANTYX_EYE_PIXELS='1000000',VERANTYX_NATIVE_QUIT_FILE=str(work/'quit.request'),VERANTYX_STEAM_STATE=str(work/'steam'),VR_PATHREG_OVERRIDE=str(registry),KL_MVK_DIR=str(base/'moltenvk-game'),KL_MEM_TOTAL_MB=str(int(subprocess.check_output(['/usr/sbin/sysctl','-n','hw.memsize']))//1048576),KL_VK_PIPELINE_ARCHIVE=str(root/'cache/native.bin'),KL_VK_PRIVATE_CACHE='1',KL_VK_FOZ_REBUILD_CACHE='1',VERANTYX_ASYNC_PIPELINES='1',KL_VK_ASYNC_PIPELINES='1',KL_VK_ASYNC_THREADS='3',KL_VK_ASYNC_GAME_THREADS='3',KL_VK_FOZ_THREADS='3',KL_VK_FOZ_REPLAY=':'.join(map(str,replay)),KL_VK_FOZ_REPLAY_BLOCK='1',ALYX_PREWARM_PROGRESS=str(progress),MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='0',KL_VK_ALYX_STATIC_COLOR_WRITE='1',ALYX_ENGINE_LOG='0',VERANTYX_QUIET='1',XDG_CACHE_HOME=str(root/'cache/xdg'),TMPDIR=str(root/'cache/tmp')+'/')
 cmd=[str(base/'alyx_host'),'--libdir',str(base/'guest-lib'),'--game',str(game),'--','-vr','-steam','-novid','-net_no_parallel_build','-noasserts','-nopassiveasserts','-vulkan','-nowindow','-nosound','+map','startup']
 started=time.monotonic();status={};last=-1;requested=None;proc=None
 with (root/'preparation.log').open('wb') as output:
  try:
   proc=subprocess.Popen(cmd,env=env,cwd=game,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
   # Drain continuously while retaining only bounded preparation/errors.
   import threading
   def drain():
    written=0
    for line in proc.stdout:
     if written<65536 and any(word in line for word in (b'[foz]',b'guest fault',b'Assertion',b'device lost')):
      output.write(line);output.flush();written+=len(line)
   reader=threading.Thread(target=drain,daemon=True);reader.start()
   while proc.poll() is None:
    elapsed=time.monotonic()-started
    try:status=json.loads(progress.read_text())
    except (OSError,ValueError):pass
    mark=status.get('done',0)//256
    if mark!=last:print('Preparing graphics: %s/%s'%(status.get('done',0),status.get('total','...')),flush=True);last=mark
    if status.get('state') in (2,3) and requested is None:
     (work/'quit.request').write_text('quit\n');requested=time.monotonic()
    if elapsed>900 or (requested and time.monotonic()-requested>90):raise TimeoutError('Alyx did not finish preparation normally.')
    time.sleep(.5)
   reader.join(timeout=5)
   if proc.returncode or status.get('state')!=2:raise RuntimeError('Preparation failed; see preparation.log.')
   result={'prepared':status,'elapsed_seconds':round(time.monotonic()-started,2),'exit_code':proc.returncode,'quit_acknowledged':(work/'quit.request.ack').exists()}
   (root/'preparation-result.json').write_text(json.dumps(result,indent=2))
   print('Shaders prepared. You can now launch Alyx.',flush=True)
  finally:
   if proc and proc.poll() is None:
    (work/'quit.request').write_text('quit\n')
    try:proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
     proc.terminate()
     try:proc.wait(timeout=10)
     except subprocess.TimeoutExpired:proc.kill();proc.wait()
