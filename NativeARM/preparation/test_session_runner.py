import os,signal,subprocess,tempfile,time,unittest,shutil
from pathlib import Path
class SessionTests(unittest.TestCase):
 def test_single_session_and_signal_release(self):
  with tempfile.TemporaryDirectory() as directory:
   root=Path(directory);p=root/'preparation';p.mkdir();runner=p/'session_runner.py';shutil.copy2(Path(__file__).with_name('session_runner.py'),runner)
   marker=root/'running';script=root/'child.sh';script.write_text('trap "exit 0" TERM INT\ntouch "'+str(marker)+'"\nwhile true; do sleep 0.1; done\n')
   first=subprocess.Popen(['python3',str(runner),str(script)])
   try:
    for _ in range(100):
     if marker.exists():break
     time.sleep(.02)
    self.assertTrue(marker.exists())
    second=subprocess.run(['python3',str(runner),str(script)],capture_output=True,text=True,timeout=5)
    self.assertEqual(second.returncode,1);self.assertIn('already running',second.stdout)
    first.terminate();self.assertEqual(first.wait(timeout=5),0)
    quick=root/'quick.sh';quick.write_text('exit 0\n')
    self.assertEqual(subprocess.run(['python3',str(runner),str(quick)],timeout=5).returncode,0)
   finally:
    if first.poll() is None:first.kill();first.wait()
if __name__=='__main__':unittest.main()
