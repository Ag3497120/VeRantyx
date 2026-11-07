import sys,time,json,os
from pathlib import Path
p=Path(sys.argv[1]);pid=int(sys.argv[2]);last=None
while True:
 try:os.kill(pid,0)
 except ProcessLookupError:break
 try:s=json.loads(p.read_text())
 except (OSError,ValueError):time.sleep(1);continue
 done,total=s.get('done',0),s.get('total',0);state=s.get('state',0)
 if state==1:
  marker=done//256
  if marker!=last:
   print(f'Preparing graphics: {done}/{total or "..."} pipelines',flush=True);last=marker
 elif state in (2,3):
  if state==2:print(f'Graphics ready: {done} pipelines prepared. Starting Alyx.',flush=True)
  else:print(f'Graphics preparation completed with {s.get("errors",0)} errors; see prewarm.log.',flush=True)
  break
 time.sleep(1)
