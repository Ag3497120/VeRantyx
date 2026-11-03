#!/usr/bin/env python3
"""Cross-volume probe isolation, using synthetic files and the real OS policy."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE/'tools'))
import native_overlay


class OverlayTests(unittest.TestCase):
    def test_read_only_content_and_private_state(self):
        with tempfile.TemporaryDirectory(dir=NATIVE/'build/tests', prefix='overlay-') as directory:
            root = Path(directory)
            source, run = root/'installation', root/'run'
            (source/'game/hlvr/cfg').mkdir(parents=True)
            (source/'game/hlvr/save').mkdir()
            (source/'game/hlvr/cfg/settings').write_text('original settings')
            (source/'game/hlvr/save/quick.sav').write_bytes(b'original save')
            asset = source/'game/hlvr/content.vpk'
            asset.write_bytes(b'synthetic immutable content')
            original = hashlib.sha256(asset.read_bytes()).digest()
            run.mkdir()
            overlay = run/'overlay'
            native_overlay.prepare(source, overlay, load_save=True, read_only_assets=True)
            self.assertTrue((overlay/'game/hlvr/content.vpk').is_symlink())
            self.assertFalse((overlay/'game/hlvr/cfg/settings').is_symlink())
            child = run/'child.py'
            child.write_text('''import json, pathlib, sys
overlay, source, outside = map(pathlib.Path, sys.argv[1:])
blocked = []
for path in [overlay/'game/hlvr/content.vpk', source/'new-file', outside]:
    try: path.write_text('unexpected write')
    except PermissionError: blocked.append(str(path))
(overlay/'game/hlvr/cfg/settings').write_text('private settings')
(overlay/'game/hlvr/save/quick.sav').write_bytes(b'private save')
print(json.dumps(blocked))
''')
            command = native_overlay.confine([sys.executable, '-B', str(child), str(overlay), str(source), str(root/'outside')], run)
            result = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(len(json.loads(result.stdout)), 3)
            self.assertEqual(hashlib.sha256(asset.read_bytes()).digest(), original)
            self.assertEqual((source/'game/hlvr/cfg/settings').read_text(), 'original settings')
            self.assertEqual((source/'game/hlvr/save/quick.sav').read_bytes(), b'original save')
            self.assertEqual((overlay/'game/hlvr/cfg/settings').read_text(), 'private settings')
            self.assertFalse((root/'outside').exists())

    def test_immutable_link_must_stay_inside_installation(self):
        with tempfile.TemporaryDirectory(dir=NATIVE/'build/tests', prefix='overlay-') as directory:
            root = Path(directory)
            (root/'source').mkdir()
            (root/'outside').write_text('outside')
            (root/'source/link').symlink_to(root/'outside')
            with self.assertRaisesRegex(ValueError, 'escapes'):
                native_overlay.prepare(root/'source', root/'overlay', read_only_assets=True)


if __name__ == '__main__': unittest.main()
