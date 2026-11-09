import sys,struct,zlib,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import prepare_native_ui as ui

def resource(text):
    data=struct.pack('<I',zlib.crc32(text))+text
    return struct.pack('<IHHII',28+len(data),12,3,8,1)+b'DATA'+struct.pack('<II',8,len(data))+data

class UICompatTests(unittest.TestCase):
    def test_verified_resource_and_owner_overlay(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);content=root/'content';overlay=root/'overlay'
            xml=resource(b'<root><GameMenuOptionRenderingFoveatedTogglePanel id="test" /></root>')
            css=resource(b'GameMenuOptionRenderingFoveatedTogglePanel { width: 10px; }')
            files={'panorama/layout/game_menus/child02.vxml_c':xml,'panorama/styles/game_menus/game_menu_main.vcss_c':css,
                   'panorama/images/game_menu_ui/options_advanced_hover_png.vtex_c':b'owned texture'}
            vpk=content/'game/hlvr/pak01_dir.vpk';ui.write_vpk(vpk,files);original=vpk.read_bytes()
            source=content/'game/hlvr/gameinfo.gi';source.write_text('GameInfo { FileSystem { SearchPaths { Game hlvr } } }')
            info=overlay/'game/hlvr/gameinfo.gi';info.parent.mkdir(parents=True);info.symlink_to(source)
            ui.prepare(content,overlay)
            self.assertEqual(vpk.read_bytes(),original);self.assertNotIn('native_ui_compat',source.read_text())
            self.assertFalse(info.is_symlink());self.assertIn('native_ui_compat',info.read_text())
            out=overlay/'game/native_ui_compat/pak01_dir.vpk';index=dict(ui.entries(out))
            patched=ui.read(out,index['panorama/layout/game_menus/child02.vxml_c'])
            self.assertEqual(len(patched),len(xml));self.assertNotIn(b'GameMenuOptionRenderingFoveatedTogglePanel',patched)
            self.assertEqual(ui.read(out,index['panorama/images/game_menu_ui/options_advanced_press_png.vtex_c']),b'owned texture')
            ui.prepare(content,overlay);self.assertEqual(info.read_text().count('native_ui_compat'),1)
    def test_invalid_resource_rejected(self):
        with self.assertRaises((ValueError,struct.error)):ui.patch_resource(resource(b'text')[:-1],True)

if __name__=='__main__':unittest.main()
