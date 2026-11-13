"""Pins a run to the harness code it started with.

A long run that imports a voicematch module late, or re-reads one, after the file
changed on disk spans two harness versions. `lock` imports everything a run can
reach up front and then refuses any local module not yet loaded; `digest` names
the code on disk, so a parent can tell its children are still the same version.
"""

from __future__ import annotations

import ast
import hashlib
import importlib
import sys
from pathlib import Path

from objective import HERE, _imports, code_text

#: Carries the digest a child must have locked to, from a parent that pinned it.
ENV = "SONARE_VOICEMATCH_HARNESS_DIGEST"


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


def _uninstall() -> None:
    global _finder
    if _finder is not None and _finder in sys.meta_path:
        sys.meta_path.remove(_finder)
    _finder = None


def lock(roots: tuple[str, ...], directory: Path = HERE) -> str:
    """Import the closure now, refuse any other local module later; return the digest."""
    global _finder
    if roots in _locked and _finder is not None:
        return _locked[roots]
    _uninstall()
    for name in closure(roots, directory):
        importlib.import_module(name)
    _locked[roots] = digest(roots, directory)
    _finder = _Finder(frozenset(p.stem for p in directory.glob("*.py")))
    sys.meta_path.insert(0, _finder)
    return _locked[roots]


def unlock() -> None:
    """Remove the finder and forget the locked digests."""
    _uninstall()
    _locked.clear()


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
