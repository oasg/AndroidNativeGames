import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games' / 'totk'
D2R = GAME / 'third_party' / 'd2r'
ANDROID = GAME / 'tests' / 'android'


def tool(name):
    p = shutil.which(name)
    if p:
        return p
    for d in ('/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/llvm@23/bin',
              '/usr/local/opt/llvm/bin', '/Library/Developer/CommandLineTools/usr/bin',
              '/usr/bin'):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None


def have(mod):
    try:
        __import__(mod)
        return True
    except Exception:
        return False


@unittest.skipUnless(have('capstone') and have('lz4.block'), 'rewriter dependencies missing')
@unittest.skipUnless(tool('llvm-objdump'), 'llvm-objdump missing')
class AndroidCpuRewriteTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / 'build'
        self.env = os.environ.copy()
        self.env['PATCH_TARGET'] = 'android'
        self.env['LLVM_BIN'] = str(Path(tool('llvm-objdump')).parent)
        subprocess.run([sys.executable, str(ANDROID / 'make_fixture.py'),
                        '--out', str(self.out), '--python', sys.executable],
                       check=True, cwd=GAME, env=self.env,
                       stdout=subprocess.DEVNULL)

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, script, module, extra=()):
        return subprocess.run([sys.executable, str(D2R / script), str(self.out / 'rewritten'),
                               module, str(self.out / 'fixture.nso'), *extra],
                              check=True, cwd=D2R, env=self.env,
                              text=True, capture_output=True).stdout

    @unittest.skipUnless(have('unicorn'), 'unicorn missing')
    def test_structure_and_equivalence(self):
        s = self.run_check('structck.py', 'main')
        self.assertIn('STRUCT main OK', s)
        e = self.run_check('equiv.py', 'main', ('--jobs', '1', '--mutate', '4',
                                                 '--min-eff', '1', '--max-extra', '2'))
        self.assertIn('EQUIV main OK', e)
        self.assertIn('漏抓=0', e)

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64',
                         'native probe needs arm64 macOS')
    def test_native_probe(self):
        cc = shutil.which('clang')
        if not cc:
            self.skipTest('clang missing')
        probe = self.out / 'fixture_probe'
        subprocess.run([cc, '-O2', '-fno-stack-protector', '-ffixed-x18',
                        f'-I{self.out}', str(ANDROID / 'fixture_probe.c'),
                        str(ANDROID / 'fixture_gate.S'), '-o', str(probe)],
                       check=True, cwd=ANDROID)
        r = subprocess.run([str(probe), str(self.out / 'rewritten' / 'main.patched.bin')],
                           check=True, text=True, capture_output=True)
        self.assertIn('PASS all', r.stdout)


if __name__ == '__main__':
    unittest.main()
