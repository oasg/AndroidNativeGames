#!/usr/bin/env python3
"""Audit an offline TOTK Android shader cache without touching the device.

The runtime reads `<root>/<stage>/<key>.spv` plus `<key>.res.json` (and
`<key>-<suffix>` variants).  This tool checks that layout, validates every
SPIR-V header, and summarises the NVN reflection: how many UBOs, SSBOs,
sampled textures and storage images each module declares, and which NVN slot
each binding maps to.

It is deliberately dependency-free so it runs anywhere Python 3 does.  No
game bytes are embedded; point it at a cache copied to /tmp.

Usage:
  python3 games/totk/android/tools/audit_shader_cache.py \
      --cache /tmp/totk-lane5/cache-copy \
      --dump /tmp/totk-lane5/missdump \
      --json /tmp/totk-lane5/audit.json \
      --markdown /tmp/totk-lane5/audit.md

  # only compute modules, full key table
  python3 .../audit_shader_cache.py --cache ... --stage cs --full-keys

Missing-key check: a key that a dump captured but the cache does not contain
as a canonical `<key>.spv` is reported under `missing`.  Variants alone are
not enough: `shader_cache_android.c` only loads a variant after the canonical
module hit.
"""
import argparse
import json
import pathlib
import struct
import sys

SPV_MAGIC = b'\x03\x02\x23\x07'
STAGES = ('vs', 'ps', 'gs', 'tcs', 'tes', 'cs')
SPV_MAX = 16 << 20
STAGE_BY_ID = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs'}
# Ryujinx TextureType values that appear in .res.json.  Only used for a
# human hint; the authoritative target is the translated SPIR-V sampler type
# and the `-t<slot>_<target>` variant suffix.
TEX_TYPE_HINT = {0: '1d', 1: '2d', 2: '3d', 3: '2d', 4: '3d',
                 1283: 'buffer/unknown'}


def nvn_texture_slot(entry):
    """Decode the NVN texture slot exactly like prepare_shader_cache.py."""
    slot = entry.get('slot')
    if isinstance(slot, int) and slot >= 0:
        return slot
    handle = entry.get('handle')
    if not isinstance(handle, int):
        return None
    off0 = handle & 0x3fff
    byte_off = off0 * 4
    if 0x168 <= byte_off < 0x568:
        return (byte_off - 0x168) // 8
    if off0 < 128:
        return off0
    return None


def is_hex_key(name):
    return len(name) == 64 and all(c in '0123456789abcdef' for c in name)


def split_module(name):
    """`<key>` or `<key>-<suffix>` -> (key, suffix)."""
    if is_hex_key(name):
        return name, ''
    if len(name) > 65 and is_hex_key(name[:64]) and name[64] == '-':
        return name[:64], name[65:]
    return name, ''


def compact_slots(pairs):
    """`[(binding, slot)]` -> `2..9->0..7` when contiguous, else `2->0 3->1`."""
    if not pairs:
        return '-'
    pairs = sorted(pairs)
    if len(pairs) > 2:
        b = [p[0] for p in pairs]
        s = [p[1] for p in pairs]
        if all(b[i] + 1 == b[i + 1] for i in range(len(b) - 1)) and \
           all(s[i] + 1 == s[i + 1] for i in range(len(s) - 1)):
            return f'{b[0]}..{b[-1]}->{s[0]}..{s[-1]}'
    return ' '.join(f'{a}->{b}' for a, b in pairs)


def load_res(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as e:
        return {'_error': str(e)}


def reflect_module(res, stage):
    cbufs = [(c.get('binding'), c.get('slot')) for c in res.get('cbufs') or []
             if isinstance(c.get('binding'), int)]
    sbufs = [(s.get('binding'), s.get('slot')) for s in res.get('sbufs') or []
             if isinstance(s.get('binding'), int)]
    texs = []
    for t in res.get('textures') or []:
        texs.append(dict(
            binding=t.get('binding'),
            nvn_slot=nvn_texture_slot(t),
            type=t.get('type'),
            hint=TEX_TYPE_HINT.get(t.get('type'), '?'),
            array=t.get('array'),
            handle=t.get('handle'),
            format=t.get('format'),
        ))
    imgs = []
    for i in res.get('images') or []:
        imgs.append(dict(binding=i.get('binding'), nvn_slot=i.get('slot'),
                         format=i.get('format'), type=i.get('type')))
    return dict(
        stage=stage,
        ubo=len(cbufs), ssbo=len(sbufs), sampled=len(texs),
        storage_images=len(imgs),
        cbufs=[dict(binding=a, nvn_slot=b) for a, b in cbufs],
        sbufs=[dict(binding=a, nvn_slot=b) for a, b in sbufs],
        textures=texs,
        images=imgs,
        cbuf_slots=compact_slots(cbufs),
        sbuf_slots=compact_slots(sbufs),
        tex_slots=compact_slots([(t['binding'], t['nvn_slot'])
                                 for t in texs if t['nvn_slot'] is not None]),
        notes=res.get('notes') or [],
    )


def module_role(row):
    if row['stage'] != 'cs':
        return 'graphics'
    if row['storage_images']:
        return 'compute-image-writer'
    if row['ssbo'] and row['sampled']:
        return 'compute-sample+buffer'
    if row['ssbo']:
        return 'compute-buffer'
    if row['sampled']:
        return 'compute-sample'
    return 'compute-empty'


def audit_cache(root, stages, verify):
    modules, problems = [], []
    for stage in stages:
        d = root/stage
        if not d.is_dir():
            continue
        for spv in sorted(d.glob('*.spv')):
            key, variant = split_module(spv.stem)
            row = dict(stage=stage, key=key, variant=variant,
                       spv=str(spv.relative_to(root)), spv_bytes=0,
                       reflection=False, res_key_match=None)
            if not is_hex_key(key):
                problems.append(dict(module=row['spv'],
                                     why='filename is not <64-hex>[-suffix].spv'))
                continue
            data = b''
            try:
                data = spv.read_bytes()
            except OSError as e:
                problems.append(dict(module=row['spv'], why=f'read: {e}'))
            row['spv_bytes'] = len(data)
            if len(data) < 20 or len(data) % 4 or data[:4] != SPV_MAGIC:
                problems.append(dict(module=row['spv'], why='bad SPIR-V header'))
                row['valid'] = False
            elif len(data) > SPV_MAX:
                problems.append(dict(module=row['spv'], why='SPIR-V too large'))
                row['valid'] = False
            else:
                row['valid'] = True
            res_path = spv.with_suffix('.res.json')
            if res_path.is_file():
                res = load_res(res_path)
                if '_error' in res:
                    problems.append(dict(module=str(res_path.relative_to(root)),
                                         why=f'res.json: {res["_error"]}'))
                else:
                    row['reflection'] = True
                    row.update(reflect_module(res, stage))
                    if isinstance(res.get('key'), str):
                        # The canonical res.json names `<key>`; a variant
                        # res.json names the translator job `<key>--<suffix>`
                        # while the file on disk is `<key>-<suffix>`.
                        want = key if not variant else (key + '--' + variant)
                        row['res_key_match'] = (res['key'] in (key, want))
                        if not row['res_key_match']:
                            problems.append(dict(
                                module=str(res_path.relative_to(root)),
                                why=f"res.json key {res['key'][:16]}... != {key[:16]}..."))
            else:
                if verify:
                    problems.append(dict(module=row['spv'], why='missing .res.json'))
            row.setdefault('ubo', 0)
            row.setdefault('ssbo', 0)
            row.setdefault('sampled', 0)
            row.setdefault('storage_images', 0)
            row.setdefault('cbuf_slots', '-')
            row.setdefault('sbuf_slots', '-')
            row.setdefault('tex_slots', '-')
            row.setdefault('notes', [])
            row['role'] = module_role(row)
            modules.append(row)
    return modules, problems


def scan_dumps(dumps):
    """Return {(stage, key): [dump dirs]} from captured .ctl files."""
    found = {}
    for dump in dumps:
        d = pathlib.Path(dump)
        if not d.is_dir():
            continue
        for ctl in sorted(d.glob('*.ctl')):
            try:
                raw = ctl.read_bytes()
            except OSError:
                continue
            if len(raw) < 0x718:
                continue
            stage_id = struct.unpack_from('<I', raw, 0x714)[0]
            stage = STAGE_BY_ID.get(stage_id)
            if not stage:
                continue
            found.setdefault((stage, ctl.stem), []).append(str(d))
    return found


def scan_manifest(paths):
    """Canonical (stage, key) pairs from a `find`-style cache listing.

    Accepts `<root>/<stage>/<key>.spv`, `<stage>/<key>.spv`, or `stage key`
    per line.  Variants are ignored: only canonical modules count as present.
    """
    keys = set()
    for path in paths:
        for line in pathlib.Path(path).read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if ' ' in line and '/' not in line:
                stage, name = line.split(None, 1)
            else:
                parts = line.replace('\\', '/').split('/')
                if len(parts) < 2:
                    continue
                stage, name = parts[-2], parts[-1]
            if name.endswith('.spv'):
                name = name[:-4]
            key, variant = split_module(name)
            if stage in STAGES and is_hex_key(key) and not variant:
                keys.add((stage, key))
    return keys


def missing_keys(modules, dumps, manifest_keys=None):
    canonical = {(m['stage'], m['key']) for m in modules if not m['variant']}
    canonical |= set(manifest_keys or ())
    missing = []
    for (stage, key), dirs in sorted(scan_dumps(dumps).items()):
        if (stage, key) not in canonical:
            missing.append(dict(stage=stage, key=key, dumps=sorted(set(dirs))))
    return missing


def summarise(modules):
    summary = dict(modules=len(modules), canonical=0, variants=0,
                   valid=0, reflections=0, storage_image_modules=0,
                   roles={}, stages={})
    for m in modules:
        s = summary['stages'].setdefault(
            m['stage'], dict(canonical=0, variants=0, ubo=0, ssbo=0,
                             sampled=0, storage_images=0))
        s['variants' if m['variant'] else 'canonical'] += 1
        s['ubo'] += m['ubo']
        s['ssbo'] += m['ssbo']
        s['sampled'] += m['sampled']
        s['storage_images'] += m['storage_images']
        summary['variants' if m['variant'] else 'canonical'] += 1
        summary['valid'] += 1 if m.get('valid') else 0
        summary['reflections'] += 1 if m['reflection'] else 0
        summary['storage_image_modules'] += 1 if m['storage_images'] else 0
        summary['roles'][m['role']] = summary['roles'].get(m['role'], 0) + 1
    return summary


def markdown_report(summary, modules, missing, problems, full_keys, stages):
    out = ['# Shader cache audit', '',
           'Generated by `games/totk/android/tools/audit_shader_cache.py`.', '',
           '## Summary', '',
           f"- modules: {summary['modules']} "
           f"({summary['canonical']} canonical, {summary['variants']} variants)",
           f"- valid SPIR-V: {summary['valid']}",
           f"- reflections: {summary['reflections']}",
           f"- modules with storage images: {summary['storage_image_modules']}",
           f"- roles: {json.dumps(summary['roles'], sort_keys=True)}", '']
    out += ['| stage | canonical | variants | UBOs | SSBOs | sampled | storage images |',
            '| --- | --- | --- | --- | --- | --- | --- |']
    for stage in stages:
        s = summary['stages'].get(stage)
        if not s:
            continue
        out.append(f"| {stage} | {s['canonical']} | {s['variants']} | {s['ubo']} | "
                   f"{s['ssbo']} | {s['sampled']} | {s['storage_images']} |")
    out += ['', '## Modules', '']
    for stage in stages:
        rows = [m for m in modules if m['stage'] == stage and not m['variant']]
        if not rows:
            continue
        out += [f'### {stage}', '',
                '| key | UBO | SSBO | sampled | storage img | role | cbuf slots | sbuf slots | tex slots |',
                '| --- | --- | --- | --- | --- | --- | --- | --- | --- |']
        for m in sorted(rows, key=lambda r: r['key']):
            key = m['key'] if full_keys else m['key'][:12]
            out.append(f"| `{key}` | {m['ubo']} | {m['ssbo']} | {m['sampled']} | "
                       f"{m['storage_images']} | {m['role']} | {m['cbuf_slots']} | "
                       f"{m['sbuf_slots']} | {m['tex_slots']} |")
        out.append('')
    out += ['## Missing keys', '']
    if missing:
        out += ['| stage | key | captured in |', '| --- | --- | --- |']
        for m in missing:
            out.append(f"| {m['stage']} | `{m['key']}` | {', '.join(m['dumps'])} |")
    else:
        out.append('None: every dumped key is present as a canonical `.spv`.')
    out += ['', '## Problems', '']
    if problems:
        for p in problems[:100]:
            out.append(f"- `{p['module']}`: {p['why']}")
        if len(problems) > 100:
            out.append(f'- ... {len(problems) - 100} more')
    else:
        out.append('None.')
    return '\n'.join(out) + '\n'


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=pathlib.Path, required=True,
                   help='cache root containing vs/ ps/ ... subdirectories')
    p.add_argument('--dump', action='append', default=[], type=pathlib.Path,
                   help='shader dump directory with <key>.ctl files; repeatable')
    p.add_argument('--manifest', action='append', default=[], type=pathlib.Path,
                   help='cache listing (find output or `stage key` lines); '
                        'counts canonical modules without parsing them')
    p.add_argument('--stage', action='append', choices=STAGES,
                   help='restrict to a stage; repeatable (default all)')
    p.add_argument('--json', type=pathlib.Path, help='write JSON report')
    p.add_argument('--markdown', type=pathlib.Path, help='write Markdown report')
    p.add_argument('--full-keys', action='store_true',
                   help='print full 64-hex keys in the Markdown table')
    p.add_argument('--no-verify', action='store_true',
                   help='do not report missing .res.json files as problems')
    a = p.parse_args(argv)
    if not a.cache.is_dir():
        print(f'audit_shader_cache: not a directory: {a.cache}', file=sys.stderr)
        return 1
    stages = a.stage or list(STAGES)
    modules, problems = audit_cache(a.cache, stages, not a.no_verify)
    manifest_keys = scan_manifest(a.manifest)
    missing = missing_keys(modules, a.dump, manifest_keys)
    summary = summarise(modules)
    doc = dict(schema=1, tool='audit_shader_cache', cache=str(a.cache),
               dumps=[str(d) for d in a.dump], stages=stages,
               summary=summary, missing=missing, problems=problems,
               modules=modules)
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(doc, indent=2) + '\n')
    md = markdown_report(summary, modules, missing, problems, a.full_keys, stages)
    if a.markdown:
        a.markdown.parent.mkdir(parents=True, exist_ok=True)
        a.markdown.write_text(md)
    print(f"modules={summary['modules']} canonical={summary['canonical']} "
          f"variants={summary['variants']} valid={summary['valid']} "
          f"reflections={summary['reflections']} "
          f"storage_image_modules={summary['storage_image_modules']} "
          f"missing={len(missing)} problems={len(problems)}")
    for stage in stages:
        s = summary['stages'].get(stage)
        if s:
            print(f"  {stage}: canonical={s['canonical']} variants={s['variants']} "
                  f"ubo={s['ubo']} ssbo={s['ssbo']} sampled={s['sampled']} "
                  f"images={s['storage_images']}")
    for m in missing[:40]:
        print(f"  MISSING {m['stage']} {m['key']} ({', '.join(m['dumps'])})")
    for q in problems[:20]:
        print(f"  PROBLEM {q['module']}: {q['why']}", file=sys.stderr)
    return 0 if not missing and not problems else 2


if __name__ == '__main__':
    sys.exit(main())
