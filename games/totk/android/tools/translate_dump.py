#!/usr/bin/env python3
"""Translate shader control/code pairs captured by the Android HLE.

`TOTK_ANDROID_SHADER_DUMP=<dir>` makes the runtime write
`<nvn-key>.ctl` (the hashed control bytes) and `<nvn-key>.code` (the runtime
Maxwell code) on a cache miss.  This tool feeds those exact runtime bytes to
the same macOS `totk_xlate` translator used by the shader spike and writes the
Android cache layout:

    <out>/<stage>/<nvn-key>.spv
    <out>/<stage>/<nvn-key>.res.json

Game-derived bytes stay under the dump/out folders; nothing is written into the
repository.

Usage:
  python3 games/totk/android/tools/translate_dump.py \
      --dump /tmp/totk-shader-dump --out /tmp/totk-shader-cache-runtime
  adb push /tmp/totk-shader-cache-runtime /data/local/tmp/totk-android/shader-cache
"""
import argparse
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[4]
MAGIC = b'\x03\x02\x23\x07'
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}


def control_key_len(control):
    declared = struct.unpack_from('<I', control, 0x20)[0] if len(control) >= 0x24 else 0
    return declared if 0x7cc <= declared < 0x880 else 0x880


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dump', type=pathlib.Path, required=True)
    p.add_argument('--out', type=pathlib.Path, required=True)
    p.add_argument('--translator', type=pathlib.Path,
                   default=REPO/'games/totk/prebuilt/macos-arm64/totk_xlate')
    p.add_argument('--answers', type=pathlib.Path,
                   help='optional JSON list of bind-state answer rows (name/attr/tex/...)')
    a = p.parse_args()

    if not a.translator.is_file():
        raise SystemExit(f'translator not found: {a.translator}')
    answers = []
    if a.answers:
        answers = json.loads(a.answers.read_text())
        if not isinstance(answers, list):
            raise SystemExit(f'{a.answers}: expected a JSON list')

    a.out.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix='totk-shader-dump-'))
    code_dir = work/'code'
    out_dir = work/'out'
    code_dir.mkdir()
    out_dir.mkdir()
    jobs = []
    rows = {}
    skipped = []
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
        base = dict(key=key, stage=STAGES[stage], code=str(sliced),
                    c1=code[c1:length].hex(), depth_minus_w=True, debug=False,
                    reduced=True, tex={}, img={}, attr={}, fout={})
        if answers:
            for ans in answers:
                suffix = ans.get('name')
                if not suffix:
                    continue
                vkey = f'{key}--{suffix}'
                jobs.append(dict(base, key=vkey, tex=ans.get('tex') or {},
                                 img=ans.get('img') or {}, attr=ans.get('attr') or {},
                                 fout=ans.get('fout') or {}))
                rows[vkey] = (key, STAGES[stage], suffix)
        else:
            jobs.append(base)
            rows[key] = (key, STAGES[stage], '')
    if not jobs:
        print(f'no shader dumps in {a.dump}', file=sys.stderr)
        return 1

    jobs_path = work/'jobs.json'
    jobs_path.write_text(json.dumps(jobs))
    log = work/'translate.log'
    with log.open('w') as f:
        r = subprocess.run([str(a.translator), str(jobs_path), str(out_dir)],
                           stdout=f, stderr=subprocess.STDOUT, text=True)
    written = 0
    for name, (key, stage, suffix) in rows.items():
        spv = out_dir/(name+'.spv')
        if not spv.is_file():
            skipped.append((name, 'translator produced no .spv'))
            continue
        data = spv.read_bytes()
        if len(data) < 20 or not data.startswith(MAGIC):
            skipped.append((name, 'bad SPIR-V'))
            continue
        dst = a.out/stage
        dst.mkdir(parents=True, exist_ok=True)
        target = key + (f'-{suffix}' if suffix else '')
        shutil.copyfile(spv, dst/(target+'.spv'))
        res = out_dir/(name+'.res.json')
        if res.is_file():
            shutil.copyfile(res, dst/(target+'.res.json'))
        written += 1
    print(f'translator exit {r.returncode}; wrote {written}/{len(jobs)} into {a.out}')
    for key, why in skipped[:20]:
        print(f'  skip {key}: {why}', file=sys.stderr)
    if r.returncode != 0:
        print(log.read_text()[-4000:], file=sys.stderr)
    return 0 if written else 1


if __name__ == '__main__':
    sys.exit(main())
