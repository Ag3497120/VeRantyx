import sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from normalize_save_names import normalize
class SaveSlotTests(unittest.TestCase):
    def test_case_only_rename_preserves_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);slot=root/'S-1';slot.mkdir();(slot/'quick.sav').write_bytes(b'owner save')
            untouched=root/'Screenshots';untouched.mkdir()
            self.assertEqual(normalize(root),[('S-1','s-1')]);self.assertIn('s-1',[p.name for p in root.iterdir()])
            self.assertEqual((root/'s-1/quick.sav').read_bytes(),b'owner save');self.assertTrue(untouched.is_dir())
            self.assertEqual(normalize(root),[])
    def test_conflict_or_missing(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'S1').mkdir();lower=root/'s1'
            if lower.exists():self.skipTest('case-insensitive volume: distinct colliding slots cannot exist')
            lower.mkdir()
            with self.assertRaises(RuntimeError):normalize(root)
            self.assertTrue((root/'S1').is_dir());self.assertTrue(lower.is_dir())
        self.assertEqual(normalize(Path(d)/'missing'),[])
if __name__=='__main__':unittest.main()
