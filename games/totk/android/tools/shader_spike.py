#!/usr/bin/env python3
"""Time-boxed 2B.2 spike: TOTK NVN (Maxwell) shader -> SPIR-V.

This does not add a new shader translator.  It reuses the checksum-verified
Ryujinx translator already shipped for the macOS route (`totk_xlate`, which
emits SPIR-V before SPIRV-Cross turns it into MSL) and stops at SPIR-V.  The
output is a small, inspectable sample from the local RomFS; game-derived bytes
stay under /tmp and are never written into the repository.

Usage:
  python3 games/totk/android/tools/shader_spike.py \
      --romfs /tmp/totk-romfs-min \
      --out /tmp/totk-shader-spike

  # optionally translate every shader of the selected packages
  python3 .../shader_spike.py --all

The matching on-device check is:
  adb push /tmp/totk-shader-spike/out /data/local/tmp/totk-android/spv
  adb shell 'TOTK_ANDROID_SPV_DIR=/data/local/tmp/totk-android/spv \
      /data/local/tmp/totk-android/totk-android /data/local/tmp/totk-android probe-spirv'
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO/'games/totk/scripts'))
from prepare_shader_seed import decoder, bnsh_shaders, sharcb_shaders  # noqa: E402

DEFAULT_PACKAGES = (
    'Shader/system.Product.100.product.Nin_NX_NVN.bfsha.zs',
    'Shader/tree.Product.100.product.Nin_NX_NVN.bfsha.zs',
    'Shader/material.Product.100.product.Nin_NX_NVN.bfsha.zs',
)


def package_shaders(data, max_per_file=None):
    """Yield (key, stage, control, code, source_offset) from a decompressed package."""
    seen = set()
    emitted = 0

    def take(key, stage, control, code, at):
        nonlocal emitted
        if key in seen:
            return None
        seen.add(key)
        emitted += 1
        return key, stage, control, code, at

    if data[:4] == b'BAHS':
        for key, stage, control, code in sharcb_shaders(data):
            item = take(key, stage, control, code, 0)
            if item:
                yield item
                if max_per_file and emitted >= max_per_file:
                    return
        return

    pos = 0
    while True:
        pos = data.find(b'BNSH\0\0\0\0', pos)
        if pos < 0:
            return
        size = struct.unpack_from('<I', data, pos+28)[0]
        try:
            for key, stage, control, code in bnsh_shaders(data[pos:pos+size]):
                item = take(key, stage, control, code, pos)
                if item:
                    yield item
                    if max_per_file and emitted >= max_per_file:
                        return
        except (ValueError, KeyError, struct.error, IndexError) as e:
            print(f'  skip BNSH at {pos:#x}: {e}', file=sys.stderr)
        pos += max(size, 8)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--romfs', type=pathlib.Path, required=True,
                   help='extracted RomFS directory containing Shader/ and Pack/ZsDic.pack.zs')
    p.add_argument('--out', type=pathlib.Path, default=pathlib.Path('/tmp/totk-shader-spike'))
    p.add_argument('--translator', type=pathlib.Path,
                   default=REPO/'games/totk/prebuilt/macos-arm64/totk_xlate')
    p.add_argument('--packages', default=','.join(DEFAULT_PACKAGES),
                   help='comma-separated RomFS package paths')
    p.add_argument('--limit-per-file', type=int, default=2,
                   help='first N shaders per package (default 2); use --all for every shader')
    p.add_argument('--all', action='store_true')
    p.add_argument('--keep-code', action='store_true', help='keep the sliced Maxwell code files')
    a = p.parse_args()

    if not a.romfs.is_dir():
        raise SystemExit(f'RomFS directory not found: {a.romfs}')
    if not a.translator.is_file():
        raise SystemExit(f'translator not found: {a.translator}')
    a.out.mkdir(parents=True, exist_ok=True)
    code_dir = a.out/'code'
    spv_dir = a.out/'out'
    shutil.rmtree(code_dir, ignore_errors=True)
    shutil.rmtree(spv_dir, ignore_errors=True)
    code_dir.mkdir()
    spv_dir.mkdir()

    def read(name):
        return (a.romfs/name).read_bytes()

    decode = decoder(a.romfs, read)
    jobs = []
    meta = {}
    used_names = set()
    for package in [x for x in a.packages.split(',') if x]:
        if not (a.romfs/package).is_file():
            print(f'skip missing {package}', file=sys.stderr)
            continue
        data = decode(read(package))
        print(f'scan {package}: {len(data)} bytes ({data[:4]!r})')
        selected = 0
        for key, stage, control, code, at in package_shaders(data, None if a.all else a.limit_per_file):
            if stage not in ('vs', 'ps', 'cs'):
                continue
            # Keep the spike focused on draw shaders; compute needs thread-group answers.
            if stage == 'cs':
                continue
            c1, length = struct.unpack_from('<II', control, 0x700)
            if not (0 <= c1 <= length <= len(code)) or not length:
                print(f'  skip {key[:12]} {stage}: bad c1/length {c1:#x}/{length:#x}', file=sys.stderr)
                continue
            stem = f'{package.split("/")[-1].split(".")[0]}-{stage}-{key[:8]}'
            n = 2
            while stem in used_names:
                stem = f'{package.split("/")[-1].split(".")[0]}-{stage}-{key[:8]}-{n}'
                n += 1
            used_names.add(stem)
            code_path = code_dir/(stem+'.bin')
            code_path.write_bytes(code[0x30:length])
            job = dict(key=stem, stage=stage, code=str(code_path), c1=code[c1:length].hex(),
                       depth_minus_w=True, debug=False, reduced=True,
                       tex={}, img={}, attr={}, fout={})
            jobs.append(job)
            meta[stem] = dict(source=package, key=key, stage=stage, control_bytes=len(control),
                              code_bytes=length, c1_hex=c1, source_offset=at)
            selected += 1
            if not a.all and selected >= a.limit_per_file:
                break
        print(f'  selected {selected}')

    if not jobs:
        raise SystemExit('no shaders selected')
    jobs_path = a.out/'jobs.json'
    jobs_path.write_text(json.dumps(jobs, indent=2))
    print(f'translating {len(jobs)} shaders with {a.translator}')
    with (a.out/'translate.log').open('w') as log:
        r = subprocess.run([str(a.translator), str(jobs_path), str(spv_dir)],
                           stdout=log, stderr=subprocess.STDOUT, text=True)
    print((a.out/'translate.log').read_text().strip())
    if r.returncode != 0:
        print(f'translator returned {r.returncode}', file=sys.stderr)

    report = dict(spike='2B.2', translator=str(a.translator), romfs=str(a.romfs),
                  packages=[x for x in a.packages.split(',') if x], jobs=len(jobs),
                  translated=0, failed=0, modules=[], errors=[])
    for stem, m in meta.items():
        spv = spv_dir/(stem+'.spv')
        if not spv.is_file():
            report['errors'].append(dict(key=stem, error='no .spv'))
            continue
        b = spv.read_bytes()
        if len(b) < 20 or b[:4] != b'\x03\x02\x23\x07':
            report['errors'].append(dict(key=stem, error='bad SPIR-V magic/header'))
            continue
        words = struct.unpack_from('<5I', b)
        report['translated'] += 1
        report['modules'].append(dict(key=stem, stage=m['stage'], nvn_key=m['key'],
                                      bytes=len(b), words=words[1],
                                      version=f'{words[1] >> 16}.{(words[1] >> 8) & 0xff}',
                                      sha256=hashlib.sha256(b).hexdigest(),
                                      source=m['source']))
    report['failed'] = len(report['errors'])
    report['bytes_total'] = sum(x['bytes'] for x in report['modules'])
    report['bytes_avg'] = report['bytes_total']//max(1, report['translated'])
    (a.out/'spike-report.json').write_text(json.dumps(report, indent=2)+'\n')
    (a.out/'spike-report.tsv').write_text(
        'key\tstage\tnvn_key\tbytes\tversion\tsha256\n' +
        ''.join(f"{x['key']}\t{x['stage']}\t{x['nvn_key']}\t{x['bytes']}\t{x['version']}\t{x['sha256']}\n"
                for x in report['modules']))
    if not a.keep_code:
        shutil.rmtree(code_dir, ignore_errors=True)
    print(f"translated {report['translated']}/{len(jobs)}, failed {report['failed']}, "
          f"bytes={report['bytes_total']} avg={report['bytes_avg']}")
    print(f'out: {spv_dir}')
    return 0 if report['failed'] == 0 and report['translated'] == len(jobs) else 1


if __name__ == '__main__':
    raise SystemExit(main())
