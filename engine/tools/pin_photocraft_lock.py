#!/usr/bin/env python3
"""Hold engine/raw-photocraft/Cargo.lock to the crate versions PhotoCraft locks.

    python3 engine/tools/pin_photocraft_lock.py           # create or re-pin the lock file
    python3 engine/tools/pin_photocraft_lock.py --check   # fail if it has drifted

The PhotoCraft revision in engine/third_party/photocraft-raw/PIN was built and
tested by its authors with the versions in its own Cargo.lock. That lock file
belongs to a workspace of thirty crates and cannot be used directly, so the
setup script stages it as build/deps/photocraft-raw/photocraft-Cargo.lock and
this script makes ours agree with it: every registry package in our lock that
PhotoCraft also locks is moved to PhotoCraft's version (`cargo update
--precise`). A package PhotoCraft does not lock at all is reported; there
should be none, since spk-photocraft-raw adds no dependency of its own.

Writing needs the crates.io index (cargo resolves); --check reads two files
and needs nothing. Run cargo from the crate directory so rustup applies
rust-toolchain.toml. Python 3.11+ (tomllib), standard library only.
"""
import argparse
import subprocess
import sys
import tomllib
from pathlib import Path

ENGINE = Path(__file__).resolve().parent.parent
CRATE = ENGINE / "raw-photocraft"
THEIRS = ENGINE.parent / "build" / "deps" / "photocraft-raw" / "photocraft-Cargo.lock"


def registry_packages(path):
    """name -> set of versions, for packages that come from a registry."""
    with open(path, "rb") as f:
        lock = tomllib.load(f)
    found = {}
    for package in lock.get("package", []):
        if package.get("source", "").startswith("registry+"):
            found.setdefault(package["name"], set()).add(package["version"])
    return found


def key(version):
    return tuple(int(part) if part.isdigit() else part for part in version.replace("-", ".").replace("+", ".").split("."))


def compatible(ours, theirs):
    """The version of `theirs` cargo could substitute for `ours`: same major, or same minor
    below 1.0 (cargo's caret rule). The highest, when PhotoCraft locks several."""
    def line(version):
        parts = version.split(".")
        return parts[0] if parts[0] != "0" else ".".join(parts[:2])
    same = sorted((v for v in theirs if line(v) == line(ours)), key=key)
    return same[-1] if same else None


def drift(ours, theirs):
    moves, unknown, other_line = [], [], []
    for name, versions in sorted(ours.items()):
        if name not in theirs:
            unknown.append(f"{name} {', '.join(sorted(versions))}")
            continue
        for version in sorted(versions):
            if version in theirs[name]:
                continue
            target = compatible(version, theirs[name])
            if target:
                moves.append((name, version, target))
            else:
                other_line.append(f"{name} {version} (PhotoCraft locks {', '.join(sorted(theirs[name]))})")
    return moves, unknown, other_line


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="verify only; write nothing")
    parser.add_argument("--cargo", default="cargo")
    args = parser.parse_args()
    lock = CRATE / "Cargo.lock"
    if not THEIRS.exists():
        sys.exit(f"{THEIRS} is missing; run engine/setup-photocraft-raw.sh (or .ps1) first")
    theirs = registry_packages(THEIRS)

    if not args.check:
        if not lock.exists():
            subprocess.run([args.cargo, "generate-lockfile"], cwd=CRATE, check=True)
        # A move can change what its dependents resolve to; repeat until nothing moves.
        for _ in range(8):
            moves, _, _ = drift(registry_packages(lock), theirs)
            if not moves:
                break
            for name, version, target in moves:
                print(f"pin {name} {version} -> {target}")
                subprocess.run([args.cargo, "update", "--package", f"{name}@{version}", "--precise", target],
                               cwd=CRATE, check=True)

    if not lock.exists():
        sys.exit(f"{lock} is missing; run this script without --check where crates.io is reachable")
    ours = registry_packages(lock)
    moves, unknown, other_line = drift(ours, theirs)
    for name, version, target in moves:
        print(f"drift: {name} {version}, PhotoCraft locks {target}")
    for item in other_line:
        print(f"no compatible locked version: {item}")
    for item in unknown:
        print(f"not in PhotoCraft's lock: {item}")
    if moves or unknown or other_line:
        sys.exit("engine/raw-photocraft/Cargo.lock does not match PhotoCraft's locked versions")
    print(f"{sum(len(v) for v in ours.values())} registry packages, all at PhotoCraft's locked versions")


if __name__ == "__main__":
    main()
