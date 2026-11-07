import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import cache_control as cc

class IdentityTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.state = self.root / 'state.json'
        self.host = self.root / 'host'
        self.host.write_bytes(b'host-1')
        self.recipe = self.root / 'recipe'
        self.recipe.write_bytes(b'pipeline-recipe')
        self.inputs = {'host': self.host, 'recipe': self.recipe}

    def test_progress_reports_actual_hash_and_metadata_reuse(self):
        events=[]
        cc.verify(self.state,self.inputs,{},on_progress=lambda *event:events.append(event))
        self.assertEqual(events,[(1,2,1,0),(2,2,2,0)])
        events.clear()
        cc.verify(self.state,self.inputs,{},on_progress=lambda *event:events.append(event))
        self.assertEqual(events,[(1,2,0,1),(2,2,0,2)])

    def test_full_verify_cached_verify_force_and_policy(self):
        first = cc.verify(self.state, self.inputs, {'msaa': '4'})
        self.assertEqual((first['hashed'], first['reused']), (2, 0))
        second = cc.verify(self.state, self.inputs, {'msaa': '4'})
        self.assertEqual((second['hashed'], second['reused']), (0, 2))
        self.assertEqual(first['fingerprint'], second['fingerprint'])
        forced = cc.verify(self.state, self.inputs, {'msaa': '4'}, force=True)
        self.assertEqual(forced['hashed'], 2)
        self.assertEqual(forced['fingerprint'], first['fingerprint'])
        changed = cc.verify(self.state, self.inputs, {'msaa': '2'})
        self.assertNotEqual(changed['fingerprint'], first['fingerprint'])
        self.assertFalse(changed['warmup_skip_certified'])

    def test_file_replace_same_size_and_preserved_mtime_invalidates(self):
        first = cc.verify(self.state, self.inputs, {})
        mtime = self.host.stat().st_mtime_ns
        replacement = self.root / 'replacement'
        replacement.write_bytes(b'host-2')
        os.utime(replacement, ns=(mtime, mtime))
        os.replace(replacement, self.host)
        second = cc.verify(self.state, self.inputs, {})
        self.assertEqual(second['hashed'], 1)
        self.assertNotEqual(first['fingerprint'], second['fingerprint'])
        self.assertEqual(second['identity']['inputs']['host']['sha256'], hashlib.sha256(b'host-2').hexdigest())

    def test_status_is_metadata_only_and_detects_missing_or_changed(self):
        cc.verify(self.state, self.inputs, {})
        with patch.object(cc, '_hash_stream', side_effect=AssertionError('Status must not hash')):
            self.assertTrue(cc.status(self.state)['metadata_current'])
            self.host.write_bytes(b'changed')
            self.assertFalse(cc.status(self.state)['metadata_current'])
            self.host.unlink()
            self.assertIn('host', cc.status(self.state)['changed_inputs'])

    def test_changes_during_hash_do_not_replace_prior_state(self):
        cc.verify(self.state, self.inputs, {})
        previous = self.state.read_bytes()
        original = cc._hash_stream
        def changing(fd):
            digest = original(fd)
            self.host.write_bytes(b'mutated during hash')
            return digest
        with patch.object(cc, '_hash_stream', side_effect=changing):
            with self.assertRaises(cc.ChangedDuringRead):
                cc.verify(self.state, self.inputs, {}, force=True)
        self.assertEqual(self.state.read_bytes(), previous)

    def test_symlink_retarget_invalidates(self):
        link = self.root / 'link'
        link.symlink_to(self.host)
        first = cc.verify(self.state, {'host': link}, {})
        alternate = self.root / 'other'
        alternate.write_bytes(self.host.read_bytes())
        link.unlink()
        link.symlink_to(alternate)
        second = cc.verify(self.state, {'host': link}, {})
        self.assertEqual(second['hashed'], 1)
        self.assertNotEqual(first['fingerprint'], second['fingerprint'])

    def test_invalid_metadata_rehashes_and_invalidation_preserves_files(self):
        self.state.write_text('{broken')
        result = cc.verify(self.state, self.inputs, {})
        self.assertEqual(result['hashed'], 2)
        before = {str(p): p.read_bytes() for p in self.inputs.values()}
        cc.invalidate_metadata(self.state)
        self.assertFalse(cc.status(self.state)['metadata_current'])
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.inputs.values()})
        self.assertEqual(cc.verify(self.state, self.inputs, {})['hashed'], 2)

    def _vpk(self, archive=0, length=8, version=2):
        p = self.root / 'shaders_vulkan_dir.vpk'
        entry = struct.pack('<IHHIIH', 0, 0, archive, 0, length, 0xFFFF)
        tree = b'vcs\0shaders\0test\0' + entry + b'\0\0\0'
        header = struct.pack('<III', 0x55AA1234, version, len(tree))
        if version == 2:
            header += b'\0' * 16
        p.write_bytes(header + tree)
        return p

    def test_vpk_parts_and_missing_reference(self):
        p = self._vpk()
        with self.assertRaisesRegex(cc.IdentityError, 'Missing VPK'):
            cc.vpk_inputs('core', p)
        (self.root / 'shaders_vulkan_000.vpk').write_bytes(b'shader-0')
        (self.root / 'shaders_vulkan_007.vpk').write_bytes(b'extra-part')
        inputs = cc.vpk_inputs('core', p)
        self.assertEqual(len(inputs), 3)
        self.assertIn('core:part:0', inputs)
        self.assertIn('core:part:7', inputs)
        result = cc.verify(self.state, inputs, {})
        self.assertEqual(result['hashed'], 3)
        for version in (1, 2):
            self._vpk(archive=0x7FFF, version=version)
            self.assertIn('core:dir', cc.vpk_inputs('core', p))

    def test_vpk_malformed_tree_rejected(self):
        p = self._vpk()
        p.write_bytes(p.read_bytes()[:-1])
        with self.assertRaisesRegex(cc.IdentityError, 'Truncated'):
            cc.vpk_inputs('core', p)

    def test_atomic_failure_preserves_certificate(self):
        cc.verify(self.state, self.inputs, {})
        previous = self.state.read_bytes()
        with patch.object(cc.os, 'replace', side_effect=OSError('simulated write interruption')):
            with self.assertRaises(OSError):
                cc.verify(self.state, self.inputs, {'changed': 'yes'})
        self.assertEqual(self.state.read_bytes(), previous)
        self.assertEqual(list(self.root.glob('.state.json.*')), [])

class ContractTests(unittest.TestCase):
    def setUp(self):
        IdentityTests.setUp(self)
        self.shader = self.root / 'shader'
        self.shader.write_bytes(b'shader-code')
        self.inputs['core:part:0'] = self.shader
        self.expected_path = self.root / 'recipe-contract.json'
        observed = cc.verify(self.state, self.inputs, {'msaa': '4', 'buffers': '0'})
        self.expected = cc.make_contract(observed, ['recipe', 'core:part:0'], ['msaa'])
        cc.atomic_write(self.expected_path, self.expected)

    def test_contract_host_change_is_independent_but_shader_change_fails(self):
        self.host.write_bytes(b'new-runtime')
        observed = cc.verify(self.state, self.inputs, {'msaa': '4', 'buffers': '1'})
        self.assertTrue(cc.validate_contract(self.expected_path, observed)['valid'])
        original = self.expected_path.read_bytes()
        self.shader.write_bytes(b'new-shader')
        observed = cc.verify(self.state, self.inputs, {'msaa': '4'})
        result = cc.validate_contract(self.expected_path, observed)
        self.assertFalse(result['valid'])
        self.assertTrue(result['needs_rebuild'])
        self.assertEqual(result['mismatched_inputs'], ['core:part:0'])
        self.assertEqual(self.expected_path.read_bytes(), original)

    def test_identical_content_move_and_touch_remain_compatible(self):
        moved = self.root / 'moved-shader'
        moved.write_bytes(self.shader.read_bytes())
        self.inputs['core:part:0'] = moved
        os.utime(self.recipe, None)
        observed = cc.verify(self.state, self.inputs, {'msaa': '4'})
        self.assertTrue(cc.validate_contract(self.expected, observed)['valid'])

    def test_policy_change_missing_content_and_extra_part_require_rebuild(self):
        observed = cc.verify(self.state, self.inputs, {'msaa': '2'})
        self.assertEqual(cc.validate_contract(self.expected, observed)['mismatched_policy'], ['msaa'])
        missing = dict(self.inputs)
        missing.pop('recipe')
        observed = cc.verify(self.state, missing, {'msaa': '4'})
        self.assertEqual(cc.validate_contract(self.expected, observed)['missing_inputs'], ['recipe'])
        extra = dict(self.inputs)
        extra['core:part:1'] = self.shader
        observed = cc.verify(self.state, extra, {'msaa': '4'})
        result = cc.validate_contract(self.expected, observed)
        self.assertEqual(result['extra_inputs'], ['core:part:1'])
        self.assertTrue(result['needs_rebuild'])

    def test_missing_identity_or_contract_fails_closed(self):
        for observed in ({}, {'fingerprint': 'x'}, cc.status(self.state)):
            result = cc.validate_contract(self.expected, observed)
            self.assertFalse(result['valid'])
            self.assertTrue(result['needs_rebuild'])
        observed = cc.verify(self.state, self.inputs, {'msaa': '4'})
        self.assertFalse(cc.validate_contract(self.root / 'missing-contract', observed)['valid'])
        self.assertFalse(cc.validate_contract({}, observed)['valid'])
        self.state.write_text('{"schema":1,"digests":{},"last_verified":{"fingerprint":"x"}}')
        status = cc.status(self.state)
        self.assertFalse(status['valid'])
        self.assertFalse(status['known'])

if __name__ == '__main__':
    unittest.main()
