#!/usr/bin/env python3
"""Generate the Android loader's module/relocation/gate tables from an Android rewrite.

Input : <work>/rewritten/android-v0/{main,sdk,subsdk0}.patched.bin
        <work>/analysis/android-v0-port/android-cpu-validation.json
Output: <work>/build/android-v0/runtime_tables.h
        <work>/analysis/android-v0-port/runtime-layout.json

The output has the same Module/Symbol/Reloc/Gate layout as the Darwin
runtime_tables.h.  It is deliberately generated outside the source tree: game
bytes never enter the repository.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party' / 'd2r'))
from relr import decode_relr

MODULES = ['main', 'subsdk0', 'sdk']


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--version', default='v0', choices=('v0',))
    args = ap.parse_args()
    work = args.work.resolve()
    rewritten = work / 'rewritten' / f'android-{args.version}'
    analysis = work / 'analysis' / f'android-{args.version}-port'
    out = work / 'build' / f'android-{args.version}'
    out.mkdir(parents=True, exist_ok=True)
    report_path = analysis / 'android-cpu-validation.json'
    report = json.loads(report_path.read_text())
    assert report.get('status') == 'pass' and report.get('target') == 'android', 'Android rewrite report is not pass'

    modules = []
    for name in MODULES:
        path = rewritten / f'{name}.patched.bin'
        image = path.read_bytes()
        digest_hex = digest(path)
        assert digest_hex == report['outputs'][name], f'{path}: hash differs from the validation report'
        layout = {}
        for line in (rewritten / f'{name}.fixups.tsv').read_text().splitlines():
            _, key, value = line.split()
            try:
                layout[key] = int(value, 0)
            except ValueError:
                continue
        mod0 = struct.unpack_from('<I', image, 4)[0]
        assert image[mod0:mod0 + 4] == b'MOD0'
        dyn = mod0 + struct.unpack_from('<i', image, mod0 + 4)[0]
        tags = {}
        for offset in range(dyn, dyn + 4096 * 16, 16):
            tag, value = struct.unpack_from('<qQ', image, offset)
            if tag == 0:
                break
            tags[tag] = value
        else:
            raise ValueError('unterminated dynamic table')
        if 4 in tags:
            nsym = struct.unpack_from('<I', image, tags[4] + 4)[0]
        else:
            raw = (work / 'extracted' / args.version / 'exefs' / name).read_bytes()[:256]
            _, _, symoff, symsize = struct.unpack_from('<IIII', raw, 0x90)
            assert layout['RO_OFF'] + symoff == tags[6] and symsize % 24 == 0
            nsym = symsize // 24
        assert tags[11] == 24 and tags[9] == 24 and tags[20] == 7
        symbols = []
        for i in range(nsym):
            no, info, other, section, value, size = struct.unpack_from('<IBBHQQ', image, tags[6] + 24 * i)
            end = image.index(0, tags[5] + no, tags[5] + tags[10])
            symbols.append(dict(name=image[tags[5] + no:end].decode(), value=value, size=size,
                                defined=bool(section), binding=info >> 4, type=info & 15))
        relocs = [dict(where=where, kind=1027, symbol=0, addend=addend)
                  for where, addend in decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
        relocated = {r['where'] for r in relocs}
        for address, size in ((tags[7], tags[8]), (tags[23], tags[2])):
            assert size % 24 == 0
            for offset in range(address, address + size, 24):
                where, info, addend = struct.unpack_from('<QQq', image, offset)
                kind, si = info & 0xffffffff, info >> 32
                assert kind in (257, 1025, 1026, 1027) and si < nsym
                assert layout['DATA_OFF'] <= where <= layout['KEY_PAGE_OFF'] - 8
                assert where not in relocated
                relocated.add(where)
                relocs.append(dict(where=where, kind=kind, symbol=si, addend=addend))
        gates = []
        for line in (rewritten / f'{name}.gates.tsv').read_text().splitlines():
            ret, kind, arg, original = line.split()
            assert kind in ('cntpct', 'cntvct', 'svc')
            gates.append(dict(ret=int(ret, 0), timer=kind != 'svc', arg=int(arg), original=int(original, 0)))
        # DT_INIT's compiler-generated state load; same pattern as prepare_runtime.py.
        flag_reg, load_offset = (19, 20) if args.version in ('v143',) else (20, 24)
        a, w = struct.unpack_from('<I', image, tags[12] + 16)[0], struct.unpack_from('<I', image, tags[12] + load_offset)[0]
        assert a & 0x9f00001f == 0x90000000 | flag_reg and w & 0xffc003ff == 0xb9400008 | (flag_reg << 5)
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        ctor_flag = ((tags[12] + 16) & ~4095) + (imm << 12) + ((w >> 10) & 0xfff) * 4
        assert layout['DATA_OFF'] <= ctor_flag < layout['KEY_PAGE_OFF']
        modules.append(dict(name=name, sha256=digest_hex, size=len(image), ctor_flag=ctor_flag,
                            layout=layout, tags=tags, symbols=symbols, relocs=relocs, gates=gates))

    # main's original PLT stub for nn::os::GetSystemTick; the probe uses it to cross
    # main -> sdk -> host timer without starting the game.
    main = modules[0]
    target = '_ZN2nn2os13GetSystemTickEv'
    slot = next(r['where'] for r in main['relocs'] if main['symbols'][r['symbol']]['name'] == target and r['kind'] == 1026)
    image = (rewritten / 'main.patched.bin').read_bytes()
    plt = []
    for address in range(main['layout']['TEXT_END'] - 0x10000, main['layout']['TEXT_END'] - 12, 4):
        a, b, c, d = struct.unpack_from('<IIII', image, address)
        if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220:
            continue
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        referenced = (address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8
        if referenced == slot:
            plt.append(address)
    assert len(plt) == 1, f'PLT matches {plt}'
    main['probe_plt'] = plt[0]
    main['probe_slot'] = slot

    lines = ['/* Generated locally from verified Android rewrite output; do not commit. */',
             '#include "runtime.h"',
             '#ifdef RUNTIME_TABLES_DEFINE']
    for n, m in enumerate(modules):
        lines.append(f'static const Reloc reloc_{n}[] = {{')
        lines.extend(f'{{{r["where"]}ULL,{r["kind"]},{r["symbol"]},{r["addend"]}LL}},' for r in m['relocs'])
        lines.append('};')
        lines.append(f'static const Symbol symbol_{n}[] = {{')
        lines.extend('{' + f'{json.dumps(s["name"])},{s["value"]}ULL,{s["size"]}ULL,{int(s["defined"])},{s["binding"]},{s["type"]}' + '},' for s in m['symbols'])
        lines.append('};')
        lines.append(f'static const Gate gate_{n}[] = {{')
        lines.extend(f'{{{g["ret"]}ULL,{g["original"]}ULL,{int(g["timer"])},{g["arg"]}}},' for g in m['gates'])
        if not m['gates']:
            lines.append('{0,0,0,0},')
        lines.append('};')
    lines.append('Module modules[] = {')
    for n, m in enumerate(modules):
        l = m['layout']
        lines.append('{' + f'.name={json.dumps(m["name"])},.digest={json.dumps(m["sha256"])},.file_size={m["size"]}ULL,'
                     f'.text_end={l["TEXT_END"]}ULL,.ro_off={l["RO_OFF"]}ULL,.data_off={l["DATA_OFF"]}ULL,'
                     f'.patch_off={l["PATCH_OFF"]}ULL,.key_off={l["KEY_PAGE_OFF"]}ULL,'
                     f'.symbols=symbol_{n},.nsym={len(m["symbols"])},.relocs=reloc_{n},.nreloc={len(m["relocs"])},'
                     f'.gates=gate_{n},.ngate={len(m["gates"])},.init={m["tags"].get(12,0)}ULL,'
                     f'.ctor_flag={m["ctor_flag"]}ULL,.init_array={m["tags"].get(25,0)}ULL,'
                     f'.init_size={m["tags"].get(27,0)}ULL' + '},')
    lines.append('};')
    lines.append('#else')
    lines.append('extern Module modules[];')
    lines.append('#endif')
    lines.append(f'#define NMODULES {len(modules)}')
    lines.append(f'#define PROBE_PLT {main["probe_plt"]}ULL')
    lines.append(f'#define PROBE_SLOT {main["probe_slot"]}ULL')
    (out / 'runtime_tables.h').write_text('\n'.join(lines) + '\n')
    (analysis / 'runtime-layout.json').write_text(json.dumps(modules, indent=2) + '\n')
    print(json.dumps(dict(modules=len(modules), relocations=sum(len(m['relocs']) for m in modules),
                          gates=sum(len(m['gates']) for m in modules),
                          probe_plt=hex(main['probe_plt']), probe_slot=hex(main['probe_slot'])), indent=2))


if __name__ == '__main__':
    main()
