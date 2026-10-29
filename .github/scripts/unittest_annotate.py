#!/usr/bin/env python3
"""Run `python -m unittest discover` and turn each failure into a GitHub annotation.

    python .github/scripts/unittest_annotate.py -s engine/tests -p "test_*.py"

The full output still goes to the log. On failure, each FAIL / ERROR block
(the first 10) also becomes an `::error` annotation, so the failing test and
its traceback show on the run's summary page and through the check-runs API,
without anyone having to open the raw log. The exit status is unittest's.
Standard library only.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys


def escape(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "").replace("\n", "%0A")


def escape_property(text: str) -> str:
    return escape(text).replace(":", "%3A").replace(",", "%2C")


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # a Windows runner's console is cp1252
    except (AttributeError, ValueError):
        pass
    cmd = [sys.executable, "-m", "unittest", "discover", "-v", *argv]
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    output = proc.stdout + proc.stderr
    sys.stdout.write(output)
    sys.stdout.flush()
    if proc.returncode == 0:
        return 0
    blocks = re.split(r"^={50,}$", output, flags=re.M)[1:]
    for block in blocks[:10]:
        block = re.split(r"^-{50,}$\n(?=Ran )", block, flags=re.M)[0].strip()
        title = block.splitlines()[0] if block else "unittest"
        print(f"::error title={escape_property(title)[:200]}::{escape(block)[:8000]}")
    if not blocks:
        print(f"::error title=unittest exited {proc.returncode}::{escape(output[-8000:])}")
    summary = [l for l in output.splitlines() if l.startswith(("Ran ", "FAILED", "OK"))]
    print(f"::error title=unittest summary::{escape(' / '.join(summary) or 'no summary')}")
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
