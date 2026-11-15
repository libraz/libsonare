"""Refuse a sync that would carry capture audio, a spectrum or an unlisted cache field off this Mac.

    python tools/voicematch/leave_check.py --tree DIR [--cache FILE_OR_DIR ...]

Every file under each `--tree` (the directory a sync mirrors, `.git` and `.venv`
aside) is refused when its extension or its first bytes name an audio, sample
or array container. Every `--cache` file must be a gzip oracle cache whose body
passes `oracle_cache.check`, the whitelist that also caps every numeric list.
Prints one verdict line and exits 0 only when nothing is refused.
"""

from __future__ import annotations

import argparse
import gzip
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import oracle_cache

AUDIO_SUFFIXES = frozenset(
    [
        ".wav",
        ".wave",
        ".aif",
        ".aiff",
        ".aifc",
        ".caf",
        ".flac",
        ".mp3",
        ".m4a",
        ".aac",
        ".ogg",
        ".oga",
        ".opus",
        ".wma",
        ".w64",
        ".rf64",
        ".sd2",
        ".snd",
        ".au",
        ".sf2",
        ".sf3",
        ".sfz",
        ".dls",
        ".gig",
        ".exs",
        ".npy",
        ".npz",
        ".mat",
        ".h5",
        ".hdf5",
    ]
)
SKIP_DIRS = frozenset({".git", ".venv"})
#: Files the tree tracks on purpose although their extension is on the list: synthetic fixtures.
TRACKED_FIXTURES = frozenset({"tests/fixtures/sf2/minimal_gs.sf2"})


def _magic(head: bytes) -> str | None:
    if head[:4] in (b"RIFF", b"RF64", b"BW64") and head[8:12] in (b"WAVE", b"sfbk", b"DLS "):
        return head[8:12].decode().strip()
    if head[:4] == b"FORM" and head[8:12] in (b"AIFF", b"AIFC"):
        return head[8:12].decode()
    for sig, name in (
        (b"fLaC", "FLAC"),
        (b"OggS", "Ogg"),
        (b"caff", "CAF"),
        (b"ID3", "MP3"),
        (b"\x93NUMPY", "npy"),
        (b".snd", "AU"),
    ):
        if head.startswith(sig):
            return name
    if head[4:8] == b"ftyp":
        return "MP4"
    if len(head) > 1 and head[0] == 0xFF and head[1] & 0xE0 == 0xE0:
        return "MPEG audio"
    return None


def check_tree(root: Path) -> list[str]:
    refused = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts) or not path.is_file():
            continue
        if rel.as_posix() in TRACKED_FIXTURES:
            continue
        if path.suffix.lower() in AUDIO_SUFFIXES:
            refused.append(f"{rel}: {path.suffix} container")
            continue
        with path.open("rb") as fh:
            kind = _magic(fh.read(16))
        if kind:
            refused.append(f"{rel}: {kind} bytes")
    return refused


def check_cache(path: Path) -> list[str]:
    if not path.name.endswith(".json.gz"):
        return [f"{path.name}: not a .json.gz oracle cache"]
    try:
        with gzip.open(path, "rb") as fh:
            body = json.loads(fh.read())
        oracle_cache.check(body)
    except (OSError, ValueError) as exc:
        return [f"{path.name}: {exc}"]
    return []


def cache_files(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in map(Path, paths):
        out += sorted(p.rglob("*")) if p.is_dir() else [p]
    return [p for p in out if p.is_file()]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tree", action="append", default=[], metavar="DIR")
    ap.add_argument("--cache", action="append", default=[], metavar="FILE_OR_DIR")
    args = ap.parse_args(argv)
    if not args.tree and not args.cache:
        ap.error("name a --tree, a --cache or both")
    refused: list[str] = []
    for tree in args.tree:
        refused += check_tree(Path(tree))
    caches = cache_files(args.cache)
    if args.cache and not caches:
        refused.append(f"{', '.join(args.cache)}: no cache file to check")
    for path in caches:
        refused += check_cache(path)
    if refused:
        print(f"leave-check REFUSED: {len(refused)} item(s)")
        for line in refused:
            print(f"  {line}")
        return 1
    print(
        f"leave-check PASS: {len(args.tree)} tree(s) hold no audio or array container; "
        f"{len(caches)} cache file(s) pass the whitelist"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
