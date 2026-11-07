"""Installer regression using synthetic game files; no Steam or headset required."""
import importlib.util,json,struct,sys,tempfile,unittest,zlib
from pathlib import Path
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE))
import prepare_native_ui as ui
import install

def resource(text):
    data=struct.pack('<I',zlib.crc32(text))+text
    return struct.pack('<IHHII',28+len(data),12,3,8,1)+b'DATA'+struct.pack('<II',8,len(data))+data

class InstallerTests(unittest.TestCase):
    def test_install_and_update_preserve_state_and_original_ui(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);game=root/'Alyx';depot=root/'depot';debian=root/'debian';package=root/'package'
            for p in [game/'game/hlvr/cfg',game/'game/hlvr/bin',game/'game/bin',depot/'game/bin/linuxsteamrtarm64',depot/'game/hlvr/bin/linuxsteamrtarm64',debian/'usr/lib/aarch64-linux-gnu',package/'runtime']:
                p.mkdir(parents=True)
            elf=b'\x7fELF\x02\x01'+bytes(12)+struct.pack('<H',183)
            (depot/'game/bin/linuxsteamrtarm64/libengine2.so').write_bytes(elf)
            (game/'game/hlvr/gameinfo.gi').write_text('"GameInfo" { FileSystem { SearchPaths { Game hlvr Game core } } }')
            (game/'game/hlvr/cfg/video.txt').write_text('initial')
            files={'panorama/layout/game_menus/child02.vxml_c':resource(b'<root><GameMenuOptionRenderingFoveatedTogglePanel id="c_foveated" /></root>'), 'panorama/styles/game_menus/game_menu_main.vcss_c':resource(b'GameMenuOptionRenderingFoveatedTogglePanel { width: 10px; }')}
            vpk=game/'game/hlvr/pak01_dir.vpk';ui.write_vpk(vpk,files);original=vpk.read_bytes()
            for n in ['alyx_host','bridge','vr_fidelity_levels_override.csv']:(package/'runtime'/n).write_bytes(b'synthetic executable')
            (package/'start.command').write_text('#!/bin/bash\n')
            (package/'normalize_save_names.py').write_text('')
            args=['install.py',str(game),str(depot),'--debian',str(debian)]
            with patch.object(install,'__file__',str(package/'install.py')),patch.object(sys,'argv',args),patch.object(install.subprocess,'check_output',return_value=''),patch.object(install.subprocess,'run') as run_cache:
                install.main()
                cfg=game/'verantyx/native-game/game/hlvr/cfg/video.txt';cfg.write_text('saved low preset')
                saves=game/'verantyx/native-game/game/hlvr/save';saves.mkdir();(saves/'quick.sav').write_bytes(b'user save')
                cache=game/'verantyx/pipeline-cache-v2';cache.mkdir();(cache/'native.bin.0').write_bytes(b'cached shader')
                install.main()
                self.assertEqual(run_cache.call_count,2)
                self.assertTrue(all('install_cache.py' in call.args[0][1] for call in run_cache.call_args_list))
            self.assertEqual(cfg.read_text(),'saved low preset')
            self.assertEqual((cache/'native.bin.0').read_bytes(),b'cached shader')
            self.assertEqual((saves/'quick.sav').read_bytes(),b'user save')
            self.assertEqual(vpk.read_bytes(),original)
            self.assertEqual(json.loads((game/'verantyx/moltenvr-install.json').read_text())['version'],'0.3.2')
            self.assertIn('native_ui_compat',(game/'verantyx/native-game/game/hlvr/gameinfo.gi').read_text())
            self.assertTrue((game/'verantyx/native-game/game/native_ui_compat/pak01_dir.vpk').is_file())
            self.assertEqual(len(list(game.glob('verantyx.bak.*'))),1)

if __name__=='__main__':unittest.main()
