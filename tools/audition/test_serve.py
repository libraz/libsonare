#!/usr/bin/env python3
"""What counts as a set, and what does not.

Everything here is about `discover`, which decides what the page's set picker
lists. It got that wrong in a way nothing else could catch: the default output
directory is the parent of every named one, so the glob that finds the sets
finds their take directories too, and a set of six phrases came out as six
play-only "sets" of two versions each -- while the real one vanished, dropped
for being their parent. Every listed name resolved to audio and played, so
there was nothing to notice beyond a picker that had grown.
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serve  # noqa: E402


def _write_set(root: Path, takes: dict[str, list[str]], title: str = "") -> Path:
    """A set directory: a manifest plus the WAVs it names."""
    root.mkdir(parents=True, exist_ok=True)
    items = []
    for take, versions in takes.items():
        (root / take).mkdir(parents=True, exist_ok=True)
        tracks = {}
        for v in versions:
            (root / take / f"{v}.wav").write_bytes(b"RIFF")
            tracks[v] = f"{take}/{v}.wav"
        items.append({"id": take, "tracks": tracks})
    manifest = {"items": items}
    if title:
        manifest["title"] = title
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


def _discover(scratch: Path) -> list[str]:
    """`discover` against a scratch root, as ids, in the order it returns them."""
    original = serve.SCRATCH_ROOT
    serve.SCRATCH_ROOT = scratch
    try:
        return [serve.set_id(p) for p in serve.discover([])]
    finally:
        serve.SCRATCH_ROOT = original


def test_take_dirs_of_a_set_are_not_sets() -> None:
    """The default output directory is the parent of every named one."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp).resolve()
        # The kit set, written to the default `--out`.
        _write_set(scratch / "audition",
                   {"groove": ["model", "kit-a"], "tom-fill": ["model", "kit-a"]})
        # A named set, written under it.
        _write_set(scratch / "audition" / "piano-body",
                   {"single-c4": ["model", "grand-227"]})

        ids = _discover(scratch)
        assert "groove" not in ids, ids
        assert "tom-fill" not in ids, ids
        assert "piano-body" in ids, ids
        # The set whose takes those are has to survive as well: it is the one
        # that used to be dropped, and losing it is the half of this that is
        # invisible -- the six spurious names at least showed up.
        assert "audition" in ids, ids
        assert len(ids) == 2, ids


def test_a_manifestless_parent_is_not_a_set() -> None:
    """A directory that only holds sets is a container, not a set."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp).resolve()
        (scratch / "audition").mkdir()
        _write_set(scratch / "audition" / "piano-body", {"single-c4": ["model"]})
        _write_set(scratch / "audition" / "piano-poly", {"chord": ["model"]})

        ids = _discover(scratch)
        assert sorted(ids) == ["piano-body", "piano-poly"], ids


def test_a_directory_of_loose_renders_is_a_set() -> None:
    """No manifest, no subdirectories: the layout somebody assembled by hand."""
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp).resolve()
        loose = scratch / "audition"
        loose.mkdir()
        (loose / "a.wav").write_bytes(b"RIFF")
        (loose / "b.wav").write_bytes(b"RIFF")

        ids = _discover(scratch)
        assert ids == ["audition"], ids


def test_set_id_keeps_a_generic_leaf_under_the_scratch_root() -> None:
    """`.../pianolab/audition` is `pianolab`; `<scratch>/audition` stays itself.

    The parent stands in for a leaf that says nothing, which is what tells two
    archived captures apart. Directly under the scratch root the parent names
    the harness, and every set there would come out with the same id.
    """
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp).resolve()
        original = serve.SCRATCH_ROOT
        serve.SCRATCH_ROOT = scratch
        try:
            assert serve.set_id(scratch / "audition") == "audition"
            assert serve.set_id(scratch / "pianolab" / "audition") == "pianolab"
            assert serve.set_id(scratch / "piano-body") == "piano-body"
        finally:
            serve.SCRATCH_ROOT = original


def test_a_named_directory_of_sets_is_expanded() -> None:
    """A run that renders several voices writes one set per voice under a root.

    Naming that root is the obvious way to ask for all of them, and it is what
    the renderer prints. Reported as empty it is true of the root and false of
    everything in it, which reads as a run that produced nothing.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / "audition"
        root.mkdir()
        _write_set(root / "p040-violin", {"single-long": ["model"]})
        _write_set(root / "p006-harpsichord", {"single-c4": ["model", "baroque"]})

        ids = sorted(serve.set_id(p) for p in serve.discover([str(root)]))
        assert ids == ["p006-harpsichord", "p040-violin"], ids


def test_a_named_set_is_served_as_itself() -> None:
    """Expansion must not reach into a set and serve its takes as sets."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / "pipe-organ"
        _write_set(root, {"single-long": ["model", "plenum-a"]})

        found = serve.discover([str(root)])
        assert found == [root], found


def test_a_named_directory_with_nothing_in_it_stays_itself() -> None:
    """So the "no renders found in: <path>" message names what was asked for."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / "empty"
        root.mkdir()
        assert serve.discover([str(root)]) == [root]


def test_take_dirs_survives_an_unreadable_manifest() -> None:
    """A half-written manifest must not take the whole picker down with it."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / "broken"
        root.mkdir()
        (root / "manifest.json").write_text("{not json")
        assert serve.take_dirs(root) == set()


def test_a_probe_is_not_a_set() -> None:
    """A component-isolation run renders a voice with parts of it switched off.

    On the picker beside the real pages of the same voice it reads as another
    candidate version, which is what four piano pages and one probe looked like.
    """
    with tempfile.TemporaryDirectory() as tmp:
        scratch = Path(tmp).resolve()
        _write_set(scratch / "audition" / "p000-acoustic-grand-piano",
                   {"single-c4": ["model", "grand-227"]})
        probe = _write_set(scratch / "audition" / "p000-isolate",
                           {"single-c4": ["model", "D_AIR"]})
        manifest = json.loads((probe / "manifest.json").read_text())
        manifest["probe"] = True
        (probe / "manifest.json").write_text(json.dumps(manifest))

        ids = _discover(scratch)
        assert "p000-acoustic-grand-piano" in ids, ids
        assert "p000-isolate" not in ids, ids


def test_a_probe_named_explicitly_is_still_skipped() -> None:
    """The flag travels with the data, so pointing at one does not serve it."""
    with tempfile.TemporaryDirectory() as tmp:
        probe = _write_set(Path(tmp).resolve() / "p000-isolate",
                           {"single-c4": ["model", "D_AIR"]})
        manifest = json.loads((probe / "manifest.json").read_text())
        manifest["probe"] = True
        (probe / "manifest.json").write_text(json.dumps(manifest))
        assert serve.is_probe(probe)
        assert serve.discover([str(probe)]) == []


def test_an_ordinary_set_is_not_a_probe() -> None:
    """A guard that cannot go either way is worth nothing."""
    with tempfile.TemporaryDirectory() as tmp:
        ordinary = _write_set(Path(tmp).resolve() / "p000-acoustic-grand-piano",
                              {"single-c4": ["model", "grand-227"]})
        assert not serve.is_probe(ordinary)
        assert serve.discover([str(ordinary)]) == [ordinary]


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        except AssertionError as e:  # noqa: PERF203
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
