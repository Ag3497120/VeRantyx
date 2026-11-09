#!/usr/bin/env python3
"""Hold an OS-backed lock across the entire native launch, including preparation."""
from pathlib import Path
import fcntl,os,signal,subprocess,sys

def main():
 base=Path(__file__).resolve().parent.parent
 with (base/'native-session.lock').open('a+') as lock:
  try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
  except BlockingIOError:
   print('Alyx is already running or preparing shaders. Return to the existing session.',flush=True);return 1
  env={**os.environ,'ALYX_SESSION_LOCKED':'1'}
  command=[sys.executable,*sys.argv[1:]] if sys.argv[1].endswith('.py') else ['/bin/bash',*sys.argv[1:]]
  child=subprocess.Popen(command,env=env,start_new_session=True)
  def forward(number,frame):
   if child.poll() is None:child.send_signal(number)
  signal.signal(signal.SIGTERM,forward);signal.signal(signal.SIGINT,forward)
  return child.wait()
if __name__=='__main__':sys.exit(main())
