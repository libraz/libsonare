"""Write `<dir>/BASELINE`: which HEAD, which uncommitted sources and which link the shared fit library is.

`make baseline` is the one writer of `build-tuning/`; this records what it built so
a fit or a gate written against that library can name it, dirty files included.

    python tools/voicematch/baseline_record.py build-tuning
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _git(*args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(REPO), *args], capture_output=True, text=True, check=False
    ).stdout


def library(build_dir: Path) -> Path:
    for name in ("libsonare.dylib", "libsonare.so"):
        if (build_dir / "lib" / name).exists():
            return build_dir / "lib" / name
    raise FileNotFoundError(f"no libsonare under {build_dir / 'lib'}")


def record(build_dir: Path) -> dict:
    dylib = library(build_dir).resolve()
    dirty = _git("status", "--porcelain", "--", "src").splitlines()
    return {
        "head": _git("rev-parse", "--short", "HEAD").strip(),
        "dirty": sorted(line[3:] for line in dirty),
        "linked_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(dylib.stat().st_mtime)),
        "dylib": str(dylib),
    }


def read(build_dir: Path) -> dict | None:
    """The recorded baseline beside @p build_dir's library, or None if it was built otherwise."""
    path = build_dir / "BASELINE"
    return json.loads(path.read_text()) if path.exists() else None


def main(argv: list[str]) -> int:
    build_dir = REPO / (argv[0] if argv else "build-tuning")
    rec = record(build_dir)
    (build_dir / "BASELINE").write_text(json.dumps(rec, indent=1) + "\n")
    print(f"baseline {rec['head']} linked {rec['linked_utc']}, {len(rec['dirty'])} dirty file(s)")
    for f in rec["dirty"]:
        print(f"  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
