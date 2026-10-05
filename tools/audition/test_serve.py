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

import http.client
import json
import re
import socketserver
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import serve


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
        _write_set(
            scratch / "audition", {"groove": ["model", "kit-a"], "tom-fill": ["model", "kit-a"]}
        )
        # A named set, written under it.
        _write_set(scratch / "audition" / "piano-body", {"single-c4": ["model", "grand-227"]})

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
        _write_set(
            scratch / "audition" / "p000-acoustic-grand-piano",
            {"single-c4": ["model", "grand-227"]},
        )
        probe = _write_set(scratch / "audition" / "p000-isolate", {"single-c4": ["model", "D_AIR"]})
        manifest = json.loads((probe / "manifest.json").read_text())
        manifest["probe"] = True
        (probe / "manifest.json").write_text(json.dumps(manifest))

        ids = _discover(scratch)
        assert "p000-acoustic-grand-piano" in ids, ids
        assert "p000-isolate" not in ids, ids


def test_a_probe_named_explicitly_is_still_skipped() -> None:
    """The flag travels with the data, so pointing at one does not serve it."""
    with tempfile.TemporaryDirectory() as tmp:
        probe = _write_set(Path(tmp).resolve() / "p000-isolate", {"single-c4": ["model", "D_AIR"]})
        manifest = json.loads((probe / "manifest.json").read_text())
        manifest["probe"] = True
        (probe / "manifest.json").write_text(json.dumps(manifest))
        assert serve.is_probe(probe)
        assert serve.discover([str(probe)]) == []


def test_an_ordinary_set_is_not_a_probe() -> None:
    """A guard that cannot go either way is worth nothing."""
    with tempfile.TemporaryDirectory() as tmp:
        ordinary = _write_set(
            Path(tmp).resolve() / "p000-acoustic-grand-piano", {"single-c4": ["model", "grand-227"]}
        )
        assert not serve.is_probe(ordinary)
        assert serve.discover([str(ordinary)]) == [ordinary]


def _feedback_in(tmp: str) -> Path:
    """Point the feedback log at a scratch directory for one test."""
    root = Path(tmp).resolve() / "feedback"
    serve.FEEDBACK_ROOT = root
    return root


def _with_feedback_root(fn):
    def run() -> None:
        original = serve.FEEDBACK_ROOT
        try:
            fn()
        finally:
            serve.FEEDBACK_ROOT = original

    run.__name__ = fn.__name__
    run.__doc__ = fn.__doc__
    return run


@_with_feedback_root
def test_feedback_is_one_append_only_file_per_set() -> None:
    """Two tabs on two voices must not be able to lose each other's lines."""
    with tempfile.TemporaryDirectory() as tmp:
        _feedback_in(tmp)
        violin = serve.feedback_path("p040-violin")
        flute = serve.feedback_path("p073-flute")
        serve.append_feedback(violin, {"tag": "onset/hard", "text": "きつい"})
        serve.append_feedback(flute, {"tag": "tone/dark"})
        serve.append_feedback(violin, {"tag": "tail/short"})

        got = serve.read_feedback(violin)
        assert [e["tag"] for e in got] == ["onset/hard", "tail/short"], got
        # Non-ASCII survives the round trip: the page is bilingual and a note
        # written in Japanese is the common case rather than the exotic one.
        assert got[0]["text"] == "きつい", got
        assert [e["tag"] for e in serve.read_feedback(flute)] == ["tone/dark"]


@_with_feedback_root
def test_feedback_undo_removes_exactly_the_entry_with_that_id() -> None:
    """A note is sent while the sound is still going and the wrong version is
    one keystroke away, so undo has to be exact -- and byte-preserving."""
    with tempfile.TemporaryDirectory() as tmp:
        _feedback_in(tmp)
        path = serve.feedback_path("p040-violin")
        ids = []
        for tag in ("off/unsure", "onset/hard", "tone/bright"):
            entry = serve.feedback_entry({"tag": tag}, "now")
            ids.append(entry["id"])
            serve.append_feedback(path, entry)
        assert len(set(ids)) == 3 and all(re.fullmatch(r"[0-9a-f]{32}", i) for i in ids), ids
        # A malformed line, a non-object line and a legacy entry with no id.
        with path.open("ab") as fh:
            fh.write(b'{"tag": "legacy"}\n{broken\n[1, 2]\n')
        before = path.read_bytes()
        assert serve.drop_feedback(path, ids[1]) is True
        lines = before.splitlines(keepends=True)
        assert path.read_bytes() == b"".join(lines[:1] + lines[2:])
        assert [e["tag"] for e in serve.read_feedback(path)] == [
            "off/unsure",
            "tone/bright",
            "legacy",
        ]
        # Unknown, repeated, missing and non-string ids write nothing.
        after = path.read_bytes()
        for bad in (ids[1], "nope", None, "", 7):
            assert serve.drop_feedback(path, bad) is False, bad
        assert path.read_bytes() == after
        assert [p.name for p in path.parent.iterdir()] == [path.name]
        assert serve.drop_feedback(path.with_name("none.jsonl"), ids[0]) is False


@_with_feedback_root
def test_a_set_name_cannot_write_outside_the_feedback_directory() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = _feedback_in(tmp)
        # The invariant is containment, not refusal: a name that sanitises to
        # something is written under the directory, and one that sanitises to
        # nothing is refused. Either way nothing lands elsewhere.
        for name in (
            "",
            "..",
            "../..",
            "/etc/passwd",
            "...",
            "./.",
            "../p040-violin",
            "p040-violin",
            "a/b/c",
        ):
            got = serve.feedback_path(name)
            assert got is None or got.parent == root, (name, got)
        for empty in ("", "..", "...", "./.", "///"):
            assert serve.feedback_path(empty) is None, empty
        assert serve.feedback_path("p040-violin") == root / "p040-violin.jsonl"


@_with_feedback_root
def test_a_truncated_line_does_not_take_the_log_down() -> None:
    """What a crash mid-append leaves. The file is a log, not a document."""
    with tempfile.TemporaryDirectory() as tmp:
        _feedback_in(tmp)
        path = serve.feedback_path("p040-violin")
        serve.append_feedback(path, {"tag": "onset/hard"})
        with path.open("a", encoding="utf-8") as fh:
            fh.write('{"tag": "tone/br')
        assert [e["tag"] for e in serve.read_feedback(path)] == ["onset/hard"]


def test_every_module_the_page_loads_is_servable() -> None:
    """The page is ES modules, so a file the resolver does not know about is a
    blank page rather than a missing style: the import throws before anything
    on it runs."""
    handler = serve.Handler.__new__(serve.Handler)
    web = serve.WEB_DIR
    files = sorted(str(p.relative_to(web)) for p in [*web.glob("js/*.js"), *web.glob("css/*.css")])
    assert len(files) >= 4, files
    # Every sheet and the entry module the markup names, so a file added to the
    # page and not to the resolver's pattern fails here rather than in a browser.
    linked = re.findall(r'(?:href|src)="((?:js|css)/[^"]+)"', (web / "index.html").read_text())
    assert linked and set(linked) <= set(files), linked
    for name in files + ["index.html", ""]:
        assert handler._resolve(name) == web / (name or "index.html"), name
    for bad in (
        "../serve.py",
        "js/../../serve.py",
        "sub/dir.js",
        "css/a/b.css",
        "serve.py",
        "app.js",
        "js/nope.js",
    ):
        assert handler._resolve(bad) is None, bad


@_with_feedback_root
def test_the_index_marks_each_voice_with_the_worst_thing_said_about_it() -> None:
    """Not the last thing, and not a count on its own.

    The list this fills is 180 rows long and its marker is read at a glance, so
    a voice that barely sounds has to outrank anything about its colour however
    many cheerful notes were taken after it. A preference is counted apart
    because it carries no verdict at all: a voice with three of them and no
    grade has been listened to hard and judged not once.
    """
    with tempfile.TemporaryDirectory() as tmp:
        _feedback_in(tmp)
        path = serve.feedback_path("p081-lead")
        serve.append_feedback(
            path, {"at": "2026-09-01T00:00:00+00:00", "grade": "broken", "tag": "broken"}
        )
        serve.append_feedback(
            path, {"at": "2026-09-19T00:00:00+00:00", "grade": "acceptable", "tag": "tone/dark"}
        )
        serve.append_feedback(
            path, {"at": "2026-09-20T00:00:00+00:00", "grade": "", "tag": "prefer"}
        )
        quiet = serve.feedback_path("p040-violin")
        serve.append_feedback(
            quiet,
            {
                "at": "2026-09-05T00:00:00+00:00",
                "grade": "ok",
                "tag": "ok",
                "conditions": {"compared_against": {"role": "reference", "version": "gm041"}},
            },
        )

        index = serve.feedback_index()
        assert index["p081-lead"]["worst"] == "broken", index
        assert index["p081-lead"]["n"] == 3, index
        assert index["p081-lead"]["prefer"] == 1, index
        assert index["p081-lead"]["last"] == "2026-09-20T00:00:00+00:00", index
        # None of the three records `compared_against`, so all count as off-reference.
        assert index["p081-lead"]["off_reference"] == 3, index
        # A voice nobody has faulted is not the same as one nobody has opened,
        # and the second must not appear in the index at all.
        assert index["p040-violin"]["worst"] == "ok", index
        assert index["p040-violin"]["off_reference"] == 0, index
        assert "p019-organ" not in index, index


def _with_bank_files(fn):
    """Point the policy and the capture definitions at a scratch tree."""

    def wrapped() -> None:
        policy, captures = serve.POLICY_PATH, serve.CAPTURE_DIR
        try:
            fn()
        finally:
            serve.POLICY_PATH, serve.CAPTURE_DIR = policy, captures

    wrapped.__name__ = fn.__name__
    wrapped.__doc__ = fn.__doc__
    return wrapped


#: A policy with one machine-defined program, one kit branch and one declined
#: address — the three branches that are not the default.
_POLICY = {
    "reference_layer": {
        "_": "prose the reader skips",
        "default": {
            "timbre": "instrument",
            "behaviour": "instrument",
            "reason": "names a real instrument",
        },
        "machine_defined": {
            "timbre": "machine",
            "behaviour": "machine",
            "programs": [81],
            "reason": "the machine invented it",
        },
        "kits": {
            "timbre": "instrument",
            "behaviour": "machine",
            "reason": "real drums, the machine's relations",
        },
    },
    "no_reference": {
        "p000b016-acoustic-grand-piano": {
            "names": "Piano 1d",
            "carries": "European Pf",
            "reason": "the recordings hold a different instrument at this address",
        },
    },
}


def _bank_files(tmp: Path, captures: dict[str, dict]) -> None:
    """Write a policy and a capture directory, and point the server at them."""
    serve.POLICY_PATH = tmp / "policy.json"
    serve.POLICY_PATH.write_text(json.dumps(_POLICY), encoding="utf-8")
    serve.CAPTURE_DIR = tmp / "capture"
    serve.CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    for name, body in captures.items():
        (serve.CAPTURE_DIR / f"{name}.json").write_text(json.dumps(body), encoding="utf-8")


@_with_bank_files
def test_a_machine_defined_slot_wants_the_module_and_says_when_it_did_not_get_it() -> None:
    """The gap this line exists for.

    A slot naming a sound the machine invented has nothing outside the machine
    to be recorded, so a sample library's idea of it is a substitute rather than
    a target -- and it sounds exactly like a finished voice, which is why the
    page has to say so rather than leave it to be remembered.
    """
    with tempfile.TemporaryDirectory() as tmp:
        _bank_files(
            Path(tmp),
            {
                "lead_saw": {"label": "Lead 2", "source_class": "library"},
                "lead_saw_hw": {"label": "Lead 2", "source_class": "module"},
                "violin": {"label": "Violin", "source_class": "library"},
            },
        )
        off = serve.provenance({"program": 81, "capture": "lead_saw"}, "p081-lead")
        assert off["state"] == "off-target", off
        assert off["want"]["timbre"] == "machine", off

        module = serve.provenance({"program": 81, "capture": "lead_saw_hw"}, "p081-lead")
        assert module["state"] == "aimed", module

        # The same source class on a slot that names a real instrument is
        # exactly what the policy asks for. Without this the test would pass on
        # a resolver that called every library capture off-target.
        instrument = serve.provenance({"program": 40, "capture": "violin"}, "p040-violin")
        assert instrument["state"] == "aimed", instrument
        assert instrument["want"]["timbre"] == "instrument", instrument


@_with_bank_files
def test_a_kit_takes_the_kit_branch_whatever_number_selects_it() -> None:
    """A kit and a melodic voice share the program space.

    Nothing in the number tells them apart, so a kit selected by a program some
    other branch names would be answered as that melodic voice -- and a kit is
    the one entry whose two axes differ, colour from a recording and ring from
    the machine.
    """
    with tempfile.TemporaryDirectory() as tmp:
        _bank_files(Path(tmp), {"drums": {"label": "kit", "source_class": "library"}})
        kit = serve.provenance({"program": 81, "kit": True, "capture": "drums"}, "kit081-whatever")
        assert kit["want"]["branch"] == "kits", kit
        assert kit["state"] == "aimed", kit


@_with_bank_files
def test_an_address_held_to_have_no_reference_is_not_one_nobody_captured() -> None:
    """Two silences that mean opposite things.

    One is work nobody has done and the other is a decision: the recordings
    hold a different instrument at that address, so there is nothing to capture
    and the page should not read as though a capture is owed.
    """
    with tempfile.TemporaryDirectory() as tmp:
        _bank_files(Path(tmp), {})
        declined = serve.provenance({"program": 0, "bank": 16}, "p000b016-acoustic-grand-piano")
        assert declined["state"] == "declined", declined
        assert declined["declined"]["names"] == "Piano 1d", declined

        missing = serve.provenance({"program": 7}, "p007-clavi")
        assert missing["state"] == "uncaptured", missing
        assert "declined" not in missing, missing


@_with_bank_files
def test_the_product_is_read_from_the_overlay_and_the_class_from_the_definition() -> None:
    """The split the capture files are in two halves for.

    A clone without the untracked overlay has to be told what KIND of source it
    is hearing -- that is the fact a listener needs -- while the product's name
    is the one that may not be committed.
    """
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        _bank_files(
            tmp_path,
            {"violin": {"label": "Violin, sampled", "source_class": "library", "room": "none"}},
        )
        bare = serve.capture_facts("violin")
        assert bare["source_class"] == "library", bare
        assert bare["product"] == "", bare
        assert bare["room"] == "none", bare

        (serve.CAPTURE_DIR / "violin.local.json").write_text(
            json.dumps({"label": "Some Sampler 9"}), encoding="utf-8"
        )
        assert serve.capture_facts("violin")["product"] == "Some Sampler 9"

        # An unclassified capture is its own state rather than a quiet pass:
        # a definition that says nothing cannot be compared with the policy.
        _bank_files(tmp_path, {"mystery": {"label": "?"}})
        assert (
            serve.provenance({"program": 81, "capture": "mystery"}, "p081-lead")["state"]
            == "unclassified"
        )


def test_every_capture_this_tree_holds_resolves_to_a_state() -> None:
    """Against the real policy and the real definitions, not a fixture.

    The resolver reads two tracked files it does not own, and a key renamed in
    either of them fails by returning nothing rather than by raising -- which
    on the page is a line that quietly stops appearing.
    """
    states = {"aimed", "off-target", "unclassified", "declined", "uncaptured"}
    seen = set()
    definitions = sorted(
        p for p in serve.CAPTURE_DIR.glob("*.json") if not p.name.endswith(".local.json")
    )
    assert len(definitions) >= 100, len(definitions)
    for path in definitions:
        cfg = json.loads(path.read_text(encoding="utf-8"))
        program = cfg.get("program")
        if not isinstance(program, int):
            continue
        got = serve.provenance({"program": program, "capture": path.stem}, path.stem)
        assert got.get("state") in states, (path.stem, got)
        assert got.get("want", {}).get("timbre"), (path.stem, got)
        seen.add(got["state"])
    # Every capture landing in one bucket would satisfy the loop above while
    # saying nothing, and the two that matter are the two that differ.
    assert {"aimed", "off-target"} <= seen, seen


def test_a_page_rendered_before_the_words_existed_still_gets_them() -> None:
    """A candidate's title and line are resolved per request, not per render.

    The registry moves on its own and the pages do not: re-rendering one voice
    to read its own buttons is hours of audio for a sentence. So a manifest that
    carries no words gets today's, and one that carries its own keeps them —
    a render says what the setting was when it was made.
    """
    slug = "p019-church-organ"
    variants = serve.recorded_variants(slug)
    assert variants, "the shipped registry has no settings for this voice"
    name = next(iter(variants))

    manifest = {
        "sources": {
            "model": {"role": "model"},
            name: {"role": "model"},
            f"{name}-di": {"role": "model"},
            "mine": {"role": "model", "title": {"en": "kept", "ja": "kept"}},
        }
    }
    assert serve.label_sources(manifest, slug)
    src = manifest["sources"]
    assert set(src[name]["title"]) == {"en", "ja"}, src[name]
    assert src[name]["desc"]["ja"], src[name]
    # The rig-cleared render of a candidate is a different version of it, and
    # the two sit side by side, so its button may not read the same.
    assert src[f"{name}-di"]["title"]["ja"] != src[name]["title"]["ja"]
    # Not everything on a page is a recorded setting.
    assert "title" not in src["model"], src["model"]
    assert src["mine"]["title"] == {"en": "kept", "ja": "kept"}


def test_the_knob_never_reaches_the_page() -> None:
    """A listener shown `piano.brightness=0.30` reports on brightness, which is
    the one thing that knob cannot be asked about — and every page rendered so
    far carries the override string in the field the banner reads."""
    manifest = {
        "sources": {
            "felt-worn": {
                "role": "model",
                "detail": "the felt is flat — fam0.piano.brightness=0.30",
            },
            "felt-worn-di": {"role": "model", "detail": "the same, rig cleared — a.b=1,c.d=2"},
            "prose": {"role": "model", "detail": "two references — both of them dark"},
        }
    }
    assert serve.label_sources(manifest, "p000-acoustic-grand-piano")
    src = manifest["sources"]
    assert src["felt-worn"]["detail"] == "the felt is flat"
    assert src["felt-worn-di"]["detail"] == "the same, rig cleared"
    assert "scope" not in src["felt-worn-di"], "the boundary is scope_sources' job"
    # A note is prose and prose has dashes in it.
    assert src["prose"]["detail"] == "two references — both of them dark"


def test_a_first_generation_manifest_is_read_with_scopes() -> None:
    """`path: "direct"` and a bare `-di` key are the instrument scope, every
    other model render the product scope; references take none, and a manifest
    that already carries scopes keeps them."""
    manifest = {
        "sources": {
            "model": {"role": "model"},
            "model-di": {"role": "model", "path": "direct"},
            "felt-di": {"role": "model"},
            "felt": {"role": "model"},
            "odd": {"role": "model", "path": "direct"},
            "ref": {"role": "reference"},
        }
    }
    assert serve.scope_sources(manifest)
    src = manifest["sources"]
    assert {k: v.get("scope") for k, v in src.items()} == {
        "model": "product",
        "model-di": "instrument",
        "felt-di": "instrument",
        "felt": "product",
        "odd": "instrument",
        "ref": None,
    }
    assert not any("path" in v for v in src.values())

    current = {"sources": {"x-di": {"role": "model", "scope": "product"}}}
    assert not serve.scope_sources(current)
    assert current["sources"]["x-di"]["scope"] == "product"


def test_a_set_outside_the_bank_is_left_as_it_is() -> None:
    """A directory served from anywhere else has no registry behind it, and the
    page falls back to the keys rather than the response failing."""
    manifest = {"sources": {"a": {"role": "model"}}}
    assert not serve.label_sources(manifest, "not-a-voice")
    assert manifest["sources"]["a"] == {"role": "model"}


#: One recorded path: a rig ahead of a GS unit whose second stage is bypassed.
_PATH = {
    "schema": 1,
    "bank_registry_digest": "ab" * 32,
    "library_version": "0.0.0",
    "complete": True,
    "reason": None,
    "events": [
        {
            "frame": 0,
            "kind": "topology",
            "parts": [
                {
                    "part": 0,
                    "program": 27,
                    "bank": 0,
                    "backend": "model",
                    "rig_source": "bank",
                    "stages": ["amp", "cab"],
                    "unit": 0,
                    "mono_prefix": 0,
                    "send_tap": "pre_rig",
                }
            ],
            "units": [
                {
                    "unit": 0,
                    "type": "0x0110",
                    "realization": "modern",
                    "stages": ["od", "amp"],
                    "enabled": [True, False],
                }
            ],
        },
        {"frame": 24000, "kind": "param", "unit": 0, "slot": 3, "value": 1},
    ],
}


def _v2_set(root: Path, generation: str = "g" * 64) -> Path:
    """A schema-2 set: one take, a model render with a path record and a reference."""
    _write_set(root, {"riff": ["model", "ref"]})
    manifest = json.loads((root / "manifest.json").read_text())
    manifest.update(
        schema_version=2,
        set_generation=generation,
        voice={"program": 27, "bank": 0},
        sources={"model": {"role": "model", "scope": "product"}, "ref": {"role": "reference"}},
        comparisons=[
            {
                "id": "gm_gs_product",
                "scope": "product",
                "model_sources": ["model"],
                "oracle_sources": ["ref"],
                "status": "matched",
                "rig_evidence": "verified",
            }
        ],
    )
    manifest["items"][0]["evidence"] = {
        "model": {
            "request_id": "req-m",
            "source_id": None,
            "build_id": "build-m",
            "asset_id": "asset-m",
            "path": _PATH,
            "complete": True,
            "status": "recorded",
            "reason": None,
        },
        "ref": {
            "request_id": "req-r",
            "source_id": "src-r",
            "build_id": None,
            "asset_id": "asset-r",
            "path": None,
            "complete": None,
            "status": "verified",
            "reason": None,
            "origin": "rendered",
        },
    }
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


def _note(generation: str | None, **over) -> dict:
    """A schema-2 note about the model render, as the page posts it."""
    note = {
        "schema_version": 2,
        "grade": "acceptable",
        "tag": "tone/dark",
        "answers": [],
        "text": "dull",
        "lang": "en",
        "conditions": {"set": "riffset", "take": "riff", "version": "model"},
        "evaluation": {
            "comparison_id": "gm_gs_product",
            # Wrong on purpose: scope and status are the manifest's to say.
            "scope": "instrument",
            "judged_source": "model",
            "oracle_source": "ref",
            "comparison_status": "context_only",
            "blind": False,
        },
        "evidence": {"set_generation": generation, "take": "riff", "source": "model"},
    }
    note.update(over)
    return note


def _served(fn):
    """Run with one v2 set served as `riffset` and the feedback log in scratch."""

    def run() -> None:
        original = (serve.Sets.by_id, serve.FEEDBACK_ROOT)
        try:
            with tempfile.TemporaryDirectory() as tmp:
                _feedback_in(tmp)
                serve.Sets.by_id = {"riffset": _v2_set(Path(tmp).resolve() / "riffset")}
                fn(Path(tmp).resolve())
        finally:
            serve.Sets.by_id, serve.FEEDBACK_ROOT = original

    run.__name__ = fn.__name__
    run.__doc__ = fn.__doc__
    return run


@_served
def test_evidence_is_filled_from_the_manifest_not_the_page(tmp: Path) -> None:
    payload = _note("g" * 64)
    payload["evidence"]["asset_id"] = "forged"
    entry = serve.feedback_entry(payload, "2026-10-05T00:00:00+00:00")
    ev = entry["evidence"]
    assert entry["schema_version"] == 2
    assert ev["set_generation"] == "g" * 64 and ev["take"] == "riff"
    assert (ev["request_id"], ev["asset_id"], ev["build_id"]) == ("req-m", "asset-m", "build-m")
    assert ev["completeness"] == "complete"
    assert ev["path_digest"] == serve.json_digest(_PATH)
    assert ev["rig_evidence"] == "verified"
    assert "source" not in ev and "forged" not in json.dumps(ev)
    assert entry["evaluation"]["scope"] == "product"
    assert entry["evaluation"]["comparison_status"] == "matched"
    assert entry["evaluation"]["judged_source"] == "model"


def test_the_path_digest_matches_the_canonical_one_where_it_can_be_imported() -> None:
    """Skipped where the voicematch environment is absent: system python has no numpy."""
    sys.path.insert(0, str(serve.REPO_ROOT / "tools" / "voicematch"))
    try:
        from boundary import canonical_digest
    except ImportError:
        return
    assert serve.json_digest(_PATH) == canonical_digest(_PATH)


@_served
def test_a_claim_on_another_generation_or_take_is_refused(tmp: Path) -> None:
    for payload, reason in (
        (_note("h" * 64), "set_generation"),
        (_note(None), "set_generation"),
        (_note("g" * 64, evidence={"set_generation": "g" * 64, "take": "gone"}), "take"),
        (
            _note("g" * 64, evidence={"set_generation": "g" * 64, "take": "riff", "source": "x"}),
            "source",
        ),
    ):
        try:
            serve.feedback_entry(payload, "now")
        except serve.StaleClaim as stale:
            assert stale.reason == reason, (stale.reason, reason)
        else:
            raise AssertionError(f"accepted a stale claim: {payload['evidence']}")
    bad = _note("g" * 64)
    bad["evaluation"]["comparison_id"] = "instrument_di"
    try:
        serve.feedback_entry(bad, "now")
    except serve.StaleClaim as stale:
        assert stale.reason == "comparison"
    else:
        raise AssertionError("accepted a comparison the manifest does not define")


@_served
def test_a_memo_is_accepted_with_null_evidence(tmp: Path) -> None:
    memo = _note("h" * 64, evidence=None, conditions={"set": "riffset"})
    entry = serve.feedback_entry(memo, "now")
    assert entry["evidence"] is None
    assert entry["schema_version"] == 2
    # A set served by nothing still takes a memo: the log is keyed by name.
    stray = _note(None, evidence=None, conditions={"set": "elsewhere"})
    assert serve.feedback_entry(stray, "now")["evidence"] is None


@_served
def test_blind_answers_are_stored_as_sent(tmp: Path) -> None:
    answers = [
        {
            "take": "riff",
            "candidates": ["ref", "model"],
            "picked": "model",
            "abstained": False,
            "comparison_id": "gm_gs_product",
            "set_generation": "g" * 64,
            "answered_at": "2026-10-01T09:00:00.000Z",
        },
        {
            "take": "older",
            "candidates": ["model", "ref"],
            "picked": None,
            "abstained": True,
            "comparison_id": "gm_gs_product",
            "set_generation": "g" * 64,
            "answered_at": "2026-10-01T09:01:00.000Z",
        },
    ]
    payload = _note(
        "g" * 64,
        tag="blind",
        evidence={"set_generation": "g" * 64, "take": None, "source": None},
        blind_answers=answers,
    )
    payload["evaluation"]["judged_source"] = None
    entry = serve.feedback_entry(payload, "2026-10-05T00:00:00+00:00")
    assert entry["blind_answers"] == answers
    # The run's evidence names the generation and no recording of the moment it was sent.
    assert entry["evidence"]["set_generation"] == "g" * 64
    assert entry["evidence"]["take"] is None and entry["evidence"]["asset_id"] is None


def _post(port: int, body: dict) -> tuple[int, dict]:
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/feedback",
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def _get(port: int, rel: str) -> tuple[int, dict]:
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/{rel}", timeout=5) as r:
        return r.status, json.load(r)


@_served
def test_the_endpoint_answers_409_and_writes_nothing(tmp: Path) -> None:
    """Through the handler, so the status code and the body are the ones the page reads."""
    # A v1 set beside it: written before generations, still served and still noted.
    legacy = _write_set(tmp / "oldset", {"riff": ["model", "ref"]})
    serve.Sets.by_id["oldset"] = legacy
    server = socketserver.TCPServer(("127.0.0.1", 0), serve.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    try:
        status, body = _post(port, _note("h" * 64))
        assert status == 409, status
        assert body["error"] == "stale" and body["reason"] == "set_generation", body
        assert serve.read_feedback(serve.feedback_path("riffset")) == []

        status, body = _post(port, _note("g" * 64))
        assert status == 200 and len(body["entries"]) == 1, (status, body)

        status, manifest = _get(port, "s/oldset/manifest.json")
        assert status == 200 and manifest["items"][0]["id"] == "riff"
        assert "set_generation" not in manifest
        old = _note(None, conditions={"set": "oldset"})
        old["evaluation"]["comparison_id"] = None
        status, body = _post(port, old)
        assert status == 200, (status, body)
        ev = body["entries"][-1]["evidence"]
        assert ev["set_generation"] is None and ev["completeness"] == "unrecorded", ev
        assert ev["asset_id"] is None
    finally:
        server.shutdown()
        server.server_close()


def _with_server(fn):
    """Run `fn(port)` against a live handler, the sets and feedback root as the caller left them."""
    server = socketserver.TCPServer(("127.0.0.1", 0), serve.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        return fn(server.server_address[1])
    finally:
        server.shutdown()
        server.server_close()


def _raw_post(port: int, body: bytes, length: str | None = None) -> int:
    """The status of a POST whose headers are chosen by the caller; 0 where the connection dropped."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.putrequest("POST", "/feedback")
        conn.putheader("Content-Length", str(len(body)) if length is None else length)
        conn.endheaders(body)
        return conn.getresponse().status
    except (http.client.HTTPException, OSError):
        return 0
    finally:
        conn.close()


@_served
def test_an_appended_note_gets_a_server_id_and_undo_removes_it_by_id(tmp: Path) -> None:
    def run(port: int) -> None:
        mine = _post(port, {**_note("g" * 64), "id": "client-chosen"})
        other = _post(port, _note("g" * 64))
        assert mine[0] == 200 and other[0] == 200
        first = mine[1]["entry_id"]
        assert re.fullmatch(r"[0-9a-f]{32}", first), first
        assert mine[1]["entries"][0]["id"] == first and "client-chosen" not in json.dumps(mine[1])
        assert set(mine[1]) == {"entries", "path", "entry_id"}
        status, body = _post(port, {"op": "undo", "set": "riffset", "id": first})
        assert status == 200 and "entry_id" not in body, (status, body)
        assert [e["id"] for e in body["entries"]] == [other[1]["entry_id"]]
        for undo in (
            {"op": "undo", "set": "riffset", "id": first},
            {"op": "undo", "set": "riffset"},
        ):
            status, body = _post(port, undo)
            assert (status, body) == (404, {"error": "not_found"}), (status, body)
        assert len(serve.read_feedback(serve.feedback_path("riffset"))) == 1

    _with_server(run)


@_served
def test_malformed_posts_are_answered_400_not_dropped(tmp: Path) -> None:
    def run(port: int) -> None:
        bad_cid = _note("g" * 64)
        bad_cid["evaluation"]["comparison_id"] = ["gm_gs_product"]
        bad_judged = _note("g" * 64)
        bad_judged["evaluation"]["judged_source"] = {"a": 1}
        bad_take = _note("g" * 64, evidence={"set_generation": "g" * 64, "take": ["riff"]})
        for body in (
            {"set": ["riffset"], "grade": "ok"},
            {"conditions": ["x"], "grade": "ok"},
            bad_cid,
            bad_judged,
            bad_take,
            [1, 2],
        ):
            data = json.dumps(body).encode()
            assert _raw_post(port, data) == 400, body
        assert _raw_post(port, b"{}", length="abc") == 400
        assert serve.read_feedback(serve.feedback_path("riffset")) == []

    _with_server(run)


@_served
def test_log_lines_that_are_json_but_not_objects_are_skipped(tmp: Path) -> None:
    path = serve.feedback_path("riffset")
    serve.append_feedback(path, {"grade": "ok", "at": "2026-10-01"})
    with path.open("a") as fh:
        fh.write('[1]\n"text"\n7\nnull\n')
    assert len(serve.read_feedback(path)) == 1
    assert serve.feedback_index()["riffset"]["n"] == 1


def test_one_unreadable_manifest_skips_its_set_and_leaves_the_rest() -> None:
    original = (serve.Sets.by_id, serve.Sets.index)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            good = _write_set(Path(tmp).resolve() / "good", {"a": ["x", "y"]})
            bad = _write_set(Path(tmp).resolve() / "bad", {"a": ["x", "y"]})
            (bad / "manifest.json").write_text("{not json")
            last = _write_set(Path(tmp).resolve() / "last", {"a": ["x", "y"]})
            serve.Sets.load([good, bad, last])
            assert list(serve.Sets.by_id) == ["good", "last"], serve.Sets.by_id
            assert [e["id"] for e in serve.Sets.index] == ["good", "last"]
    finally:
        serve.Sets.by_id, serve.Sets.index = original


@_served
def test_request_paths_are_url_decoded_before_they_are_resolved(tmp: Path) -> None:
    def run(port: int) -> None:
        def status(rel: str) -> int:
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/{rel}", timeout=5) as r:
                    return r.status
            except urllib.error.HTTPError as e:
                return e.code

        assert status("s/riffset/riff/model.wav") == 200
        assert status("s/riffset/riff%2Fmodel.wav") == 200
        assert status("s/riffset/ri%66f/model.wav") == 200
        (tmp / "secret.txt").write_text("outside the set")
        assert status("s/riffset/%2e%2e/secret.txt") == 404
        assert status("s/riffset/riff/%2e%2e/%2e%2e/%2e%2e/etc/hosts") == 404
        assert status("s/riffset/riff/model.wav%00") == 404

    _with_server(run)


@_served
def test_an_unattached_note_takes_scope_from_the_manifest_not_the_page(tmp: Path) -> None:
    unattached = _note("g" * 64, evidence=None)
    entry = serve.feedback_entry(unattached, "now")
    assert entry["evaluation"]["scope"] == "product", entry["evaluation"]
    assert entry["evaluation"]["comparison_status"] == "matched"
    unknown = _note("g" * 64, evidence=None)
    unknown["evaluation"]["comparison_id"] = "made_up"
    try:
        serve.feedback_entry(unknown, "now")
    except serve.StaleClaim as stale:
        assert stale.reason == "comparison"
    else:
        raise AssertionError("accepted a comparison the manifest does not define")
    none_named = _note("g" * 64, evidence=None)
    none_named["evaluation"]["comparison_id"] = None
    ev = serve.feedback_entry(none_named, "now")["evaluation"]
    assert ev["scope"] is None and ev["comparison_status"] is None, ev
    # A set nothing serves cannot vouch for what the page sent either.
    stray = _note(None, evidence=None, conditions={"set": "elsewhere"})
    ev = serve.feedback_entry(stray, "now")["evaluation"]
    assert ev["scope"] is None and ev["comparison_status"] is None, ev


@_served
def test_a_source_the_take_does_not_hold_is_refused_as_source_not_as_a_rerender(
    tmp: Path,
) -> None:
    payload = _note("g" * 64)
    payload["evaluation"]["oracle_source"] = "elsewhere-ref"
    try:
        serve.feedback_entry(payload, "now")
    except serve.StaleClaim as stale:
        assert stale.reason == "source", stale.reason
    else:
        raise AssertionError("accepted an oracle the take does not hold")


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        # Any exception, not just a failed assertion: a test that reaches for a
        # key a file stopped carrying raises, and catching only AssertionError
        # ends the whole run with no line saying which test it was.
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(
                f"FAIL {t.__name__}: {type(e).__name__}: {e}"
                if not isinstance(e, AssertionError)
                else f"FAIL {t.__name__}: {e}"
            )
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
