#!/usr/bin/env python3
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
BUILD = REPO / 'dm/gfx/tools/dmsh_build.py'


@unittest.skipIf(os.name == 'nt', 'dmsh_build uses POSIX APIs; run these regression tests in WSL')
class AndroidShaderMapTests(unittest.TestCase):
    def run_build(self, root, record, index):
        package = root / 'pkg'
        bindir = root / 'bin'
        package.mkdir()
        bindir.mkdir()
        (package / 'vs_base_only.bin').write_bytes(record)
        spirv_cross = bindir / 'spirv-cross'
        spirv_cross.write_text('#!/bin/sh\necho test\n')
        spirv_cross.chmod(0o755)
        source_map = root / 'map.tsv'
        facts = root / 'facts.tsv'
        pairs = root / 'pairs.tsv'
        source_map.write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\n'
                              f'vs_base_only.bin\t{index}\tvertex\t0123456789abcdef\t/synthetic\n')
        facts.write_text('# generated facts\n')
        pairs.write_text('# generated pairs\n')
        env = dict(os.environ, PATH=str(bindir) + os.pathsep + os.environ.get('PATH', ''),
                   DM_GAME='01008DB008C2C000', DMSH_SPV_ONLY='1',
                   DMSH_SOURCE_MAP=str(source_map), DMSH_FACTS=str(facts),
                   DMSH_PAIRS=str(pairs), VK_SPV_OUT=str(root / 'spv'), XLATE='/bin/true')
        return subprocess.run([sys.executable, str(BUILD), str(package), str(root / 'cache')],
                              env=env, capture_output=True, text=True)

    @staticmethod
    def record():
        code = bytearray(0x100)
        struct.pack_into('<I', code, 0, 0x12345678)
        control = bytearray(0x880)
        struct.pack_into('<I', control, 0, 0x98761234)
        struct.pack_into('<I', control, 0x704, len(code))
        struct.pack_into('<2I', control, 0x700, 0, len(code))
        header = bytearray(b'BTCDBTCD' + bytes(0x30))
        struct.pack_into('<2I', header, 0x28, len(control), len(code))
        return bytes(header) + code + control

    def test_base_only_map_reads_its_generated_record(self):
        with tempfile.TemporaryDirectory(prefix='pkshield-shader-map-') as tmp:
            result = self.run_build(pathlib.Path(tmp), self.record(), 0)
            output = result.stdout + result.stderr
            self.assertIn('shaders 1: ok 0, translation failed 1', output)
            self.assertNotIn('IndexError', output)

    def test_out_of_range_dump_index_has_setup_error(self):
        with tempfile.TemporaryDirectory(prefix='pkshield-shader-map-') as tmp:
            result = self.run_build(pathlib.Path(tmp), b'BTCDBTCD', 1)
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn('points to vs_base_only.bin record 1, but this package has only 1 record', output)
            self.assertNotIn('Traceback', output)


if __name__ == '__main__':
    unittest.main()
