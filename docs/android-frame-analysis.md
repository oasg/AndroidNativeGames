# Frame analysis for the Android UI gates

`games/totk/android/tools/analyze_frame.py` is a dependency-free PPM analyzer
for the Wave 4 gates.  It reads a binary (P6) or ASCII (P3) PPM, reports the
numbers the contract asks for, and writes JSON plus a short human summary.  It
never reads game assets; point it at a frame captured on the device.

## Usage

```sh
adb pull /data/local/tmp/totk-android/lanes/shader/ui-frame.ppm /tmp/ui-frame.ppm

python3 games/totk/android/tools/analyze_frame.py /tmp/ui-frame.ppm \
  --region window=0,0,1920,1080 \
  --region target3f=0,0,1920,1080 \
  --region ui-source=0,0,360,69 \
  --json /tmp/ui-frame.json
```

`--region` is repeatable and accepts `x,y,w,h` or `name=x,y,w,h`.  A region is
clipped to the image; an out-of-frame rectangle is reported with
`clipped: true` and zero pixels instead of failing.  The coordinate origin is
the top-left pixel.  Substitute the rectangle Lane 4 recorded for the 360x69
source; the analyzer only needs the rectangle, not a semantic name.

Useful options:

- `--threshold N`: a pixel counts as non-black when any channel is above `N`
  (default `0`).  Use `--threshold 2` to ignore one-bit noise.
- `--colour-cap N`: maximum distinct colours tracked (default 65536).  When
  the cap is hit, `colours_capped` is true and `unique_colours` is the number
  tracked so far.
- `--no-component`: skip the connected-component pass (row/column profiles
  only) for a faster run.
- `--self-test`: generate a synthetic 64x48 PPM under `/tmp`, check the
  analyzer, and exit.  No game data is involved.

## What it reports

Top level:

- `width`, `height`, `maxval`, `threshold`;
- `unique_colours`, `colours_capped`;
- `non_black` (pixel count), `non_black_fraction`, `bbox` (`[x,y,w,h]` of all
  non-black pixels, or `null`);
- `mean_rgb`, `max_rgb`, `top_colours` (up to 8 packed `rrggbb` values);
- `largest_component`: the largest 4-connected non-black component inside the
  full frame (`pixels`, `bbox`, `touches_border`), or `null`;
- `row_profile` / `col_profile`: per-row/per-column non-black counts.  A frame
  wider/taller than 256 pixels is summarised as `length`, `nonzero`, `peak`
  and ten deciles to keep the JSON small;
- `regions[]`: the same statistics for each `--region`, plus `name`, `rect`
  and `clipped`;
- `summary`: the short human text (also printed to stderr).

## Gate mapping

| contract gate | check |
| --- | --- |
| 360x69 UI source `first_nz != none` | `regions[ui-source].non_black > 0` |
| `0x3f` target not all zero | `regions[target3f].non_black > 0` |
| 1920x1080 window has many colours | `regions[window].unique_colours > 1` |
| title / save-slot visible, not a strip | window `bbox` and `largest_component.bbox` cover the expected title area; `top_colours` is not a single colour |
| A/Continue reaches a 3D frame | window `unique_colours` grows and the largest component is not the loading strip |

A single `colours=1` frame is never a pass: the contract explicitly says so.
Compare the `--region` numbers before and after an input script step.

## Example output

```text
/tmp/ui-frame.ppm: 1920x1080 colours=762 non_black=1200 (0.06%) bbox=[0, 0, 1920, 96]
  largest component: 900 px bbox=[0, 0, 1920, 12]
  window: 1920x1080 at (0,0) colours=762 non_black=1200 (0.06%) bbox=[0, 0, 1920, 96] mean_rgb=[13.2, 20.1, 30.4]
  ui-source: 360x69 at (0,0) colours=1 non_black=0 (0.00%) bbox=None mean_rgb=[0.0, 0.0, 0.0]
```

The exact numbers depend on the run; this block only shows the shape.

## Self-test

```sh
python3 games/totk/android/tools/analyze_frame.py --self-test
```

Expected:

```text
self-test PASS: 64x48 synthetic frame, colours=3 non_black=89 largest=80px
```

The synthetic frame is a black 64x48 image with a 10x8 red block at (5,6) and
a separate 3x3 blue block at (40,30).  The test checks the colour count, the
non-black bbox, the largest component, per-region stats, out-of-frame
clipping, and a P3 round-trip.  Artefacts are written under a temporary
directory in `/tmp`; nothing is committed.
