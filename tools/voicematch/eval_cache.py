"""Raw loss terms kept across runs, keyed by everything that decides them.

A fit spends its wall clock rendering candidates, and the same candidates come
back: a staged fit re-scores its own start point, a restart revisits a basin, and
the human loop — listen, change a constant, fit again — re-renders whatever the
previous run already measured. `Evaluator.cache` catches that inside one process
and forgets it at exit, so every one of those is paid again.

What is stored is the raw per-term mismatch, the same value the in-memory cache
holds, so a hit is indistinguishable from the render it stands for. That is only
true while the key covers everything the value depends on, which is why the
signature is deliberately over-broad: the library's own bytes, the *whole*
harness source, the probe's layout and the oracle it was measured against. A
signature that covers too much costs a cold start nobody notices; one that
covers too little hands back a number from a scorer that no longer exists.

A rebuilding fit (a source knob in the spec) is not cached at all — its library
changes every evaluation, so no key could ever repeat.
"""

from __future__ import annotations

import json
import threading
from hashlib import blake2b
from pathlib import Path

#: What a cache file may grow to before a run starts a fresh one. A line is a
#: few hundred bytes, so this is tens of thousands of evaluations — far past any
#: one voice's history, and small enough that loading it is not felt.
MAX_BYTES = 8 * 1024 * 1024


def digest(*parts: bytes) -> str:
    """Fold several byte strings into one name, length-delimited.

    The lengths are in the hash because the parts are concatenated: without them
    two different splits of the same bytes would collide, which for a signature
    built out of a source tree and an oracle is not a theoretical worry.
    """
    h = blake2b(digest_size=16)
    for part in parts:
        h.update(len(part).to_bytes(8, "little"))
        h.update(part)
    return h.hexdigest()


def file_digest(path: Path) -> str:
    """Content digest of one file, or a marker when it is not there."""
    try:
        return digest(path.read_bytes())
    except OSError:
        return "absent"


def source_digest(directory: Path) -> str:
    """Digest of the harness modules that can change a render or a score.

    Every `.py` in the directory except the tests, rather than the handful that
    obviously matter. Naming them would be a second list to keep in step with
    the imports, and the failure it invites is silent and one-sided: a module
    left off the list keeps serving values computed by code that has since
    changed. Over-invalidating costs a cold run.
    """
    files = sorted(p for p in directory.glob("*.py") if not p.name.startswith("test_"))
    return digest(*(p.name.encode() + b"\0" + p.read_bytes() for p in files))


class EvalCache:
    """Append-only store of `key -> terms`, one file per signature.

    Loaded whole at construction and appended to as a run renders, so a run that
    dies mid-fit still leaves everything it measured for the next one. A later
    line for a key wins, which is what makes the file safe to append to without
    ever rewriting it.
    """

    def __init__(self, path: Path | None):
        self.path = path
        self.entries: dict[tuple[str, ...], dict | None] = {}
        self._lock = threading.Lock()
        self.loaded = 0
        # Whether opening it threw the previous generation away, so the caller
        # can say so: measurements disappearing is worth a line even when it is
        # the intended behaviour.
        self.dropped = False
        if path is None:
            return
        if path.exists() and path.stat().st_size > MAX_BYTES:
            path.unlink()
            self.dropped = True
        for key, terms in _read(path):
            self.entries[key] = terms
        self.loaded = len(self.entries)

    def put(self, key: tuple[str, ...], terms: dict | None) -> None:
        if self.path is None:
            return
        with self._lock:
            self.entries[key] = terms
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with self.path.open("a") as fh:
                fh.write(json.dumps({"k": list(key), "t": terms}) + "\n")


def _read(path: Path) -> list[tuple[tuple[str, ...], dict | None]]:
    """Every entry the file holds, in order, skipping anything unreadable.

    A truncated last line is the expected damage — a run killed mid-append — and
    it is not a reason to lose the rest of the file or to fail the fit that is
    about to start.
    """
    out: list[tuple[tuple[str, ...], dict | None]] = []
    try:
        text = path.read_text()
    except OSError:
        return out
    for line in text.splitlines():
        try:
            row = json.loads(line)
            out.append((tuple(row["k"]), row["t"]))
        except (ValueError, KeyError, TypeError):
            continue
    return out


def open_cache(root: Path, signature: str, *, enabled: bool = True) -> EvalCache:
    """The store for one signature, or an inert one when caching is off."""
    if not enabled:
        return EvalCache(None)
    return EvalCache(root / "eval-cache" / f"{signature}.jsonl")
