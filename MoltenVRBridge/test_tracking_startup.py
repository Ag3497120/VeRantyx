"""Runtime setup may precede play; headset tracking must never gate it."""
from pathlib import Path
import os,subprocess,tempfile
source=Path(__file__).with_name('start.command').read_text()
block=source[source.index('# Runtime configuration is available'):source.index('read -r runtime_width runtime_height')]
assert 'BRIDGE_READY' not in block and 'prepare_cache.py' not in block
for case in ('config-ready-no-headset','config-later-no-headset','bridge-dead','config-missing'):
 with tempfile.TemporaryDirectory() as folder:
  resolution=Path(folder)/'resolution'
  (Path(folder)/'manage_cache.py').write_text('')
  if case=='config-ready-no-headset':resolution.write_text('1728 1984\n')
  script='''set -euo pipefail
bridge_pid=$$
if [[ "$CASE" == bridge-dead ]]; then bridge_pid=99999999; fi
sleep(){ if [[ "$CASE" == config-later-no-headset ]]; then echo '1728 1984' > "$VERANTYX_RUNTIME_RESOLUTION_FILE"; fi; }
'''+block+"\necho GAME_STARTS_WITHOUT_HEADSET\n"
  result=subprocess.run(['/bin/bash','-c',script],env={**os.environ,'CASE':case,'VERANTYX_RUNTIME_RESOLUTION_FILE':str(resolution),'test':folder,'ALYX_PREWARM_PROGRESS':str(Path(folder)/'progress')},capture_output=True,text=True,timeout=5)
  if 'no-headset' in case:assert result.returncode==0 and 'GAME_STARTS_WITHOUT_HEADSET' in result.stdout,(case,result)
  else:assert result.returncode and 'GAME_STARTS_WITHOUT_HEADSET' not in result.stdout,(case,result)
  print(case,'passed')
