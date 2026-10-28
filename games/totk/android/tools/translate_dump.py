#!/usr/bin/env python3
"""Translate shader control/code pairs captured by the Android HLE.

`TOTK_ANDROID_SHADER_DUMP=<dir>` makes the runtime write
`<nvn-key>.ctl` (the hashed control bytes) and `<nvn-key>.code` (the runtime
Maxwell code) on a cache miss.  This tool feeds those exact runtime bytes to
the same macOS `totk_xlate` translator used by the shader spike and writes the
Android cache layout:

    <out>/<stage>/<nvn-key>.spv
    <out>/<stage>/<nvn-key>.res.json
    <out>/<stage>/<nvn-key>-<suffix>.spv
    <out>/<stage>/<nvn-key>-<suffix>.res.json

The canonical module is always translated and kept as the fallback.  With
`--answers` each bind-state row additionally produces a variant.  `--answers
auto` derives one row per shader from the canonical SPIR-V / .res.json, using
the same naming as `nvn_variant_suffix()` in nvn_android.c:

    a<16 x f/u/s/_>[-t<slot>_<target>[d]]...

Game-derived bytes stay under the dump/out folders; nothing is written into
the repository.

Usage:
  python3 games/totk/android/tools/translate_dump.py \
      --dump /tmp/totk-shader-dump --out /tmp/totk-shader-cache-runtime
  python3 .../translate_dump.py --dump ... --out ... --answers auto
  adb push /tmp/totk-shader-cache-runtime /data/local/tmp/totk-android/shader-cache
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[3]
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
from prepare_shader_cache import (  # noqa: E402
    answer_attr, answer_from_reflection, answer_suffix, validate_suffix,
)

MAGIC = b'\x03\x02\x23\x07'
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}


def control_key_len(control):
    declared = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return declared if 0x7cc <= declared < 0x880 else 0x880


def load_answers(spec):
    """Parse --answers.  Returns ('auto', []) or ('list', rows)."""
    if not spec:
        return 'none', []
    if spec == 'auto':
        return 'auto', []
    path = pathlib.Path(spec)
    if not path.is_file():
        raise SystemExit(f'answers file not found: {path}')
    try:
        rows = json.loads(path.read_text())
    except ValueError as e:
        raise SystemExit(f'{path}: invalid JSON: {e}')
    if not isinstance(rows, list) or not rows:
        raise SystemExit(f'{path}: expected a non-empty JSON list')
    for i, row in enumerate(rows):
        if not isinstance(row, dict):
            raise SystemExit(f'{path}: row {i}: expected an object')
        if not (row.get('name') or row.get('attr') is not None or
                row.get('formats') is not None or row.get('tex')):
            raise SystemExit(f'{path}: row {i}: no name/attr/formats/tex answer')
        if row.get('name') and not validate_suffix(row['name']):
            raise SystemExit(f'{path}: row {i}: invalid name suffix')
    return 'list', rows


def translator_run(translator, jobs, out_dir, log_path):
    jobs_path = out_dir.parent/(out_dir.name + '-jobs.json')
    jobs_path.write_text(json.dumps(jobs))
    with log_path.open('a') as f:
        r = subprocess.run([str(translator), str(jobs_path), str(out_dir)],
                           stdout=f, stderr=subprocess.STDOUT, text=True)
    return r.returncode


def valid_spv(path):
    if not path.is_file():
        return None
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4 or not data.startswith(MAGIC):
        return None
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dump', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--translator', type=pathlib.Path,
                   default=REPO/'games/totk/prebuilt/macos-arm64/totk_xlate')
    p.add_argument('--answers',
                   help="JSON answer rows, or 'auto' to derive from the canonical reflection")
    p.add_argument('--report', type=pathlib.Path,
                   help='machine-readable report (default <out>/translation-report.json)')
    p.add_argument('--clean', action='store_true', help='remove the output first')
    a = p.parse_args()

    if not a.translator.is_file():
        raise SystemExit(f'translator not found: {a.translator}')
    if not a.dump.is_dir():
        raise SystemExit(f'dump directory not found: {a.dump}')
    answer_mode, answers = load_answers(a.answers)

    if a.clean:
        shutil.rmtree(a.out, ignore_errors=True)
    a.out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix='totk-shader-dump-'))
    code_dir = work/'code'
    out_dir = work/'out'
    code_dir.mkdir()
    out_dir.mkdir()
    log = work/'translate.log'

    shaders = []          # (key, stage, code path)
    code_by_key = {}
    skipped = []
    rows = {}             # job key -> (nvn key, stage, suffix)
    canonical_jobs = []
    for ctl_path in sorted(a.dump.glob('*.ctl')):
        key = ctl_path.stem
        code_path = a.dump/(key+'.code')
        if not code_path.is_file():
            skipped.append((key, 'missing .code'))
            continue
        control = ctl_path.read_bytes()
        code = code_path.read_bytes()
        if len(control) < 0x718:
            skipped.append((key, 'short control'))
            continue
        c1, length = struct.unpack_from('<II', control, 0x700)
        stage = struct.unpack_from('<I', control, 0x714)[0]
        if stage not in STAGES:
            skipped.append((key, f'unsupported stage {stage}'))
            continue
        if not length or length > len(code) or c1 > length or length < 0x30:
            skipped.append((key, f'bad code range c1={c1:#x} length={length:#x}'))
            continue
        sliced = code_dir/(key+'.bin')
        sliced.write_bytes(code[0x30:length])
        job = dict(key=key, stage=STAGES[stage], code=str(sliced),
                   c1=code[c1:length].hex(), depth_minus_w=True, debug=False,
                   reduced=True, tex={}, img={}, attr={}, fout={})
        canonical_jobs.append(job)
        code_by_key[key] = str(job['code'])
        rows[key] = (key, STAGES[stage], '')
        shaders.append((key, STAGES[stage], str(job['code'])))

    if not canonical_jobs:
        print(f'no shader dumps in {a.dump}', file=sys.stderr)
        return 1

    # Pass 1: canonical modules, the fallback the runtime always keeps.
    translator_run(a.translator, canonical_jobs, out_dir, log)
    canonical = {}
    for key, stage, _ in shaders:
        data = valid_spv(out_dir/(key+'.spv'))
        if data is None:
            skipped.append((key, 'translator produced no valid .spv'))
            continue
        canonical[key] = data

    # Pass 2: bind-state variants.
    variant_jobs = []
    if answer_mode == 'auto':
        for key, stage, _ in shaders:
            if key not in canonical:
                continue
            ans = answer_from_reflection(out_dir/(key+'.spv'),
                                         out_dir/(key+'.res.json'))
            suffix = answer_suffix(ans)
            if not suffix or not validate_suffix(suffix):
                skipped.append((key, 'auto answer produced no usable suffix'))
                continue
            vkey = f'{key}--{suffix}'
            variant_jobs.append(dict(
                key=vkey, stage=stage, code=code_by_key[key],
                c1=next(j['c1'] for j in canonical_jobs if j['key'] == key),
                depth_minus_w=True, debug=False, reduced=True,
                tex=ans.get('tex') or {}, img={}, attr=answer_attr(ans),
                fout={}))
            rows[vkey] = (key, stage, suffix)
    elif answer_mode == 'list':
        for key, stage, code in shaders:
            if key not in canonical:
                continue
            for i, ans in enumerate(answers):
                suffix = answer_suffix(ans)
                if not suffix or not validate_suffix(suffix):
                    skipped.append((key, f'answer {i}: empty/invalid suffix'))
                    continue
                vkey = f'{key}--{suffix}'
                if vkey in rows:
                    skipped.append((key, f'answer {i}: duplicate suffix {suffix}'))
                    continue
                variant_jobs.append(dict(
                    key=vkey, stage=stage, code=code,
                    c1=next(j['c1'] for j in canonical_jobs if j['key'] == key),
                    depth_minus_w=True, debug=False, reduced=True,
                    tex=ans.get('tex') or {}, img={}, attr=answer_attr(ans),
                    fout={}))
                rows[vkey] = (key, stage, suffix)

    if variant_jobs:
        translator_run(a.translator, variant_jobs, out_dir, log)

    written = []
    for key, stage, _ in shaders:
        data = canonical.get(key)
        if data is None:
            continue
        dst = a.out/stage
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out_dir/(key+'.spv'), dst/(key+'.spv'))
        res = out_dir/(key+'.res.json')
        if res.is_file():
            shutil.copyfile(res, dst/(key+'.res.json'))
        written.append(dict(nvn_key=key, stage=stage, variant='',
                            spv=len(data), reflection=res.is_file(),
                            source=str(a.dump/(key+'.ctl'))))

    for vkey, (key, stage, suffix) in rows.items():
        if not suffix:
            continue
        data = valid_spv(out_dir/(vkey+'.spv'))
        if data is None:
            skipped.append((key, f'variant {suffix}: no valid .spv'))
            continue
        dst = a.out/stage
        dst.mkdir(parents=True, exist_ok=True)
        target = f'{key}-{suffix}'
        shutil.copyfile(out_dir/(vkey+'.spv'), dst/(target+'.spv'))
        res = out_dir/(vkey+'.res.json')
        if res.is_file():
            shutil.copyfile(res, dst/(target+'.res.json'))
        written.append(dict(nvn_key=key, stage=stage, variant=suffix,
                            spv=len(data), reflection=res.is_file(),
                            source=str(a.dump/(key+'.ctl'))))

    by_key = {}
    for row in written:
        entry = by_key.setdefault(row['nvn_key'], dict(
            key=row['nvn_key'], stage=row['stage'], source=row['source'],
            canonical=False, variants=[]))
        if row['variant']:
            entry['variants'].append(row['variant'])
        else:
            entry['canonical'] = True
    missing_canonical = sorted(k for k, v in by_key.items() if not v['canonical'])
    summary = dict(
        canonical=sum(1 for r in written if not r['variant']),
        variants=sum(1 for r in written if r['variant']),
        reflections=sum(1 for r in written if r['reflection']),
        skipped=len(skipped), missing_canonical=len(missing_canonical),
    )
    report_path = a.report or a.out/'translation-report.json'
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(dict(
        schema=1, tool='translate_dump', dump=str(a.dump), out=str(a.out),
        answers=a.answers or '', summary=summary,
        modules=written,
        keys=sorted(by_key.values(), key=lambda x: (x['stage'], x['key'])),
        skipped=[dict(key=k, why=w) for k, w in skipped],
    ), indent=2)+'\n')
    print(f'translator exit; wrote {summary["canonical"]} canonical + '
          f'{summary["variants"]} variants into {a.out}')
    print(f'report: {report_path}')
    for key, why in skipped[:20]:
        print(f'  skip {key}: {why}', file=sys.stderr)
    if summary['canonical'] != len(shaders):
        print(log.read_text()[-4000:], file=sys.stderr)
    return 0 if summary['canonical'] else 1


if __name__ == '__main__':
    sys.exit(main())
