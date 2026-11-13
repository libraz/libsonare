"""Pins a run to the harness code it started with.

A long run that imports a voicematch module late, or re-reads one, after the file
changed on disk spans two harness versions. `lock` imports everything a run can
reach up front and then refuses any local module not yet loaded; `digest` names
the code on disk, so a parent can tell its children are still the same version.
Render workers are fresh processes that import from disk, so `lock` also stamps the
files they reach and `check_workers` refuses a spawn once one has moved.
"""

from __future__ import annotations

import ast
import hashlib
import importlib
import os
import sys
from pathlib import Path

from objective import HERE, _imports, code_text

#: Carries the digest a child must have locked to, from a parent that pinned it.
ENV = "SONARE_VOICEMATCH_HARNESS_DIGEST"

#: The module a render worker subprocess runs; its closure is what a worker imports.
WORKER_ROOTS = ("render_rows",)


class HarnessFileMoved(Exception):
    """A file a render worker imports changed on disk after the lock.

    Deliberately not a RuntimeError: render failures are caught as that and scored as
    an unusable candidate, which would let the run finish on two versions.
    """


def closure(roots: tuple[str, ...], directory: Path = HERE) -> list[str]:
    """Every local module the roots reach, imports inside functions included, sorted."""
    local = {p.stem for p in directory.glob("*.py")}
    seen: set[str] = set()
    pending = [r for r in roots if r in local]
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        tree = ast.parse((directory / f"{name}.py").read_text())
        pending.extend(sorted((_imports(tree) & local) - seen))
    return sorted(seen)


def digest(roots: tuple[str, ...], directory: Path = HERE) -> str:
    """sha256 over the closure's code text, normalised as `objective_digest` is."""
    h = hashlib.sha256()
    for name in closure(roots, directory):
        h.update(name.encode() + b"\0" + code_text((directory / f"{name}.py").read_text()).encode())
        h.update(b"\0")
    return h.hexdigest()[:16]


def snapshot_mtimes(roots: tuple[str, ...], directory: Path = HERE) -> dict[str, int]:
    """Modification time (ns) of every file in the roots' closure, keyed by path."""
    return {
        str(path): path.stat().st_mtime_ns
        for path in (directory / f"{name}.py" for name in closure(roots, directory))
    }


def moved_since(snapshot: dict[str, int]) -> str | None:
    """File name of the first snapshotted file changed or gone since, else None. Stat only."""
    for path, mtime in snapshot.items():
        try:
            if os.stat(path).st_mtime_ns != mtime:
                return Path(path).name
        except OSError:
            return Path(path).name
    return None


class _Finder:
    """Refuses a local module that is not loaded yet."""

    def __init__(self, local: frozenset[str]):
        self.local = local

    def find_spec(self, name, path=None, target=None):
        if name.split(".")[0] in self.local and name not in sys.modules:
            raise ImportError(
                f"harness module {name} first imported after the run started: "
                "the run would span two versions"
            )


_finder: _Finder | None = None
_locked: dict[tuple[str, ...], str] = {}
_workers: dict[str, int] | None = None


def _uninstall() -> None:
    global _finder
    if _finder is not None and _finder in sys.meta_path:
        sys.meta_path.remove(_finder)
    _finder = None


def lock(roots: tuple[str, ...], directory: Path = HERE) -> str:
    """Import the closure now, refuse any other local module later; return the digest."""
    global _finder, _workers
    if roots in _locked and _finder is not None:
        return _locked[roots]
    _uninstall()
    for name in closure(roots, directory):
        importlib.import_module(name)
    _locked[roots] = digest(roots, directory)
    _workers = snapshot_mtimes(WORKER_ROOTS, directory)
    _finder = _Finder(frozenset(p.stem for p in directory.glob("*.py")))
    sys.meta_path.insert(0, _finder)
    return _locked[roots]


def unlock() -> None:
    """Remove the finder and forget the locked digests and worker snapshot."""
    global _workers
    _uninstall()
    _locked.clear()
    _workers = None


def check_workers() -> None:
    """Raise `HarnessFileMoved` if a render worker would import a file changed since `lock`.

    A no-op when no lock was taken.
    """
    if _workers is None:
        return
    name = moved_since(_workers)
    if name is not None:
        raise HarnessFileMoved(
            f"harness file {name} changed on disk since this run started; "
            "the run would score with two versions"
        )


def locked_digest(roots: tuple[str, ...]) -> str | None:
    """The digest `lock` took for these roots, or None when they are not locked."""
    return _locked.get(roots)


def mismatch(locked: str) -> str | None:
    """Why a child locked to `locked` must not start under a pinned parent, else None."""
    import os

    expected = os.environ.get(ENV)
    if expected and expected != locked:
        return (
            f"the harness on disk ({locked}) is not the version the parent run started "
            f"with ({expected}); restart the run"
        )
    return None
