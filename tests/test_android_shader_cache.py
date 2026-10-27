"""The Android shader-cache packer writes the runtime key + stage layout."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT/'games/totk/android/tools/prepare_shader_cache.py'
MAGIC = b'\x03\x02\x23\x07'
VS_KEY = 'a'*64
PS_KEY = 'b'*64


def spv(tag=b'\x01'):
    return MAGIC + tag*16


class AndroidShaderCachePacker(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.spike = self.root/'spike'
        (self.spike/'out').mkdir(parents=True)
        (self.spike/'out'/'system-vs-deadbeef.spv').write_bytes(spv())
        (self.spike/'out'/'system-vs-deadbeef.res.json').write_text(
            json.dumps(dict(cbufs=[], sbufs=[], textures=[], images=[])))
        (self.spike/'out'/'system-ps-cafebabe.spv').write_bytes(spv(b'\x02'))
        (self.spike/'spike-report.json').write_text(json.dumps(dict(modules=[
            dict(key='system-vs-deadbeef', nvn_key=VS_KEY, stage='vs'),
            dict(key='system-ps-cafebabe', nvn_key=PS_KEY, stage='ps'),
        ])))

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, out, *args):
        return subprocess.run([sys.executable, str(TOOL), '--spike', str(self.spike),
                               '--out', str(out), *args],
                              capture_output=True, text=True)

    def test_canonical_layout_uses_the_nvn_key(self):
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((out/'vs'/(VS_KEY+'.spv')).read_bytes(), spv())
        self.assertTrue((out/'vs'/(VS_KEY+'.res.json')).is_file())
        self.assertEqual((out/'ps'/(PS_KEY+'.spv')).read_bytes(), spv(b'\x02'))
        meta = json.loads((out/'cache.json').read_text())
        self.assertEqual(meta['modules'], 2)
        self.assertFalse(meta['skipped'])

    def test_bad_modules_are_skipped_not_packed(self):
        (self.spike/'out'/'system-vs-deadbeef.spv').write_bytes(b'not spirv')
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((out/'vs'/(VS_KEY+'.spv')).exists())
        self.assertTrue((out/'ps'/(PS_KEY+'.spv')).exists())
        meta = json.loads((out/'cache.json').read_text())
        self.assertEqual(meta['skipped'][0]['key'], 'system-vs-deadbeef')

    def test_clean_removes_stale_entries(self):
        out = self.root/'cache'
        out.mkdir()
        (out/'stale.spv').write_bytes(spv())
        r = self.run_tool(out, '--clean')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((out/'stale.spv').exists())

    def test_no_modules_fails(self):
        (self.spike/'spike-report.json').write_text(json.dumps(dict(modules=[])))
        r = self.run_tool(self.root/'cache')
        self.assertNotEqual(r.returncode, 0)


if __name__ == '__main__':
    unittest.main()
