# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
import io
import sys
from unittest.mock import patch
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('mod_hash', Path(__file__).with_name('mod_hash.py'))
mod_hash = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod_hash)


class ModHashTests(unittest.TestCase):
    def setUp(self):
        self.maxDiff = None
        self.scratch = tempfile.TemporaryDirectory(prefix='oot-mod-hash-test-', dir=Path.cwd())
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.keys = self.root / 'keys.json'
        self.title = self.root / '0000000000000001'
        self.put('keys.json', b'{}\n')
        self.put('coop-ux.json', b'{"version":1}\n')
        self.put('0000000000000001/Alpha/romfs/a.bin', b'abc')
        self.put('0000000000000001/Beta/exefs/b.bin', b'xyz\n')
        self.put('0000000000000001/Alpha/cheats/local.txt', b'ignored')

    def put(self, path, data):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def text(self, **kwargs):
        return mod_hash.canonical_text(self.keys, self.title, **kwargs)

    def test_v1_golden_text(self):
        expected = ('oot-coop-mod-hash 1\n'
                    'keys 3 ca3d163bab055381827226140568f3bef7eaac187cebd76878e0b63e9e442356\n'
                    'sidecar coop-ux.json 14 50208d78350a7a160dec59a82df1499b6ca7da33e54c5eb11c97e342118e68bb\n'
                    'sidecar coop-anim.json absent\nsidecar coop-equip.json absent\n'
                    'sidecar coop-ledger.json absent\n'
                    'mod exefs/b.bin 4 f34fe622a8fe7565fc15be3ce8bc43d7e32a0dd744ebef509fa0bdb130c0ac31\n'
                    'mod romfs/a.bin 3 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n')
        # Golden record bytes are independently fixed; v1 ignores disabled selection.
        self.assertEqual(self.text(hash_version=1, disabled=['Alpha']), expected)
        self.assertEqual(hashlib.sha256(expected.encode()).hexdigest(),
                         '2d664dfd842f28a77e40468dbc1b732493b854d9f52fb91945fc4f09e240867e')

    def test_v2_header_and_disabled_exact_name(self):
        self.assertTrue(self.text().startswith('oot-coop-mod-hash 2\n'))
        self.assertNotIn('mod romfs/a.bin', self.text(disabled=['Alpha']))
        self.assertIn('mod romfs/a.bin', self.text(disabled=['alpha']))
        self.assertNotEqual(self.text(), self.text(hash_version=1))

    def test_case_and_lite_and_cpp_agreement(self):
        (self.title / 'Alpha/romfs').rename(self.title / 'Alpha/RoMfS')
        self.put('0000000000000001/Alpha/romfslite/c.bin', b'lite\n')
        self.put('0000000000000001/Empty/cheats/local.txt', b'ignored')
        text = self.text(disabled=['Beta'])
        self.assertIn('mod RoMfS/a.bin', text)
        self.assertIn('mod romfslite/c.bin', text)
        self.assertNotIn('mod exefs/', text)
        self.assertEqual(hashlib.sha256(text.encode()).hexdigest(), 'c8800c24610409ab0db2921a73983f00828fe9736bb78eb12a7196d5a992aceb')
        self.assertEqual({p.name for p in mod_hash.enabled_mod_folders(self.title, ['Beta'])},
                         {'Alpha', 'Empty'})
        v1 = self.text(hash_version=1, disabled=['Beta'])
        if not (self.title / 'Alpha/romfs').exists():
            self.assertNotIn('mod RoMfS/', v1)
        self.assertNotIn('mod romfslite/', v1)
        self.assertIn('mod exefs/b.bin', v1)

    def test_duplicate_case_uses_first_native_directory(self):
        other = self.title / 'Alpha/ROMFS'
        other.mkdir(exist_ok=True)
        if other.samefile(self.title / 'Alpha/romfs'):
            self.skipTest('case insensitive filesystem')
        self.put('0000000000000001/Alpha/ROMFS/other.bin', b'other')
        first = next(p for p in (self.title / 'Alpha').iterdir() if p.name.lower() == 'romfs')
        text = self.text()
        self.assertIn('mod ' + first.name + '/', text)
        self.assertEqual(sum(line.startswith('mod ') for line in text.splitlines()), 2)

    def test_config_array_defaults_and_selected_title(self):
        config = self.put('qt-config.ini', br'''[DisabledAddOns]
size=3
1\title_id=2
1\title_id\default=false
1\disabled\size=1
1\disabled\1\d=Other
1\disabled\1\d\default=false
2\title_id=1
2\title_id\default=false
2\disabled\size=3
2\disabled\1\d="Alpha"
2\disabled\1\d\default=false
2\disabled\2\d=Ignored
2\disabled\2\d\default=true
2\disabled\3\d=IgnoredMissingDefault
3\title_id=1
3\title_id\default=true
3\disabled\size=1
3\disabled\1\d=Other
3\disabled\1\d\default=false
''')
        disabled = mod_hash.disabled_from_config(config, 1)
        self.assertEqual(disabled, ['Alpha', '', ''])
        self.assertNotIn('mod romfs/', self.text(disabled=disabled))
        self.assertEqual(mod_hash.disabled_from_config(config, 9), [])

    def test_cli_exclusions_config_and_legacy(self):
        config = self.put('cli.ini', br"""[DisabledAddOns]
size=1
1\title_id=1
1\title_id\default=false
1\disabled\size=1
1\disabled\1\d=Alpha
1\disabled\1\d\default=false
""")
        base = ['mod_hash', '--keys', str(self.keys), '--load', str(self.root),
                '--program-id', '0000000000000001', '--text']
        out = io.StringIO()
        with patch.object(sys, 'argv', base + ['--config', str(config), '--disabled', 'Beta',
                                              '--disabled', 'Empty']), patch('sys.stdout', out):
            mod_hash.main()
        self.assertEqual(out.getvalue(), self.text(disabled=['Alpha', 'Beta', 'Empty']))
        out = io.StringIO()
        with patch.object(sys, 'argv', base + ['--hash-version', '1', '--disabled', 'Alpha',
                                              '--config', str(self.root / 'absent.ini')]), patch('sys.stdout', out):
            mod_hash.main()
        self.assertEqual(out.getvalue(), self.text(hash_version=1))


if __name__ == '__main__':
    unittest.main()
