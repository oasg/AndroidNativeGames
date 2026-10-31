#!/usr/bin/env python3
"""Dependency-free analyzer for the PPM frames the Android port writes.

The UI gates in `docs/android-wave0-contract.md` are all frame properties:

  * the 1920x1080 window must have many colours, not one;
  * the 0x3f composite target must not be all zero;
  * the 360x69 UI source must report a non-black pixel;
  * the title / save-slot strip must be visible.

This tool reports those numbers directly from a binary PPM (P6) or ASCII PPM
(P3), optionally per rectangle, and never needs Pillow or numpy.  It does not
contain or read game data; point it at a frame captured on the device:

  adb pull /data/local/tmp/totk-android/lanes/shader/ui-frame.ppm /tmp/frame.ppm
  python3 games/totk/android/tools/analyze_frame.py /tmp/frame.ppm \
      --region window=0,0,1920,1080 \
      --region target3f=0,0,1920,1080 \
      --region ui-source=0,0,360,69 \
      --json /tmp/frame.json

A "non-black" pixel is one whose largest channel is above `--threshold`
(default 0).  A region is clipped to the image, so a rectangle that falls
outside the frame is reported with `clipped: true` and the clipped area.

Usage:
  analyze_frame.py FRAME.ppm [--region [name=]x,y,w,h]... [--json OUT]
  analyze_frame.py --self-test
"""
import argparse
import collections
import json
import pathlib
import sys
import tempfile

DEFAULT_COLOUR_CAP = 65536


class PPMError(Exception):
    pass


def read_ppm(path):
    """Return (width, height, maxval, rgb bytes).  Supports P6 and P3."""
    raw = pathlib.Path(path).read_bytes()
    if len(raw) < 2 or raw[:2] not in (b'P3', b'P6'):
        raise PPMError(f'{path}: not a PPM (expected P3 or P6)')
    magic = raw[:2]
    pos = 2
    fields = []
    n = len(raw)
    while len(fields) < 3:
        while pos < n and raw[pos:pos + 1].isspace():
            pos += 1
        if pos < n and raw[pos:pos + 1] == b'#':
            while pos < n and raw[pos:pos + 1] not in (b'\n', b'\r'):
                pos += 1
            continue
        start = pos
        while pos < n and not raw[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise PPMError(f'{path}: truncated PPM header')
        fields.append(int(raw[start:pos]))
    width, height, maxval = fields
    if width <= 0 or height <= 0 or maxval <= 0 or maxval > 65535:
        raise PPMError(f'{path}: bad PPM header {width}x{height} maxval={maxval}')
    # Exactly one whitespace byte separates the header from the raster.
    if pos < n and raw[pos:pos + 1].isspace():
        pos += 1
    pixels = width * height
    if magic == b'P6':
        bytes_per = 1 if maxval < 256 else 2
        need = pixels * 3 * bytes_per
        body = raw[pos:pos + need]
        if len(body) != need:
            raise PPMError(f'{path}: truncated P6 raster '
                           f'(need {need}, got {len(body)})')
        if bytes_per == 2:
            # Down-sample 16-bit channels to 8-bit for analysis.
            out = bytearray(pixels * 3)
            for i in range(pixels * 3):
                out[i] = body[2 * i]
            body = bytes(out)
        return width, height, maxval, body
    # P3: whitespace-separated decimal samples.
    vals = raw[pos:].split()
    if len(vals) < pixels * 3:
        raise PPMError(f'{path}: truncated P3 raster '
                       f'(need {pixels * 3}, got {len(vals)})')
    scale = 255.0 / maxval
    out = bytearray(pixels * 3)
    for i in range(pixels * 3):
        try:
            v = int(vals[i])
        except ValueError:
            raise PPMError(f'{path}: non-integer P3 sample {vals[i]!r}')
        out[i] = 255 if v >= maxval else int(v * scale)
    return width, height, maxval, bytes(out)


def packed_at(body, index):
    o = index * 3
    return (body[o] << 16) | (body[o + 1] << 8) | body[o + 2]


def is_non_black(packed, threshold):
    return ((packed >> 16) & 0xff) > threshold or \
           ((packed >> 8) & 0xff) > threshold or \
           (packed & 0xff) > threshold


def rect_intersect(rect, width, height):
    x, y, w, h = rect
    x0 = max(0, x)
    y0 = max(0, y)
    x1 = min(width, x + w)
    y1 = min(height, y + h)
    if x1 <= x0 or y1 <= y0:
        return None, True
    return (x0, y0, x1 - x0, y1 - y0), (x0, y0, x1 - x0, y1 - y0) != tuple(rect)


def largest_component(body, width, height, region, threshold):
    """Largest 4-connected non-black component inside `region`.

    Returns None for an all-black region.  Iterative BFS with a bytearray
    visited map keeps memory at one byte per pixel and avoids recursion.
    """
    rx, ry, rw, rh = region
    visited = bytearray(rw * rh)
    best = None
    queue = collections.deque()
    for yy in range(rh):
        row = (ry + yy) * width + rx
        for xx in range(rw):
            start = yy * rw + xx
            if visited[start]:
                continue
            if not is_non_black(packed_at(body, row + xx), threshold):
                visited[start] = 1
                continue
            visited[start] = 1
            queue.append((xx, yy))
            count = 0
            minx = maxx = xx
            miny = maxy = yy
            while queue:
                cx, cy = queue.popleft()
                count += 1
                if cx < minx: minx = cx
                if cx > maxx: maxx = cx
                if cy < miny: miny = cy
                if cy > maxy: maxy = cy
                for nx, ny in ((cx - 1, cy), (cx + 1, cy),
                               (cx, cy - 1), (cx, cy + 1)):
                    if nx < 0 or ny < 0 or nx >= rw or ny >= rh:
                        continue
                    k = ny * rw + nx
                    if visited[k]:
                        continue
                    visited[k] = 1
                    if is_non_black(packed_at(body, (ry + ny) * width + rx + nx),
                                    threshold):
                        queue.append((nx, ny))
            if best is None or count > best['pixels']:
                best = dict(pixels=count,
                            bbox=[rx + minx, ry + miny,
                                  maxx - minx + 1, maxy - miny + 1],
                            touches_border=(minx == 0 or miny == 0 or
                                            maxx == rw - 1 or maxy == rh - 1))
    return best


def region_stats(body, width, height, rect, threshold, colour_cap,
                 want_component=True):
    """Counts and bbox for one rectangle; never scans outside the image."""
    clipped, was_clipped = rect_intersect(rect, width, height)
    row = dict(rect=list(rect), clipped=was_clipped)
    if clipped is None:
        row.update(pixels=0, non_black=0, fraction=0.0, bbox=None,
                   unique_colours=0, colours_capped=False,
                   mean_rgb=[0.0, 0.0, 0.0], max_rgb=[0, 0, 0],
                   top_colours=[], largest_component=None,
                   row_profile=[], col_profile=[])
        return row
    rx, ry, rw, rh = clipped
    row['rect'] = list(clipped)
    colours = set()
    capped = False
    non_black = 0
    minx = miny = 1 << 30
    maxx = maxy = -1
    sr = sg = sb = 0
    mr = mg = mb = 0
    counts = collections.Counter()
    row_profile = [0] * rh
    col_profile = [0] * rw
    for yy in range(rh):
        base = (ry + yy) * width + rx
        for xx in range(rw):
            packed = packed_at(body, base + xx)
            if len(colours) < colour_cap:
                colours.add(packed)
            elif packed not in colours:
                capped = True
            if not is_non_black(packed, threshold):
                continue
            non_black += 1
            row_profile[yy] += 1
            col_profile[xx] += 1
            if xx < minx: minx = xx
            if xx > maxx: maxx = xx
            if yy < miny: miny = yy
            if yy > maxy: maxy = yy
            r = (packed >> 16) & 0xff
            g = (packed >> 8) & 0xff
            b = packed & 0xff
            sr += r; sg += g; sb += b
            if r > mr: mr = r
            if g > mg: mg = g
            if b > mb: mb = b
            if len(counts) < 16 or packed in counts:
                counts[packed] += 1
    row['pixels'] = rw * rh
    row['non_black'] = non_black
    row['fraction'] = round(non_black / float(rw * rh), 6)
    row['unique_colours'] = len(colours)
    row['colours_capped'] = capped
    row['bbox'] = None if non_black == 0 else [rx + minx, ry + miny,
                                               maxx - minx + 1, maxy - miny + 1]
    if non_black:
        row['mean_rgb'] = [round(sr / float(non_black), 2),
                           round(sg / float(non_black), 2),
                           round(sb / float(non_black), 2)]
    else:
        row['mean_rgb'] = [0.0, 0.0, 0.0]
    row['max_rgb'] = [mr, mg, mb]
    row['top_colours'] = [
        dict(rgb='%06x' % c, count=n)
        for c, n in counts.most_common(8)
    ]
    row['row_profile'] = row_profile if rh <= 256 else summarise_profile(row_profile)
    row['col_profile'] = col_profile if rw <= 256 else summarise_profile(col_profile)
    row['largest_component'] = (largest_component(body, width, height, clipped,
                                                  threshold)
                                if want_component and non_black else None)
    return row


def summarise_profile(values):
    """Bound the JSON for a full 1920-wide frame: min/max/nonzero + deciles."""
    nonzero = sum(1 for v in values if v)
    peak = max(values) if values else 0
    if not values:
        return dict(length=0, nonzero=0, peak=0, deciles=[])
    step = max(1, len(values) // 10)
    deciles = [values[i] for i in range(0, len(values), step)][:10]
    return dict(length=len(values), nonzero=nonzero, peak=peak,
                deciles=deciles)


def parse_region(spec):
    """`x,y,w,h` or `name=x,y,w,h` -> (name, (x, y, w, h))."""
    name = None
    if '=' in spec:
        name, spec = spec.split('=', 1)
        name = name.strip() or None
    parts = spec.split(',')
    if len(parts) != 4:
        raise argparse.ArgumentTypeError(
            f'region must be [name=]x,y,w,h (got {spec!r})')
    try:
        rect = tuple(int(v.strip(), 0) for v in parts)
    except ValueError:
        raise argparse.ArgumentTypeError(f'region values must be integers: {spec!r}')
    if rect[2] <= 0 or rect[3] <= 0:
        raise argparse.ArgumentTypeError(f'region width/height must be positive: {spec!r}')
    return name, rect


def analyse(path, regions, threshold, colour_cap):
    width, height, maxval, body = read_ppm(path)
    full = region_stats(body, width, height, (0, 0, width, height), threshold,
                        colour_cap, want_component=True)
    doc = dict(
        file=str(path), width=width, height=height, maxval=maxval,
        threshold=threshold,
        unique_colours=full['unique_colours'],
        colours_capped=full['colours_capped'],
        non_black=full['non_black'],
        non_black_fraction=full['fraction'],
        bbox=full['bbox'],
        mean_rgb=full['mean_rgb'],
        max_rgb=full['max_rgb'],
        top_colours=full['top_colours'],
        largest_component=full['largest_component'],
        row_profile=full['row_profile'],
        col_profile=full['col_profile'],
        regions=[],
    )
    for i, (name, rect) in enumerate(regions):
        stats = region_stats(body, width, height, rect, threshold, colour_cap,
                             want_component=True)
        stats['name'] = name or f'region{i}'
        doc['regions'].append(stats)
    doc['summary'] = human_summary(doc)
    return doc


def human_summary(doc):
    if doc['non_black'] == 0:
        head = (f"{doc['file']}: {doc['width']}x{doc['height']} "
                f"colours={doc['unique_colours']} ALL BLACK")
    else:
        head = (f"{doc['file']}: {doc['width']}x{doc['height']} "
                f"colours={doc['unique_colours']} non_black={doc['non_black']} "
                f"({doc['non_black_fraction'] * 100:.2f}%) bbox={doc['bbox']}")
    lines = [head]
    lc = doc.get('largest_component')
    if lc:
        lines.append(f"  largest component: {lc['pixels']} px bbox={lc['bbox']}"
                     f"{' (touches border)' if lc['touches_border'] else ''}")
    for r in doc['regions']:
        if r['pixels'] == 0:
            lines.append(f"  {r['name']}: empty (rect {r['rect']} outside frame)")
            continue
        lines.append(
            f"  {r['name']}: {r['rect'][2]}x{r['rect'][3]} at "
            f"({r['rect'][0]},{r['rect'][1]}) colours={r['unique_colours']} "
            f"non_black={r['non_black']} ({r['fraction'] * 100:.2f}%) "
            f"bbox={r['bbox']} mean_rgb={r['mean_rgb']}")
    return '\n'.join(lines)


def synthetic_ppm(path):
    """64x48 black frame with a 10x8 red block and a separate 3x3 blue block."""
    width, height = 64, 48
    body = bytearray(width * height * 3)
    for y in range(6, 14):
        for x in range(5, 15):
            o = (y * width + x) * 3
            body[o:o + 3] = b'\xff\x00\x00'
    for y in range(30, 33):
        for x in range(40, 43):
            o = (y * width + x) * 3
            body[o:o + 3] = b'\x00\x00\xff'
    pathlib.Path(path).write_bytes(
        f'P6\n{width} {height}\n255\n'.encode() + bytes(body))
    return width, height


def self_test():
    tmp = pathlib.Path(tempfile.mkdtemp(prefix='analyze-frame-selftest-'))
    ppm = tmp / 'synthetic.ppm'
    width, height = synthetic_ppm(ppm)
    doc = analyse(ppm, [('red', (5, 6, 10, 8)), ('blue', (40, 30, 3, 3)),
                        ('empty', (0, 0, 4, 4)), ('outside', (100, 100, 8, 8))],
                  0, DEFAULT_COLOUR_CAP)
    problems = []
    if doc['width'] != width or doc['height'] != height:
        problems.append('size')
    if doc['unique_colours'] != 3:
        problems.append(f"unique_colours={doc['unique_colours']} != 3")
    if doc['non_black'] != 80 + 9:
        problems.append(f"non_black={doc['non_black']} != 89")
    if doc['bbox'] != [5, 6, 38, 27]:
        problems.append(f"bbox={doc['bbox']} != [5, 6, 38, 27]")
    lc = doc['largest_component']
    if not lc or lc['pixels'] != 80 or lc['bbox'] != [5, 6, 10, 8]:
        problems.append(f'largest_component={lc}')
    by_name = {r['name']: r for r in doc['regions']}
    if by_name['red']['unique_colours'] != 1 or by_name['red']['non_black'] != 80:
        problems.append('red region stats')
    if by_name['red']['bbox'] != [5, 6, 10, 8]:
        problems.append('red region bbox')
    if by_name['empty']['non_black'] != 0 or by_name['empty']['bbox'] is not None:
        problems.append('empty region stats')
    if not by_name['outside']['clipped'] or by_name['outside']['pixels'] != 0:
        problems.append('outside region clipping')
    # P3 round-trip keeps the same counts.
    p3 = tmp / 'synthetic.ppm.p3'
    vals = []
    body = ppm.read_bytes().split(b'\n', 3)[3]
    for i in range(0, len(body), 3):
        vals.extend(str(v) for v in body[i:i + 3])
    p3.write_text(f'P3\n{width} {height}\n255\n' + ' '.join(vals) + '\n')
    doc3 = analyse(p3, [], 0, DEFAULT_COLOUR_CAP)
    if doc3['unique_colours'] != 3 or doc3['non_black'] != 89:
        problems.append('P3 parsing')
    if problems:
        print('self-test FAIL: ' + '; '.join(problems), file=sys.stderr)
        print(f'  artefacts: {tmp}', file=sys.stderr)
        return 1
    print(f'self-test PASS: {width}x{height} synthetic frame, '
          f'colours=3 non_black=89 largest=80px')
    print(f'  artefacts: {tmp}')
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('frame', nargs='?', type=pathlib.Path,
                   help='PPM frame (P6 or P3)')
    p.add_argument('--region', action='append', default=[],
                   type=parse_region, metavar='[name=]x,y,w,h',
                   help='rectangle to analyse; repeatable')
    p.add_argument('--json', type=pathlib.Path,
                   help='write the full JSON report here (default stdout only)')
    p.add_argument('--threshold', type=int, default=0,
                   help='channel value above which a pixel counts as non-black (default 0)')
    p.add_argument('--colour-cap', type=int, default=DEFAULT_COLOUR_CAP,
                   help=f'maximum distinct colours to track (default {DEFAULT_COLOUR_CAP})')
    p.add_argument('--no-component', action='store_true',
                   help='skip the connected-component pass (row/col profiles only)')
    p.add_argument('--self-test', action='store_true',
                   help='generate a synthetic PPM under /tmp and check the analyzer')
    a = p.parse_args(argv)
    if a.self_test:
        return self_test()
    if a.frame is None:
        p.error('FRAME is required unless --self-test is used')
    try:
        doc = analyse(a.frame, a.region, a.threshold, a.colour_cap)
    except (OSError, PPMError) as e:
        print(f'analyze_frame: {e}', file=sys.stderr)
        return 1
    if a.no_component:
        doc['largest_component'] = None
        for r in doc['regions']:
            r['largest_component'] = None
        doc['summary'] = human_summary(doc)
    if a.json:
        a.json.parent.mkdir(parents=True, exist_ok=True)
        a.json.write_text(json.dumps(doc, indent=2) + '\n')
    print(json.dumps(doc, indent=2))
    print(doc['summary'], file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
