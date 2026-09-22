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
def test_feedback_undo_drops_only_the_last_entry() -> None:
    """A note is sent while the sound is still going and the wrong version is
    one keystroke away, so undo has to be exact rather than a truncation."""
    with tempfile.TemporaryDirectory() as tmp:
        _feedback_in(tmp)
        path = serve.feedback_path("p040-violin")
        for tag in ("off/unsure", "onset/hard", "tone/bright"):
            serve.append_feedback(path, {"tag": tag})
        serve.drop_last_feedback(path)
        assert [e["tag"] for e in serve.read_feedback(path)] == ["off/unsure", "onset/hard"]
        serve.drop_last_feedback(path)
        serve.drop_last_feedback(path)
        assert serve.read_feedback(path) == []
        # An undo on an empty log is a no-op rather than an error: the button is
        # on screen before anything has been sent.
        serve.drop_last_feedback(path)
        assert serve.read_feedback(path) == []


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
    modules = sorted(p.name for p in serve.APP_DIR.glob("*.js"))
    assert len(modules) >= 4, modules
    for name in modules + ["style.css", "index.html", ""]:
        assert handler._resolve(name) == serve.APP_DIR / (name or "index.html"), name
    for bad in ("../serve.py", "sub/dir.js", "serve.py", "nope.js"):
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
            quiet, {"at": "2026-09-05T00:00:00+00:00", "grade": "ok", "tag": "ok"}
        )

        index = serve.feedback_index()
        assert index["p081-lead"]["worst"] == "broken", index
        assert index["p081-lead"]["n"] == 3, index
        assert index["p081-lead"]["prefer"] == 1, index
        assert index["p081-lead"]["last"] == "2026-09-20T00:00:00+00:00", index
        # A voice nobody has faulted is not the same as one nobody has opened,
        # and the second must not appear in the index at all.
        assert index["p040-violin"]["worst"] == "ok", index
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


def test_the_knob_never_reaches_the_page_and_the_signal_path_does() -> None:
    """Two facts a rendered manifest gets wrong, both fixed where it is served.

    A listener shown `piano.brightness=0.30` reports on brightness, which is the
    one thing that knob cannot be asked about — and every page rendered so far
    carries the override string in the field the banner reads. The signal path
    is the other way round: it is missing, and without it the switch cannot tell
    a second path from a second candidate.
    """
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
    assert src["felt-worn-di"]["path"] == "direct"
    assert "path" not in src["felt-worn"], src["felt-worn"]
    # A note is prose and prose has dashes in it.
    assert src["prose"]["detail"] == "two references — both of them dark"


def test_a_set_outside_the_bank_is_left_as_it_is() -> None:
    """A directory served from anywhere else has no registry behind it, and the
    page falls back to the keys rather than the response failing."""
    manifest = {"sources": {"a": {"role": "model"}}}
    assert not serve.label_sources(manifest, "not-a-voice")
    assert manifest["sources"]["a"] == {"role": "model"}


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
