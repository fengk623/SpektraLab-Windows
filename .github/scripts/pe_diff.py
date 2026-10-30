#!/usr/bin/env python3
"""Compare two builds of a Windows executable byte for byte, and say where they differ.

    python .github/scripts/pe_diff.py A.exe B.exe [--name-a X] [--name-b Y] [--known LABEL=SHA256 ...]

Prints each file's size and SHA-256 (naming any --known build it matches). When the two differ,
prints every run of differing bytes (runs closer than 16 bytes are merged) with the PE section it
falls in and the bytes of both files around it, as hex and as text, so a timestamp, a path or a
line ending in the binary can be recognised. Each finding is also written as a GitHub Actions
notice (at most 8), because a run's notices can be read without downloading its log.

Exit status: 0 when the files are identical, 1 when they differ. Standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys


def sections(b: bytes) -> list[tuple[str, int, int]]:
    """(name, file offset, raw size) of each PE section, plus the headers."""
    try:
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        if b[pe:pe + 4] != b"PE\0\0":
            return []
        n = struct.unpack_from("<H", b, pe + 6)[0]
        opt = struct.unpack_from("<H", b, pe + 20)[0]
        out = [("(headers)", 0, struct.unpack_from("<I", b, pe + 24 + 60)[0])]
        base = pe + 24 + opt
        for i in range(n):
            name, _vsz, _va, rsz, rptr = struct.unpack_from("<8sIIII", b, base + 40 * i)
            out.append((name.rstrip(b"\0").decode("ascii", "replace"), rptr, rsz))
        return out
    except struct.error:
        return []


def section_of(secs: list[tuple[str, int, int]], off: int) -> str:
    for name, start, size in secs:
        if start <= off < start + size:
            return f"{name}+0x{off - start:x}"
    return "(outside sections)"


def runs(a: bytes, b: bytes, gap: int = 16) -> list[tuple[int, int]]:
    n = min(len(a), len(b))
    out: list[tuple[int, int]] = []
    i = 0
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and (a[j] != b[j] or any(a[k] != b[k] for k in range(j, min(j + gap, n)))):
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    if len(a) != len(b):
        out.append((n, max(len(a), len(b))))
    return out


def show(buf: bytes, start: int, end: int) -> str:
    chunk = buf[start:end]
    text = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
    return f"{chunk.hex(' ')} |{text}|"


def notice(title: str, msg: str) -> None:
    if os.environ.get("GITHUB_ACTIONS"):
        esc = lambda t: t.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        prop = lambda t: esc(t).replace(":", "%3A").replace(",", "%2C")
        print(f"::notice title={prop(title)}::{esc(msg)}")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--name-a", default="A")
    ap.add_argument("--name-b", default="B")
    ap.add_argument("--known", action="append", default=[], help="LABEL=SHA256 of a build made elsewhere")
    args = ap.parse_args(argv)
    known = dict(reversed(k.split("=", 1)) for k in args.known)  # sha -> label
    a, b = open(args.a, "rb").read(), open(args.b, "rb").read()
    for name, buf in ((args.name_a, a), (args.name_b, b)):
        h = hashlib.sha256(buf).hexdigest()
        tag = f" (= {known[h]})" if h in known else ""
        print(f"{name}: {len(buf)} bytes, SHA-256 {h}{tag}")
        notice(f"pe_diff {name}", f"{len(buf)} bytes, SHA-256 {h}{tag}")
    if a == b:
        print("identical")
        notice("pe_diff", f"{args.name_a} and {args.name_b} are identical")
        return 0
    secs = sections(a)
    rs = runs(a, b)
    total = sum(e - s for s, e in rs)
    print(f"differ: {len(rs)} run(s), {total} byte(s)")
    notice("pe_diff", f"{args.name_a} and {args.name_b} differ: {len(rs)} run(s), {total} byte(s)")
    for k, (s, e) in enumerate(rs[:40]):
        lo, hi = max(0, s - 16), min(max(len(a), len(b)), e + 16)
        where = section_of(secs, s)
        print(f"\n#{k + 1} offset 0x{s:x}-0x{e:x} ({e - s} bytes) in {where}")
        print(f"  {args.name_a}: {show(a, lo, hi)}")
        print(f"  {args.name_b}: {show(b, lo, hi)}")
        if k < 7:
            notice(f"pe_diff #{k + 1} {where} ({e - s} B)",
                   f"{args.name_a} {show(a, max(0, s - 8), min(len(a), e + 8))} / {args.name_b} {show(b, max(0, s - 8), min(len(b), e + 8))}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
