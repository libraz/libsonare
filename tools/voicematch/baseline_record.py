"""Publish the HEAD build into `build-tuning/lib` and write `build-tuning/BASELINE`.

`make baseline` builds the library from a mirror of HEAD (`tools/head_mirror.sh`)
and this is the one writer of `build-tuning/`: it replaces the library files in
place by rename, so a run that already loaded the old one keeps it, and records
which HEAD the library is so a fit or a gate written against it can name it.

    python tools/voicematch/baseline_record.py <head build dir> <head mirror dir> build-tuning
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # tools/ for _repo

from _repo import shared_library_file

REPO = Path(__file__).resolve().parents[2]


def library(build_dir: Path) -> Path:
    found = shared_library_file(build_dir)
    if found.exists():
        return found
    raise FileNotFoundError(f"no libsonare under {build_dir / 'lib'}")


def publish(built: Path, dest: Path) -> Path:
    """Copy @p built's library and its version symlinks into @p dest/lib by rename."""
    src_lib = library(built)
    real = src_lib.resolve()
    out = dest / "lib"
    out.mkdir(parents=True, exist_ok=True)
    tmp = out / f".{real.name}.tmp"
    shutil.copy2(real, tmp)
    os.replace(tmp, out / real.name)
    # The symlink chain (libsonare.dylib -> libsonare.1.dylib -> libsonare.1.10.0.dylib; .so, .so.1, .so.1.10.0 on Linux).
    for link in sorted(src_lib.parent.glob("libsonare*")):
        if link.is_symlink():
            tmp = out / f".{link.name}.tmp"
            if tmp.is_symlink() or tmp.exists():
                tmp.unlink()
            tmp.symlink_to(os.readlink(link))
            os.replace(tmp, out / link.name)
    return library(dest).resolve()


def record(library: Path, head: str) -> dict:
    return {
        "head": head,
        "source": "head mirror",
        "linked_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(library.stat().st_mtime)),
        "library": str(library),
    }


def read(build_dir: Path) -> dict | None:
    """The recorded baseline beside @p build_dir's library, or None if it was built otherwise."""
    path = build_dir / "BASELINE"
    return json.loads(path.read_text()) if path.exists() else None


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    built, mirror, dest = Path(argv[0]), Path(argv[1]), REPO / argv[2]
    head = (mirror / ".HEAD").read_text().strip()
    rec = record(publish(built, dest), head)
    (dest / "BASELINE").write_text(json.dumps(rec, indent=1) + "\n")
    print(f"baseline {rec['head']} (HEAD mirror) linked {rec['linked_utc']} -> {rec['library']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
