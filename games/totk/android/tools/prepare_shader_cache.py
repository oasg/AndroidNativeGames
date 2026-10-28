#!/usr/bin/env python3
"""Pack an offline shader-spike result into the Android shader-cache layout.

The runtime key is NVN's SHA-256 over the control section and shader code, not
the spike's human-readable job stem.  `spike-report.json` carries both, so this
tool copies `<stem>.spv` and `<stem>.res.json` to:

    <out>/<stage>/<nvn_key>.spv
    <out>/<stage>/<nvn_key>.res.json
    <out>/<stage>/<nvn_key>-<suffix>.spv
    <out>/<stage>/<nvn_key>-<suffix>.res.json

That is the layout `shader_cache_android.c` reads from
`TOTK_ANDROID_SHADER_CACHE`.  The output may contain game-derived bytes and
belongs in the work folder, never in the repository.

The same module is also the naming authority for bind-state answers: the
runtime suffix is `a<16 x f/u/s/_>` followed by one `-t<slot>_<target>[d]` per
texture, and the translators in this directory import `answer_suffix()` and
`answer_from_reflection()` so the offline files use exactly that spelling.
"""
import argparse
import json
import pathlib
import shutil
import struct
import sys

STAGES = {'vs', 'ps', 'gs', 'tcs', 'tes', 'cs'}
SPV_MAGIC = b'\x03\x02\x23\x07'

# NVN format -> attribute class, mirroring nvn_attr_class() in nvn_android.c.
NVN_ATTR_UNSIGNED = frozenset({
    0x03, 0x08, 0x0B, 0x0F, 0x14, 0x17, 0x1B, 0x20, 0x23, 0x27, 0x2C, 0x2F,
})
NVN_ATTR_SIGNED = frozenset({
    0x04, 0x09, 0x0C, 0x10, 0x15, 0x18, 0x1C, 0x21, 0x24, 0x28, 0x2D, 0x30,
})
ATTR_CLASS_NAMES = {
    'float': 'f', 'f': 'f', '_': '_',
    'uint': 'u', 'u': 'u',
    'sint': 's', 's': 's',
}
TEX_TARGET_NAMES = {
    '1d': '1d', '1darray': '1da', '1da': '1da',
    '2d': '2d', '2darray': '2da', '2da': '2da',
    '2dms': '2dms', '2dmsarray': '2dmsa', '2dmsa': '2dmsa',
    '3d': '3d',
    'cube': 'cube', 'cubearray': 'cubearr', 'cubearr': 'cubearr',
    'buffer': 'buf', 'buf': 'buf',
}
# Ryujinx.Graphics.Shader TextureType values seen in .res.json when the
# translator could not name a sampler more precisely.
TEX_TYPE_UNKNOWN = 0
SUFFIX_CHARS = frozenset('-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')


def nvn_attr_class(fmt):
    """Attribute class the runtime uses for an NVN vertex format byte."""
    if fmt is None or fmt == '' or fmt == '_':
        return '_'
    if isinstance(fmt, str):
        try:
            fmt = int(fmt, 0)
        except ValueError:
            return ATTR_CLASS_NAMES.get(fmt.lower(), 'f')
    fmt = int(fmt) & 0xff
    if fmt in NVN_ATTR_UNSIGNED:
        return 'u'
    if fmt in NVN_ATTR_SIGNED:
        return 's'
    return 'f'


def attr_suffix(attr):
    """`a` + 16 classes for a slot -> format/class mapping (empty if all `_`)."""
    slots = {}
    if isinstance(attr, dict):
        items = attr.items()
    elif isinstance(attr, (list, tuple)):
        items = enumerate(attr)
    else:
        return ''
    for slot, value in items:
        try:
            slot = int(slot)
        except (TypeError, ValueError):
            continue
        if 0 <= slot < 16:
            slots[slot] = nvn_attr_class(value) if value is not None else '_'
    chars = ''.join(slots.get(i, '_') for i in range(16))
    return 'a' + chars if chars.strip('_') else ''


ATTR_NAMES_BY_CLASS = {'f': 'float', 'u': 'uint', 's': 'sint'}


def attr_type(value):
    """Normalize an answer value to the translator's float/sint/uint spelling."""
    if value is None or value == '' or value == '_':
        return None
    if isinstance(value, str):
        cls = ATTR_CLASS_NAMES.get(value.lower())
        if cls == '_':
            return None
        if cls:
            return ATTR_NAMES_BY_CLASS[cls]
        try:
            value = int(value, 0)
        except ValueError:
            return 'float'
    return ATTR_NAMES_BY_CLASS[nvn_attr_class(value)]


def answer_attr(answer):
    """Translator `attr` mapping for one answer (NVN formats become classes)."""
    source = answer.get('attr')
    if source is None:
        source = answer.get('formats')
    if source is None:
        return {}
    if isinstance(source, dict):
        items = source.items()
    elif isinstance(source, (list, tuple)):
        items = enumerate(source)
    else:
        return {}
    out = {}
    for slot, value in items:
        try:
            slot = int(slot)
        except (TypeError, ValueError):
            continue
        if 0 <= slot < 16 and value is not None:
            name = attr_type(value)
            if name:
                out[str(slot)] = name
    return out


def tex_token(slot, target):
    """One runtime texture suffix token: t<slot>_<target>[d]."""
    if not isinstance(target, str) or not target:
        target = '2d'
    depth = target.endswith('_depth') or target.endswith('_d')
    if depth:
        target = target[:-6] if target.endswith('_depth') else target[:-2]
    name = TEX_TARGET_NAMES.get(target.lower(), '2d')
    return f't{int(slot)}_{name}{"d" if depth else ""}'


def answer_suffix(answer, res_text=None):
    """Derive the cache suffix for one bind-state answer row.

    `answer` may carry an explicit `name`, an `attr` mapping (slot -> NVN
    format, class char, or float/sint/uint), a `formats` list/dict, and a `tex`
    mapping in reflection order: slot -> target (`2d`, `cube`, `2d_depth`, ...).
    When `tex` is absent and `res_text` is a .res.json document, texture slots
    are taken from its `textures` array in order, defaulting to 2D.
    """
    explicit = answer.get('name')
    if explicit:
        return explicit if isinstance(explicit, str) else ''

    attr = answer.get('attr')
    if attr is None and answer.get('formats') is not None:
        attr = answer['formats']
    suffix = attr_suffix(attr or {})

    tex = answer.get('tex')
    tokens = []
    if isinstance(tex, dict):
        for slot, target in tex.items():
            if target is None:
                continue
            tokens.append(tex_token(slot, target))
    elif isinstance(tex, (list, tuple)):
        for row in tex:
            if isinstance(row, (list, tuple)) and len(row) == 2:
                tokens.append(tex_token(row[0], row[1]))
    if not tokens and res_text:
        try:
            res = json.loads(res_text)
        except ValueError:
            res = {}
        targets = answer.get('tex_targets') or {}
        for entry in res.get('textures') or []:
            slot = res_texture_slot(entry)
            if slot is None:
                continue
            tokens.append(tex_token(slot, targets.get(str(slot), '2d')))

    if tokens:
        suffix += ('-' if suffix else '') + '-'.join(tokens)
    return suffix


def validate_suffix(suffix):
    return isinstance(suffix, str) and all(c in SUFFIX_CHARS for c in suffix)


def res_texture_slot(entry):
    """NVN texture slot for one .res.json textures[] row."""
    if not isinstance(entry, dict):
        return None
    slot = entry.get('slot')
    if isinstance(slot, int) and slot >= 0:
        return slot
    handle = entry.get('handle')
    if not isinstance(handle, int):
        return None
    # Same decode as reflect_json_section() in loader_android.c.
    off0 = handle & 0x3fff
    byte_off = off0 * 4
    if 0x168 <= byte_off < 0x568:
        return (byte_off - 0x168) // 8
    if off0 < 128:
        return off0
    return None


SPIRV_DIM_NAMES = {0: '1d', 1: '2d', 2: '3d', 3: 'cube', 4: '2d', 5: 'buf'}


def reflect_spirv(path):
    """Minimal SPIR-V reflection: vertex input locations and sampler targets.

    Returns `{'inputs': {location: 'f'|'s'|'u'},
               'images': {binding: {'target': '2d', 'depth': bool}}}`.
    This covers exactly what the offline variant suffix needs; it is not a
    general disassembler.
    """
    try:
        raw = pathlib.Path(path).read_bytes()
    except OSError:
        return {'inputs': {}, 'images': {}}
    if len(raw) < 20 or len(raw) % 4 or raw[:4] != SPV_MAGIC:
        return {'inputs': {}, 'images': {}}
    words = struct.unpack_from(f'<{len(raw) // 4}I', raw)
    types = {}
    decorations = {}
    variables = []
    i = 5
    while i < len(words):
        count = words[i] >> 16
        opcode = words[i] & 0xffff
        if count == 0:
            break
        args = words[i + 1:i + count]
        if opcode == 25 and len(args) >= 7:          # OpTypeImage
            types[args[0]] = ('image', args[2], args[3], args[4], args[5])
        elif opcode == 21 and len(args) >= 3:        # OpTypeInt
            types[args[0]] = ('int', bool(args[2]))
        elif opcode == 22 and len(args) >= 2:        # OpTypeFloat
            types[args[0]] = ('float',)
        elif opcode == 23 and len(args) >= 3:        # OpTypeVector
            types[args[0]] = ('vec', args[1])
        elif opcode == 27 and len(args) >= 2:        # OpTypeSampledImage
            types[args[0]] = ('sampled', args[1])
        elif opcode == 32 and len(args) >= 3:        # OpTypePointer
            types[args[0]] = ('ptr', args[2])
        elif opcode == 59 and len(args) >= 3:        # OpVariable
            variables.append((args[0], args[1], args[2]))
        elif opcode == 71 and len(args) >= 3:        # OpDecorate
            decorations.setdefault(args[0], {})[args[1]] = args[2:]
        i += count

    def deref(ty):
        for _ in range(8):
            entry = types.get(ty)
            if not entry or entry[0] != 'ptr':
                break
            ty = entry[1]
        return types.get(ty)

    def base_class(ty):
        entry = deref(ty)
        if not entry:
            return None
        if entry[0] == 'float':
            return 'f'
        if entry[0] == 'int':
            return 's' if entry[1] else 'u'
        if entry[0] == 'vec':
            component = types.get(entry[1])
            if component and component[0] == 'float':
                return 'f'
            if component and component[0] == 'int':
                return 's' if component[1] else 'u'
        return None

    inputs, images = {}, {}
    for type_id, result_id, storage in variables:
        if storage == 1:
            deco = decorations.get(result_id, {})
            loc = (deco.get(30) or [None])[0]
            if loc is not None:
                cls = base_class(type_id)
                if cls:
                    inputs[loc] = cls
        elif storage == 0:
            entry = deref(type_id)
            if not entry or entry[0] != 'sampled':
                continue
            image = types.get(entry[1])
            if not image or image[0] != 'image':
                continue
            deco = decorations.get(result_id, {})
            binding = (deco.get(33) or [None])[0]
            if binding is None:
                continue
            dim, depth, arrayed, multisampled = image[1], image[2], image[3], image[4]
            target = SPIRV_DIM_NAMES.get(dim, '2d')
            if dim == 1 and multisampled:
                target = '2dmsa' if arrayed else '2dms'
            elif dim == 1 and arrayed:
                target = '2da'
            elif dim == 3 and arrayed:
                target = 'cubearr'
            elif dim == 0 and arrayed:
                target = '1da'
            images[binding] = {'target': target, 'depth': depth == 1}
    return {'inputs': inputs, 'images': images}


def answer_from_reflection(spv_path, res_path):
    """Build one answer row matching a canonical translation.

    Vertex locations come from the canonical SPIR-V inputs, typed with the
    class the translator used (which is float unless an earlier answer said
    otherwise).  Texture slots follow the `.res.json` reflection order, the
    order the runtime uses, and take their target from the canonical sampler
    type.
    """
    reflected = reflect_spirv(spv_path)
    try:
        res = json.loads(pathlib.Path(res_path).read_text())
    except (OSError, ValueError):
        res = {}
    # Fragment inputs are varyings, not NVN vertex attributes; only a vertex
    # shader's input locations are part of the runtime bind suffix.
    attr = {}
    if res.get('stage') in (None, 'vs'):
        attr = {str(loc): cls for loc, cls in sorted(reflected['inputs'].items())}
    tex = {}
    for entry in res.get('textures') or []:
        slot = res_texture_slot(entry)
        if slot is None:
            continue
        image = reflected['images'].get(entry.get('binding'), {})
        target = image.get('target', '2d')
        if image.get('depth'):
            target += '_depth'
        tex[str(slot)] = target
    return {'attr': attr, 'tex': tex}


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
        if variant and not validate_suffix(variant):
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
    canonical = {x['nvn_key'] for x in written if not x['variant']}
    missing_canonical = sorted({x['nvn_key'] for x in written if x['variant']} - canonical)
    by_stage = {}
    for row in written:
        stats = by_stage.setdefault(row['stage'], dict(canonical=0, variants=0))
        stats['variants' if row['variant'] else 'canonical'] += 1
    (out/'cache.json').write_text(json.dumps(dict(
        schema=2, source=str(report_path), spike=str(a.spike),
        modules=len(written), canonical=len(canonical),
        variants=len(written) - len(canonical),
        stages=by_stage, missing_canonical=missing_canonical,
        skipped=[dict(key=k, why=w) for k, w in skipped],
    ), indent=2)+'\n')
    for key in missing_canonical:
        print(f'warning: {key} has variants but no canonical .spv', file=sys.stderr)
    print(f'packed {len(written)} modules into {out} ({len(skipped)} skipped, '
          f'{len(canonical)} canonical, {len(written) - len(canonical)} variants)')
    return 0 if written else 1


if __name__ == '__main__':
    sys.exit(main())
