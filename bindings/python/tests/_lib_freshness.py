"""Detect an in-tree libsonare build that is older than the sources it came from.

``EXPECTED_ABI_VERSION`` only catches a library whose ABI version moved. A
build that predates a same-version source edit loads cleanly and silently
exercises the old behaviour, so the test session compares the library's
modification time with the newest core source instead.
"""

from __future__ import annotations

import os
from pathlib import Path

SOURCE_SUFFIXES = (".cpp", ".cc", ".h", ".hpp", ".inc")
SOURCE_TREES = ("src", "include")
# WASM-only glue is never linked into the shared library.
EXCLUDED_TREES = (Path("src") / "wasm",)
ALLOW_STALE_ENV = "SONARE_ALLOW_STALE_LIB"


def newest_source(repo_root: Path) -> tuple[float, Path] | None:
    """Return the (mtime, path) of the newest core source, or None if there is none."""
    newest: tuple[float, Path] | None = None
    excluded = {(repo_root / tree).resolve() for tree in EXCLUDED_TREES}
    for tree in SOURCE_TREES:
        for directory, subdirs, files in os.walk(repo_root / tree):
            subdirs[:] = [d for d in subdirs if (Path(directory) / d).resolve() not in excluded]
            for name in files:
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = Path(directory) / name
                try:
                    mtime = path.stat().st_mtime
                except OSError:
                    continue
                if newest is None or mtime > newest[0]:
                    newest = (mtime, path)
    return newest


def stale_library_message(lib_path: str, repo_root: Path) -> str | None:
    """Explain why ``lib_path`` is a stale in-tree build, or return None if it is fine.

    A library outside ``repo_root`` (an installed wheel, or a build placed
    elsewhere through ``SONARE_LIB_PATH``) is the caller's explicit choice and is
    not judged. ``SONARE_ALLOW_STALE_LIB=1`` opts out for a run that knowingly
    uses an older build.
    """
    if os.environ.get(ALLOW_STALE_ENV) == "1":
        return None
    lib = Path(lib_path).resolve()
    root = repo_root.resolve()
    if root not in lib.parents:
        return None
    newest = newest_source(root)
    if newest is None:
        return None
    source_mtime, source = newest
    if lib.stat().st_mtime >= source_mtime:
        return None
    return (
        f"stale libsonare build: {lib} is older than {source.relative_to(root)}. "
        "The ABI version check cannot see a same-version rebuild, so these tests would "
        "run against old behaviour. Rebuild it (make build-shared), or build elsewhere and "
        f"point SONARE_LIB_PATH at the result; set {ALLOW_STALE_ENV}=1 to run anyway."
    )
