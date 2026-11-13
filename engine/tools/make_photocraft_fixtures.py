#!/usr/bin/env python3
"""Synthetic DNG fixtures for the PhotoCraft RAW path, and what they must develop to.

    python3 engine/tools/make_photocraft_fixtures.py          # (re)write the fixtures
    python3 engine/tools/make_photocraft_fixtures.py --check  # fail if the tracked ones differ

Writes, under engine/tests/fixtures/photocraft/:

    patches.dng        96 x 64 Bayer RGGB, 16-bit uncompressed, six flat 32 x 32 patches
    patches_rot6.dng   the same sensor data with TIFF Orientation 6
    expected.h         the linear ProPhoto value of each patch, for the C++ tests

and engine/raw-photocraft/tests/expected.rs, the same numbers for the Rust tests.

Standard library only, and deliberately not a port of the code under test: the
expected values come from the DNG specification's camera -> XYZ (D50) mapping
(chapter 6, the path without a ForwardMatrix) written out here with plain 3 x 3
arithmetic, so a test that compares against them is comparing two
implementations. A flat patch demosaics to itself away from its edges, whatever
the algorithm, which is what makes the interior of each patch an exact target.

Fixture design, and why:

* BlackLevel 256, WhiteLevel 4095: levels that are not 0 and 65535, so a reader
  that ignores either is wrong by a visible amount.
* AsShotNeutral is the camera response to a D55 white under the ColorMatrix, so
  white balance and chromatic adaptation both do real work (a D65 neutral would
  make the adaptation an identity for a D65 matrix... and D50 would for D50).
* The "clip" patch sits at the white level in all three colours. It is the
  highlight case (white-balanced, clipped, it must come out neutral at exactly
  1.0), and it also keeps PhotoCraft's clipped-sample detector from mistaking
  the brightest flat patch for a sensor that saturates below its white level.
* The "blue" patch is a camera colour outside ProPhoto: its red comes out
  negative. A path that clamps to 0..1 loses it.
* BaselineExposure +1: applied, the clip patch is 2.0 (headroom above 1); not
  applied (the host's default), 1.0.
"""
import argparse
import struct
import sys
from pathlib import Path

WIDTH, HEIGHT, PATCH = 96, 64, 32
BLACK, WHITE = 256, 4095
BASELINE_EXPOSURE = 1.0
MAKE, MODEL = "SpektraLab", "Fixture Patches"

# XYZ -> camera, DNG ColorMatrix1, for CalibrationIlluminant1 = 21 (D65).
COLOR_MATRIX = [
    [0.7400, -0.2100, -0.0600],
    [-0.4300, 1.2300, 0.2100],
    [-0.0500, 0.1500, 0.6200],
]
D65_XY = (0.3127, 0.3290)
D55_XY = (0.3324, 0.3474)
D50_XYZ = (0.9642, 1.0, 0.8249)
D50_XY = (0.3457, 0.3585)
ROMM = ((0.7347, 0.2653), (0.1596, 0.8404), (0.0366, 0.0001))
BRADFORD = [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]

# name, camera R G B as a fraction of the range above black (before white balance).
# The two greys are filled in below from the neutral, so they are exactly neutral.
PATCHES = [
    ("dark_grey", None, 0.05),
    ("mid_grey", None, 0.18),
    ("clip", (1.0, 1.0, 1.0), None),
    ("red", (0.30, 0.12, 0.03), None),
    ("green", (0.06, 0.45, 0.08), None),
    ("blue", (0.0, 0.02, 0.50), None),
]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def apply(m, v):
    return [sum(m[i][k] * v[k] for k in range(3)) for i in range(3)]


def invert(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def diag(v):
    return [[v[0], 0, 0], [0, v[1], 0], [0, 0, v[2]]]


def xy_to_xyz(xy):
    return [xy[0] / xy[1], 1.0, (1.0 - xy[0] - xy[1]) / xy[1]]


def bradford(src, dst):
    s, d = apply(BRADFORD, src), apply(BRADFORD, dst)
    return mul(invert(BRADFORD), mul(diag([d[k] / s[k] for k in range(3)]), BRADFORD))


def rgb_to_xyz(primaries, white_xy):
    cols = [xy_to_xyz(p) for p in primaries]
    p = [[cols[j][i] for j in range(3)] for i in range(3)]
    return mul(p, diag(apply(invert(p), xy_to_xyz(white_xy))))


def neutral():
    """AsShotNeutral: the camera's response to a D55 white, largest component 1, as the
    six-decimal rationals the file stores."""
    n = apply(COLOR_MATRIX, xy_to_xyz(D55_XY))
    top = max(n)
    return [round(v / top * 1_000_000) / 1_000_000 for v in n]


def balanced_camera_to_prophoto(n):
    """White-balanced camera RGB -> linear ProPhoto, DNG spec chapter 6 without a
    ForwardMatrix: invert the ColorMatrix, adapt from the as-shot white to D50
    (Bradford), undo the balance, scale so the balanced white has Y = 1."""
    cam_to_xyz = invert(COLOR_MATRIX)
    white = apply(cam_to_xyz, n)
    total = sum(white)
    white_xy = (white[0] / total, white[1] / total)
    m = mul(mul(bradford(xy_to_xyz(white_xy), list(D50_XYZ)), cam_to_xyz), diag([v / max(n) for v in n]))
    y = apply(m, [1.0, 1.0, 1.0])[1]
    m = [[v / y for v in row] for row in m]
    return mul(invert(rgb_to_xyz(ROMM, D50_XY)), m)


def sensor_counts(n):
    """The raw count of each patch and colour."""
    out = []
    for name, camera, grey in PATCHES:
        if camera is None:
            # A grey of balanced level `grey`: camera = grey * neutral.
            camera = [grey * v for v in n]
        out.append((name, [BLACK + round(v * (WHITE - BLACK)) for v in camera]))
    return out


def expected(n):
    """Linear ProPhoto of each patch from its stored counts, baseline exposure not applied."""
    m = balanced_camera_to_prophoto(n)
    mult = [1.0 / v for v in n]
    low = min(mult)
    mult = [v / low for v in mult]
    rows = []
    for name, counts in sensor_counts(n):
        balanced = [min(max((c - BLACK) / (WHITE - BLACK) * mult[k], 0.0), 1.0) for k, c in enumerate(counts)]
        rows.append((name, apply(m, balanced)))
    return rows, mult


def sensor_plane(n):
    counts = sensor_counts(n)
    plane = bytearray()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            patch = counts[(y // PATCH) * (WIDTH // PATCH) + x // PATCH][1]
            colour = (0, 1, 1, 2)[(y % 2) * 2 + x % 2]   # RGGB
            plane += struct.pack("<H", patch[colour])
    return bytes(plane)


def dng(orientation, n):
    """A little-endian classic TIFF with the raw image in IFD0."""
    BYTE, ASCII, SHORT, LONG, RATIONAL, SRATIONAL = 1, 2, 3, 4, 5, 10
    size = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8, SRATIONAL: 8}
    code = {BYTE: "B", SHORT: "H", LONG: "I"}

    def encode(kind, values):
        if kind == ASCII:
            return values.encode("ascii") + b"\0"
        if kind == RATIONAL:
            return b"".join(struct.pack("<II", round(v * 1_000_000), 1_000_000) for v in values)
        if kind == SRATIONAL:
            return b"".join(struct.pack("<ii", round(v * 10_000), 10_000) for v in values)
        return b"".join(struct.pack("<" + code[kind], v) for v in values)

    plane = sensor_plane(n)
    entries = [
        (254, LONG, [0]),                         # NewSubFileType: the full-resolution image
        (256, LONG, [WIDTH]),
        (257, LONG, [HEIGHT]),
        (258, SHORT, [16]),
        (259, SHORT, [1]),                        # uncompressed
        (262, SHORT, [32803]),                    # CFA
        (271, ASCII, MAKE),
        (272, ASCII, MODEL),
        (273, LONG, [0]),                         # StripOffsets, patched below
        (274, SHORT, [orientation]),
        (277, SHORT, [1]),
        (278, LONG, [HEIGHT]),
        (279, LONG, [len(plane)]),
        (284, SHORT, [1]),
        (306, ASCII, "2026:11:12 12:00:00"),      # DateTime
        (33421, SHORT, [2, 2]),                   # CFARepeatPatternDim
        (33422, BYTE, [0, 1, 1, 2]),              # CFAPattern: RGGB
        (50706, BYTE, [1, 4, 0, 0]),              # DNGVersion
        (50707, BYTE, [1, 1, 0, 0]),              # DNGBackwardVersion
        (50708, ASCII, MAKE + " " + MODEL),       # UniqueCameraModel
        (50710, BYTE, [0, 1, 2]),                 # CFAPlaneColor
        (50711, SHORT, [1]),                      # CFALayout: rectangular
        (50713, SHORT, [1, 1]),                   # BlackLevelRepeatDim
        (50714, SHORT, [BLACK]),
        (50717, SHORT, [WHITE]),
        (50721, SRATIONAL, [v for row in COLOR_MATRIX for v in row]),
        (50728, RATIONAL, n),                     # AsShotNeutral
        (50730, SRATIONAL, [BASELINE_EXPOSURE]),
        (50778, SHORT, [21]),                     # CalibrationIlluminant1: D65
    ]
    entries.sort(key=lambda e: e[0])
    ifd_at = 8
    data_at = ifd_at + 2 + 12 * len(entries) + 4
    blobs, fields = b"", []
    for tag, kind, values in entries:
        payload = encode(kind, values)
        count = len(payload) // size[kind]
        if len(payload) <= 4:
            fields.append((tag, kind, count, payload.ljust(4, b"\0")))
        else:
            fields.append((tag, kind, count, struct.pack("<I", data_at + len(blobs))))
            blobs += payload + (b"\0" if len(payload) % 2 else b"")
    strip_at = data_at + len(blobs)
    out = bytearray(b"II*\0" + struct.pack("<I", ifd_at) + struct.pack("<H", len(fields)))
    for tag, kind, count, value in fields:
        if tag == 273:
            value = struct.pack("<I", strip_at)
        out += struct.pack("<HHI", tag, kind, count) + value
    out += struct.pack("<I", 0) + blobs + plane
    return bytes(out)


def numbers(values):
    """Nine significant digits, always with a decimal point (a Rust f64 literal needs one)."""
    def one(v):
        text = f"{v:.9g}"
        return text if any(c in text for c in ".e") else text + ".0"
    return ", ".join(one(v) for v in values)


def header(rows, mult, n):
    lines = [
        "// Generated by engine/tools/make_photocraft_fixtures.py; do not edit.",
        "// What engine/tests/fixtures/photocraft/patches.dng must develop to: linear ProPhoto RGB",
        "// (ROMM, D50) of each flat 32 x 32 patch, BaselineExposure not applied. The reference is the",
        "// DNG specification's mapping written out in that script, not the code under test.",
        "#pragma once",
        "",
        "namespace spk_fixture {",
        f"constexpr int kWidth = {WIDTH}, kHeight = {HEIGHT}, kPatch = {PATCH}, kPatchColumns = {WIDTH // PATCH};",
        f"constexpr double kBlack = {BLACK}, kWhite = {WHITE}, kBaselineExposureEv = {BASELINE_EXPOSURE};",
        f"constexpr double kAsShotNeutral[3] = {{{numbers(n)}}};",
        f"constexpr double kWbMultipliers[3] = {{{numbers(mult)}}};",
        f"constexpr double kColorMatrix[9] = {{{numbers(v for row in COLOR_MATRIX for v in row)}}};",
        "struct Patch { const char* name; double rgb[3]; };",
        f"constexpr Patch kPatches[{len(rows)}] = {{",
    ]
    lines += [f'    {{"{name}", {{{numbers(rgb)}}}}},' for name, rgb in rows]
    lines += ["};", "}  // namespace spk_fixture", ""]
    return "\n".join(lines)


def rust(rows, mult, n):
    lines = [
        "// Generated by engine/tools/make_photocraft_fixtures.py; do not edit.",
        "// What engine/tests/fixtures/photocraft/patches.dng must develop to: linear ProPhoto RGB",
        "// (ROMM, D50) of each flat 32 x 32 patch, BaselineExposure not applied. The reference is the",
        "// DNG specification's mapping written out in that script, not the code under test.",
        f"pub const WIDTH: usize = {WIDTH};",
        f"pub const HEIGHT: usize = {HEIGHT};",
        f"pub const PATCH: usize = {PATCH};",
        f"pub const BLACK: f64 = {BLACK}.0;",
        f"pub const WHITE: f64 = {WHITE}.0;",
        f"pub const BASELINE_EXPOSURE_EV: f64 = {BASELINE_EXPOSURE};",
        f"pub const AS_SHOT_NEUTRAL: [f64; 3] = [{numbers(n)}];",
        f"pub const WB_MULTIPLIERS: [f64; 3] = [{numbers(mult)}];",
        f"pub const COLOR_MATRIX: [f64; 9] = [{numbers(v for row in COLOR_MATRIX for v in row)}];",
        f"pub const PATCHES: [(&str, [f64; 3]); {len(rows)}] = [",
    ]
    lines += [f'    ("{name}", [{numbers(rgb)}]),' for name, rgb in rows]
    lines += ["];", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare with the tracked files instead of writing")
    args = parser.parse_args()
    engine = Path(__file__).resolve().parent.parent
    fixtures = engine / "tests" / "fixtures" / "photocraft"
    n = neutral()
    rows, mult = expected(n)
    if not any(v < 0 for v in dict(rows)["blue"]):
        sys.exit("the blue patch no longer leaves ProPhoto; the negative-value case is gone")
    outputs = {
        fixtures / "patches.dng": dng(1, n),
        fixtures / "patches_rot6.dng": dng(6, n),
        fixtures / "expected.h": header(rows, mult, n).encode(),
        engine / "raw-photocraft" / "tests" / "expected.rs": rust(rows, mult, n).encode(),
    }
    stale = []
    for path, content in outputs.items():
        if args.check:
            tracked = path.read_bytes() if path.exists() else None
            # Text fixtures may be checked out with CRLF.
            if tracked != content and not (path.suffix in (".h", ".rs") and tracked is not None
                                           and tracked.replace(b"\r\n", b"\n") == content):
                stale.append(path)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
            print(f"wrote {path.relative_to(engine.parent)} ({len(content)} bytes)")
    if stale:
        sys.exit("stale PhotoCraft fixtures, rerun engine/tools/make_photocraft_fixtures.py: "
                 + ", ".join(str(p.relative_to(engine.parent)) for p in stale))
    for name, rgb in rows:
        print(f"{name:10s} {numbers(rgb)}")


if __name__ == "__main__":
    main()
