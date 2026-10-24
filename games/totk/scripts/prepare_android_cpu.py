#!/usr/bin/env python3
"""Rewrite TOTK 1.0.0 code modules for Android/arm64 and verify them offline.

This is the CPU-side milestone only: it does not build an APK, load the game or
touch the graphics layer.  It reuses the modules already extracted from the
player's own game image (the normal build's work folder) and produces
rewritten/android-v0/{main,sdk,subsdk0}.patched.bin plus the usual TSV tables.

Run with the project's Python environment (capstone, lz4, unicorn installed):

    python3 games/totk/scripts/prepare_android_cpu.py --jobs 4

The default work folder is games/totk.  A normal build keeps its work folder
next to the game file (TOTK_build), with the modules under TOTK_build/totk/
extracted/; pass that inner totk folder with --work.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D2R = ROOT / 'third_party' / 'd2r'
MODULES = ('main', 'sdk', 'subsdk0')


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_llvm():
    env = os.environ.get('LLVM_BIN')
    if env:
        return env
    candidates = (
        '/opt/homebrew/opt/llvm/bin',
        '/opt/homebrew/opt/llvm@23/bin',
        '/usr/local/opt/llvm/bin',
        '/Library/Developer/CommandLineTools/usr/bin',
        '/usr/bin',
    )
    for c in candidates:
        if os.path.isfile(os.path.join(c, 'llvm-objdump')):
            return c
    found = shutil.which('llvm-objdump')
    if found:
        return os.path.dirname(found)
    raise SystemExit('llvm-objdump not found; install LLVM or set LLVM_BIN')


def run(cmd, log, env):
    log.parent.mkdir(parents=True, exist_ok=True)
    p = subprocess.run(cmd, cwd=ROOT, env=env, text=True, capture_output=True)
    log.write_text((p.stdout or '') + (p.stderr or ''))
    if p.returncode:
        raise RuntimeError(f'command failed ({p.returncode}): {" ".join(map(str, cmd))}\n'
                           f'see {log}\n{(p.stdout or "")[-4000:]}{(p.stderr or "")[-4000:]}')
    return p.stdout or ''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, default=ROOT)
    ap.add_argument('--version', default='v0', choices=('v0',))
    ap.add_argument('--jobs', type=int, default=max(1, min(8, os.cpu_count() or 1)))
    ap.add_argument('--mutate', type=int, default=300)
    ap.add_argument('--min-eff', type=int, default=2)
    ap.add_argument('--skip-verify', action='store_true')
    ap.add_argument('--allow-unverified-input', action='store_true',
                    help='test-only: accept modules whose SHA-256 is not the retail 1.0.0 value')
    ap.add_argument('--funcs', help='test-only: function-start file for patch_nso.py --funcs')
    args = ap.parse_args()

    work = args.work.resolve()
    extracted = work / 'extracted' / args.version / 'exefs'
    out = work / 'rewritten' / f'android-{args.version}'
    analysis = work / 'analysis' / f'android-{args.version}-port'
    logs = analysis / 'logs'
    out.mkdir(parents=True, exist_ok=True)
    analysis.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)

    expected_path = ROOT / 'assets' / 'expected-v1.0.0.json'
    expected = json.loads(expected_path.read_text())
    if expected.get('version') != 0:
        raise SystemExit(f'{expected_path}: unexpected version {expected.get("version")}')

    env = os.environ.copy()
    env['PATCH_TARGET'] = 'android'
    env['DM_RULESET'] = 'native'
    env['LLVM_BIN'] = find_llvm()
    env.setdefault('DM_VCACHE', str(analysis / 'llvm-cache'))
    py = sys.executable

    report = {
        'format': 'totk-android-cpu-validation-v1',
        'status': 'pass',
        'recorded_at': datetime.now(timezone.utc).isoformat(),
        'rom_version': args.version,
        'display_version': expected.get('display_version'),
        'target': 'android',
        'abi': 'android-v1',
        'modules': {},
        'inputs': {},
        'outputs': {},
    }

    for name in MODULES:
        src = extracted / name
        if not src.is_file():
            raise SystemExit(f'missing extracted module {src}\n'
                             f'Run the normal build once so it extracts the game, or pass --work.')
        got = digest(src)
        want = expected['exefs_sha256'].get(name)
        if want and got != want and not args.allow_unverified_input:
            raise SystemExit(f'{src}: sha256 {got} != expected {want}')
        report['inputs'][name] = got

        cmd = [py, str(D2R / 'patch_nso.py'), str(src), str(out), name, '--target', 'android']
        if args.funcs:
            cmd += ['--funcs', args.funcs]
        run(cmd, logs / f'{name}.rewrite.log', env)
        patched = out / f'{name}.patched.bin'
        report['outputs'][name] = digest(patched)

        if args.skip_verify:
            report['modules'][name] = {'rewritten': True, 'verified': False}
            continue

        struct = run([py, str(D2R / 'structck.py'), str(out), name, str(src)],
                     logs / f'{name}.structck.log', env)
        sem = run([py, str(D2R / 'equiv.py'), str(out), name, str(src),
                   '--jobs', str(args.jobs), '--mutate', str(args.mutate),
                   '--min-eff', str(args.min_eff)], logs / f'{name}.equiv.log', env)
        sm = re.search(rf'^STRUCT {re.escape(name)} (OK|FAIL) 改写点=(\d+)', struct, re.M)
        em = re.search(rf'^EQUIV {re.escape(name)} (OK|FAIL) 站点=(\d+) 试验=(\d+) 有效试验=(\d+) 双异常=(\d+).*变异=(\d+).*抓到=(\d+) 漏抓=(\d+)', sem, re.M)
        if not sm or sm.group(1) != 'OK':
            raise RuntimeError(f'{name}: structural check failed; see {logs / f"{name}.structck.log"}')
        if not em or em.group(1) != 'OK' or em.group(8) != '0' or em.group(7) != em.group(6):
            raise RuntimeError(f'{name}: equivalence/mutation check failed; see {logs / f"{name}.equiv.log"}')
        values = [int(x) for x in em.groups()[1:]]
        report['modules'][name] = {
            'rewritten': True,
            'verified': True,
            'structural_sites': int(sm.group(2)),
            'semantic_sites': values[0],
            'trials': values[1],
            'effective_trials': values[2],
            'both_fault_trials': values[3],
            'negative_controls': values[4],
            'negative_controls_detected': values[5],
        }

    totals = {}
    for m in report['modules'].values():
        for k in ('structural_sites', 'semantic_sites', 'trials', 'effective_trials',
                  'both_fault_trials', 'negative_controls', 'negative_controls_detected'):
            if k in m: totals[k] = totals.get(k, 0) + m[k]
    report['totals'] = totals
    report['scope'] = [
        'TOTK 1.0.0 only.',
        'CPU code rewriting only: x18 is native, TPIDR_EL0/GuestSlots replace the Darwin TSD slots, host gates are global key-page entries.',
        'Offline structural and per-site differential checks; no Android loader, HLE, graphics or gameplay.',
        'svc/timer gate semantics are not executed here; only their structure is checked.',
    ]
    report_path = analysis / 'android-cpu-validation.json'
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({'status': report['status'], 'target': 'android',
                      'totals': totals, 'report': str(report_path)}, indent=2))


if __name__ == '__main__':
    main()
