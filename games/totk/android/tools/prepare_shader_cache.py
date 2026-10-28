#!/usr/bin/env python3
"""Pack an offline shader-spike result into the Android shader-cache layout.

The runtime key is NVN's SHA-256 over the control section and shader code, not
the spike's human-readable job stem.  `spike-report.json` carries both, so this
tool copies `<stem>.spv` and `<stem>.res.json` to:

    <out>/<stage>/<nvn_key>.spv
    <out>/<stage>/<nvn_key>.res.json

That is the layout `shader_cache_android.c` reads from
`TOTK_ANDROID_SHADER_CACHE`.  The output may contain game-derived bytes and
belongs in the work folder, never in the repository.
"""
import argparse
import json
import pathlib
import shutil
import sys

STAGES = {'vs', 'ps', 'gs', 'tcs', 'tes', 'cs'}
SPV_MAGIC = b'\x03\x02\x23\x07'


def validity(path):
    if not path.is_file():
        return 'missing'
    data = path.read_bytes()
    if len(data) < 20 or len(data) % 4:
        return 'bad size'
    if data[:4] != SPV_MAGIC:
        return 'bad magic'
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--spike', type=pathlib.Path, required=True,
                   help='spike output directory (contains out/ and spike-report.json)')
    p.add_argument('--out', type=pathlib.Path, required=True,
                   help='cache root to write; normally TOTK_ANDROID_SHADER_CACHE')
    p.add_argument('--report', type=pathlib.Path,
                   help='report path (default <spike>/spike-report.json)')
    p.add_argument('--clean', action='store_true', help='remove the output first')
    a = p.parse_args()

    report_path = a.report or a.spike/'spike-report.json'
    try:
        report = json.loads(report_path.read_text())
    except (OSError, ValueError) as e:
        raise SystemExit(f'cannot read {report_path}: {e}')
    modules = report.get('modules')
    if not isinstance(modules, list) or not modules:
        raise SystemExit(f'{report_path}: no modules')

    out = a.out
    if a.clean:
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    written, skipped = [], []
    seen = set()
    for m in modules:
        stem = m.get('key')
        key = m.get('nvn_key')
        stage = m.get('stage')
        if not isinstance(stem, str) or not isinstance(key, str) or stage not in STAGES:
            skipped.append((stem, 'invalid report row'))
            continue
        if len(key) != 64 or any(c not in '0123456789abcdef' for c in key):
            skipped.append((stem, 'invalid nvn key'))
            continue
        variant = m.get('variant') or ''
        if variant and any(c not in '-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
                           for c in variant):
            skipped.append((stem, 'invalid variant suffix'))
            continue
        row_key = (key, variant)
        if row_key in seen:
            skipped.append((stem, 'duplicate nvn key/variant'))
            continue
        src_spv = a.spike/'out'/(stem+'.spv')
        err = validity(src_spv)
        if err:
            skipped.append((stem, err))
            continue
        seen.add(row_key)
        dst = out/stage
        dst.mkdir(parents=True, exist_ok=True)
        name = key + (f'-{variant}' if variant else '')
        shutil.copyfile(src_spv, dst/(name+'.spv'))
        res = a.spike/'out'/(stem+'.res.json')
        if res.is_file():
            shutil.copyfile(res, dst/(name+'.res.json'))
        written.append(dict(key=stem, nvn_key=key, variant=variant, stage=stage,
                            spv=(dst/(name+'.spv')).stat().st_size,
                            reflection=res.is_file()))
    (out/'cache.json').write_text(json.dumps(dict(
        schema=1, source=str(report_path), spike=str(a.spike),
        modules=len(written), skipped=[dict(key=k, why=w) for k, w in skipped],
    ), indent=2)+'\n')
    print(f'packed {len(written)} modules into {out} ({len(skipped)} skipped)')
    return 0 if written else 1


if __name__ == '__main__':
    sys.exit(main())
