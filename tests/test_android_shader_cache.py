"""The Android shader-cache packer writes the runtime key + stage layout.

Also covers the offline answer/variant naming shared by shader_spike.py,
translate_dump.py and nvn_variant_suffix() in nvn_android.c.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT/'games/totk/android/tools'
PACKER = TOOLS/'prepare_shader_cache.py'
TRANSLATE_DUMP = TOOLS/'translate_dump.py'
SHADER_SPIKE = TOOLS/'shader_spike.py'
MAGIC = b'\x03\x02\x23\x07'
VS_KEY = 'a'*64
PS_KEY = 'b'*64

sys.path.insert(0, str(TOOLS))
import prepare_shader_cache as pack  # noqa: E402


def spv(tag=b'\x01'):
    return MAGIC + tag*16


def control_bytes(stage=0, length=0x100):
    control = bytearray(0x880)
    struct.pack_into('<I', control, 0x20, 0x880)
    struct.pack_into('<II', control, 0x700, 0, length)
    struct.pack_into('<I', control, 0x714, stage)
    return bytes(control)


FAKE_TRANSLATOR = '''#!/usr/bin/env python3
import json, pathlib, sys
jobs = json.loads(pathlib.Path(sys.argv[1]).read_text())
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
MAGIC = b'\\x03\\x02\\x23\\x07'
for job in jobs:
    key = job['key']
    if key.startswith('bad'):
        (out/(key+'.spv')).write_bytes(b'not spirv')
        continue
    tag = key.encode()[:16].ljust(16, b'\\x00')
    (out/(key+'.spv')).write_bytes(MAGIC + tag)
    (out/(key+'.res.json')).write_text(json.dumps(dict(
        key=key, stage=job['stage'], cbufs=[], sbufs=[],
        textures=[dict(binding=1, slot=8, array=1, handle=8)] if job['stage'] == 'ps' else [],
        images=[])))
'''


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
        return subprocess.run([sys.executable, str(PACKER), '--spike', str(self.spike),
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
        self.assertEqual(meta['canonical'], 2)
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

    def test_bind_state_variants_are_packed_next_to_the_key(self):
        report = json.loads((self.spike/'spike-report.json').read_text())
        report['modules'].append(dict(key='system-vs-deadbeef', nvn_key=VS_KEY,
                                      stage='vs', variant='aff_______________'))
        (self.spike/'spike-report.json').write_text(json.dumps(report))
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        variant = out/'vs'/(VS_KEY+'-aff_______________.spv')
        self.assertEqual(variant.read_bytes(), spv())
        self.assertTrue((out/'vs'/(VS_KEY+'-aff_______________.res.json')).is_file())
        self.assertTrue((out/'vs'/(VS_KEY+'.spv')).is_file())
        meta = json.loads((out/'cache.json').read_text())
        self.assertEqual(meta['modules'], 3)
        self.assertEqual(meta['variants'], 1)

    def test_variant_without_canonical_is_reported(self):
        (self.spike/'spike-report.json').write_text(json.dumps(dict(modules=[
            dict(key='system-vs-deadbeef', nvn_key=VS_KEY, stage='vs',
                 variant='af_______________'),
        ])))
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((out/'vs'/(VS_KEY+'-af_______________.spv')).is_file())
        self.assertIn('no canonical', r.stderr)
        meta = json.loads((out/'cache.json').read_text())
        self.assertEqual(meta['missing_canonical'], [VS_KEY])

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


class AnswerSuffix(unittest.TestCase):
    def test_nvn_vertex_format_classes(self):
        self.assertEqual(pack.nvn_attr_class(0x22), 'f')   # R32G32B32A32Float
        self.assertEqual(pack.nvn_attr_class(0x23), 'u')   # R32G32B32A32Uint
        self.assertEqual(pack.nvn_attr_class(0x24), 's')   # R32G32B32A32Sint
        self.assertEqual(pack.nvn_attr_class(0x01), 'f')

    def test_attr_suffix_from_formats(self):
        suffix = pack.answer_suffix(dict(formats=[0x22, 0x24, 0x23]))
        self.assertEqual(suffix, 'afsu' + '_'*13)
        self.assertFalse(pack.answer_suffix(dict(formats=[])))

    def test_tex_rows_keep_reflection_order(self):
        suffix = pack.answer_suffix(dict(
            attr={'0': 'float', '1': 'uint'},
            tex={'10': '2d', '8': '2d'}))
        self.assertEqual(suffix, 'afu______________-t10_2d-t8_2d')

    def test_tex_targets_and_depth(self):
        self.assertEqual(pack.answer_suffix(dict(tex={'3': 'cube_depth'})),
                         't3_cubed')
        self.assertEqual(pack.answer_suffix(dict(tex={'10': '2darray'})),
                         't10_2da')
        self.assertEqual(pack.answer_suffix(dict(tex={'0': '1darray'})),
                         't0_1da')

    def test_explicit_name_wins_and_attr_normalizes(self):
        self.assertEqual(pack.answer_suffix(dict(name='aff_______________')), 
                         'aff_______________')
        self.assertEqual(pack.answer_attr(dict(formats=[0x22, 0x24, 0x23])),
                         {'0': 'float', '1': 'sint', '2': 'uint'})


class TranslateDump(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.dump = self.root/'dump'
        self.dump.mkdir()
        self.translator = self.root/'fake-xlate'
        self.translator.write_text(FAKE_TRANSLATOR)
        self.translator.chmod(0o755)
        self.add_shader(VS_KEY, stage=0)
        self.add_shader(PS_KEY, stage=1)

    def tearDown(self):
        self.tmp.cleanup()

    def add_shader(self, key, stage):
        (self.dump/(key+'.ctl')).write_bytes(control_bytes(stage))
        (self.dump/(key+'.code')).write_bytes(bytes(0x200))

    def run_tool(self, out, *args):
        return subprocess.run([sys.executable, str(TRANSLATE_DUMP),
                               '--dump', str(self.dump), '--out', str(out),
                               '--translator', str(self.translator), *args],
                              capture_output=True, text=True)

    def test_canonical_always_written(self):
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((out/'vs'/(VS_KEY+'.spv')).is_file())
        self.assertTrue((out/'ps'/(PS_KEY+'.res.json')).is_file())
        report = json.loads((out/'translation-report.json').read_text())
        self.assertEqual(report['summary']['canonical'], 2)
        self.assertEqual(report['summary']['variants'], 0)

    def test_answers_add_variants_and_keep_canonical(self):
        answers = self.root/'answers.json'
        answers.write_text(json.dumps([
            dict(attr={'0': 'float', '1': 'uint'}, tex={'10': '2d', '8': '2d'}),
            dict(formats=[0x22, 0x24]),
        ]))
        out = self.root/'cache'
        r = self.run_tool(out, '--answers', str(answers))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue((out/'vs'/(VS_KEY+'.spv')).is_file())
        self.assertTrue((out/'ps'/(PS_KEY+'.spv')).is_file())
        suffix = 'afu______________-t10_2d-t8_2d'
        self.assertTrue((out/'vs'/(VS_KEY+'-'+suffix+'.spv')).is_file())
        self.assertTrue((out/'ps'/(PS_KEY+'-'+suffix+'.res.json')).is_file())
        self.assertTrue((out/'vs'/(VS_KEY+'-afs______________.spv')).is_file())
        report = json.loads((out/'translation-report.json').read_text())
        self.assertEqual(report['summary']['canonical'], 2)
        self.assertEqual(report['summary']['variants'], 4)
        row = next(x for x in report['keys'] if x['key'] == VS_KEY)
        self.assertEqual(len(row['variants']), 2)

    def test_auto_answers_use_the_canonical_reflection(self):
        out = self.root/'cache'
        r = self.run_tool(out, '--answers', 'auto')
        self.assertEqual(r.returncode, 0, r.stderr)
        # The fake PS reflection names texture slot 8, so the derived suffix is
        # exactly what nvn_variant_suffix() would build for that bind state.
        self.assertTrue((out/'ps'/(PS_KEY+'-t8_2d.spv')).is_file())
        self.assertTrue((out/'ps'/(PS_KEY+'.spv')).is_file())

    def test_missing_answers_file_fails(self):
        r = self.run_tool(self.root/'cache', '--answers', str(self.root/'nope.json'))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('answers file not found', r.stderr)

    def test_empty_answer_row_fails(self):
        answers = self.root/'answers.json'
        answers.write_text(json.dumps([{}]))
        r = self.run_tool(self.root/'cache', '--answers', str(answers))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('no name/attr/formats/tex', r.stderr)

    def test_bad_module_is_skipped(self):
        self.add_shader('bad'+('0'*61), stage=0)
        out = self.root/'cache'
        r = self.run_tool(out)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((out/'vs'/('bad'+('0'*61)+'.spv')).exists())
        report = json.loads((out/'translation-report.json').read_text())
        self.assertTrue(any('bad' in x['key'] for x in report['skipped']))

    def test_clean_removes_stale_entries(self):
        out = self.root/'cache'
        out.mkdir()
        (out/'stale.spv').write_bytes(spv())
        r = self.run_tool(out, '--clean')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((out/'stale.spv').exists())


class ShaderSpikeAnswers(unittest.TestCase):
    def test_missing_answers_fails_before_reading_the_romfs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root/'romfs'
            romfs.mkdir()
            translator = root/'xlate'
            translator.write_text('#!/bin/sh\nexit 0\n')
            translator.chmod(0o755)
            r = subprocess.run([
                sys.executable, str(SHADER_SPIKE), '--romfs', str(romfs),
                '--out', str(root/'out'), '--translator', str(translator),
                '--answers', str(root/'missing.json')],
                capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('answers file not found', r.stderr)

    def test_empty_answers_row_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            romfs = root/'romfs'
            romfs.mkdir()
            translator = root/'xlate'
            translator.write_text('#!/bin/sh\nexit 0\n')
            translator.chmod(0o755)
            answers = root/'answers.json'
            answers.write_text('[]')
            r = subprocess.run([
                sys.executable, str(SHADER_SPIKE), '--romfs', str(romfs),
                '--out', str(root/'out'), '--translator', str(translator),
                '--answers', str(answers)],
                capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('non-empty JSON list', r.stderr)


class LowAddressResolver(unittest.TestCase):
    """Compile the pure resolver and exercise a synthetic BAHS pool."""

    TEST_C = r'''
#include "shader_cache_android.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }

int main(void) {
    static unsigned char region[0x8000];
    const size_t h = 0x1000, rec = 0x100, pool = 0x2000, pool_size = 0x3000;
    memset(region, 0, sizeof region);
    memcpy(region + h, "BAHS", 4);
    put32(region + h + 20, (uint32_t)rec);
    put32(region + h + 24, (uint32_t)pool_size);
    put32(region + h + 28, (uint32_t)pool);

    unsigned char *control = region + 0x2000;
    put32(control + 0x20, 0x880);
    put32(control + 0x700, 0x100);       /* c1 */
    put32(control + 0x704, 0x100);       /* length */
    put32(control + 0x714, 0);           /* stage = vs */

    unsigned char *code = region + h + pool + 0x400;
    put32(code, 0x12345678u);
    put32(code + 0x30, 0xfe0007e1u);     /* Maxwell instruction */

    RuntimeShaderResolveInfo info;
    const void *out = NULL;
    if (!runtime_shader_resolve_in_region(region, sizeof region, control, 0x400,
                                          &out, &info) ||
        out != code || strcmp(info.why, "bahs-pool") != 0) {
        printf("FAIL bahs-pool out=%p want=%p why=%s\n", out, (void *)code,
               info.why ? info.why : "?");
        return 1;
    }
    printf("ok bahs-pool\n");

    /* A genuine pointer is passed through. */
    out = NULL;
    if (!runtime_shader_resolve_in_region(region, sizeof region, control, 0x100000,
                                          &out, &info) ||
        out != (const void *)(uintptr_t)0x100000 || strcmp(info.why, "pointer") != 0) {
        printf("FAIL pointer why=%s\n", info.why ? info.why : "?");
        return 1;
    }
    printf("ok pointer\n");

    /* Corrupt code magic: the pool candidate must be rejected. */
    put32(code, 0);
    out = NULL;
    if (runtime_shader_resolve_in_region(region, sizeof region, control, 0x400,
                                         &out, &info) || out != NULL ||
        strcmp(info.why, "bahs-pool-bad-code") != 0) {
        printf("FAIL miss out=%p why=%s\n", out, info.why ? info.why : "?");
        return 1;
    }
    printf("ok miss\n");

    /* No BAHS: an absolute mapping-base offset still validates. */
    memset(region, 0, sizeof region);
    code = region + 0x400;
    put32(code, 0x12345678u);
    put32(code + 0x30, 0xfe0007e1u);
    control = region + 0x2000;
    put32(control + 0x20, 0x880);
    put32(control + 0x700, 0x100);
    put32(control + 0x704, 0x100);
    put32(control + 0x714, 0);
    out = NULL;
    if (!runtime_shader_resolve_in_region(region, sizeof region, control, 0x400,
                                          &out, &info) ||
        out != code || strcmp(info.why, "mapping-absolute") != 0) {
        printf("FAIL absolute out=%p why=%s\n", out, info.why ? info.why : "?");
        return 1;
    }
    printf("ok absolute\n");
    return 0;
}
'''

    def test_low_offset_resolves_to_the_package_code_pool(self):
        import shutil
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root/'resolve_test.c'
            src.write_text(self.TEST_C)
            exe = root/'resolve_test'
            r = subprocess.run([
                cc, '-std=c11', '-O1', '-I', str(ROOT/'games/totk/android'),
                str(src), str(ROOT/'games/totk/android/shader_cache_android.c'),
                str(ROOT/'games/totk/android/sha256.c'), '-o', str(exe)],
                capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('ok bahs-pool', run.stdout)
            self.assertIn('ok pointer', run.stdout)
            self.assertIn('ok miss', run.stdout)
            self.assertIn('ok absolute', run.stdout)


if __name__ == '__main__':
    unittest.main()
