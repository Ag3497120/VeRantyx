import json,os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
class NotProtonTests(unittest.TestCase):
    def test_explicit_install_argument_forwarding_and_app_scope(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);game=root/'Game with spaces';(game/'verantyx').mkdir(parents=True)
            for name in ('alyx_host','bridge'):(game/'verantyx'/name).touch()
            (game/'start.command').write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$TEST_RECORD"\n')
            record=root/'arguments';env={**os.environ,'HOME':str(root/'home'),'TEST_RECORD':str(record),'STEAM_COMPAT_APP_ID':'546560'}
            subprocess.run(['/bin/bash',str(ROOT/'steam/install-notproton.sh'),str(game)],env=env,check=True,capture_output=True)
            tool=Path(env['HOME'])/'Library/Application Support/Steam/compatibilitytools.d/alyx-native-moltenvr/run'
            args=['-novid','+vr_fidelity_level_auto','0','+vr_fidelity_level','3','argument with spaces']
            subprocess.run([str(tool),'run','C:/game/hlvr.exe',*args],env=env,check=True,capture_output=True)
            self.assertEqual(record.read_text().splitlines(),args)
            record.unlink();subprocess.run([str(tool),'run','C:/redist/setup.exe'],env=env,check=True,capture_output=True)
            self.assertFalse(record.exists())
            wrong={**env,'STEAM_COMPAT_APP_ID':'1'}
            result=subprocess.run([str(tool),'run','C:/game/hlvr.exe'],env=wrong,capture_output=True)
            self.assertNotEqual(result.returncode,0);self.assertFalse(record.exists())
if __name__=='__main__':unittest.main()
