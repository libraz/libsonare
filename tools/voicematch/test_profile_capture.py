"""What a capture definition declares, and what the loader refuses: the
program the model answers with, the rig and room answers, the product
identity neither half may carry, which kind of instrument the slot holds,
and the layout and families inside the grid.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_profile_capture.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

from capture import note_groups, note_map
from loss import _kit_terms, kit_report
from profile_test_fixtures import (
    CAPTURE_DIR,
    REFERENCE_DIR,
    assert_names_no_product,
    shipped_captures,
)

# --------------------------------------------------------------------------
# which instrument the model answers with


def test_the_program_comes_from_the_profile_the_capture_recorded():
    """Not from whichever config the compare was handed.

    The two are only tied together where the capture was measured, and a
    hardcoded program is how a harpsichord reference ends up diffed against a
    grand piano without a word of complaint.
    """
    assert profile_module.profile_program({"capture": {"program": 6}}, {"program": 0}) == 6


def test_a_profile_measured_before_the_field_existed_falls_back_to_the_config():
    assert profile_module.profile_program({"capture": {}}, {"program": 6}) == 6
    assert profile_module.profile_program({}, {}) == 0


def test_the_command_line_overrides_what_the_profile_recorded():
    """One capture judging two programs is a real thing to want."""
    cfg = {"program": 7, "_program_override": True}
    assert profile_module.profile_program({"capture": {"program": 6}}, cfg) == 7


# --------------------------------------------------------------------------
# what a capture definition declares


def test_a_capture_that_names_no_phrase_set_still_resolves_one(tmp_path):
    """`load_config` declares every field it knows about, which turns a missing
    key into an empty one — and a `get(key, default)` downstream then never
    fires. The audition set is the field where that shows."""
    from capture import load_config

    cfg_path = tmp_path / "c.json"
    cfg_path.write_text(
        json.dumps(
            {
                "id": "x",
                "label": "x",
                "plugin": "a:b:c",
                "timbres": [],
                "notes": [60],
                "velocities": [100],
            }
        )
    )
    cfg = load_config(cfg_path)
    assert cfg["takes"] == ""
    assert (cfg.get("takes") or "piano") == "piano"


def test_a_capture_declares_the_program_the_model_answers_with(tmp_path):
    from capture import load_config

    cfg_path = tmp_path / "c.json"
    cfg_path.write_text(
        json.dumps(
            {
                "id": "x",
                "label": "x",
                "plugin": "a:b:c",
                "program": 6,
                "timbres": [],
                "notes": [60],
                "velocities": [100],
            }
        )
    )
    assert load_config(cfg_path)["program"] == 6


def _rig_config(tmp_path: Path, **extra) -> Path:
    cfg_path = tmp_path / "c.json"
    cfg_path.write_text(
        json.dumps(
            {
                "id": "x",
                "label": "x",
                "plugin": "a:b:c",
                "timbres": [],
                "notes": [60],
                "velocities": [100],
                **extra,
            }
        )
    )
    return cfg_path


def test_a_capture_that_says_nothing_about_a_rig_reads_as_unclassified(tmp_path):
    """Not as "no rig". Nothing downstream can tell them apart from the audio —
    a cabinet is a filter and leaves no tail — so the missing record is a
    question nobody has answered rather than an answer of no."""
    from capture import RIG_NONE, RIG_UNCLASSIFIED, load_config

    cfg = load_config(_rig_config(tmp_path))
    assert cfg["rig"] == RIG_UNCLASSIFIED
    assert cfg["rig"] != RIG_NONE


def test_each_rig_answer_survives_the_loader(tmp_path):
    from capture import RIG_VALUES, load_config

    for value in RIG_VALUES:
        assert load_config(_rig_config(tmp_path, rig=value))["rig"] == value


def test_a_rig_answer_the_loader_does_not_know_is_refused(tmp_path):
    """A misspelling would otherwise read as unclassified, which for a family
    that can carry a rig silently turns a declared `none` back into a refusal —
    and for one that cannot, turns a declared `baked` into a fit target."""
    from capture import load_config

    with pytest.raises(ValueError, match="rig is one of"):
        load_config(_rig_config(tmp_path, rig="DI"))


def test_every_shipped_capture_answers_the_rig_question_legibly():
    """Whatever they say, the loader has to understand it."""
    from capture import RIG_VALUES, load_config

    shipped = shipped_captures()
    assert shipped, "no capture definitions found to check"
    for name in shipped:
        assert load_config(CAPTURE_DIR / f"{name}.json")["rig"] in RIG_VALUES


def test_a_capture_that_says_nothing_about_a_room_reads_as_unclassified(tmp_path):
    """Not as "no room". A note-tail estimate cannot tell a room from a long
    release, so a capture that never answered is a question nobody put rather
    than an answer of no — and the estimator still runs, which is what every
    capture written before the field did."""
    from capture import ROOM_NONE, ROOM_UNCLASSIFIED, load_config

    cfg = load_config(_rig_config(tmp_path))
    assert cfg["room"] == ROOM_UNCLASSIFIED
    assert cfg["room"] != ROOM_NONE


def test_each_room_answer_survives_the_loader(tmp_path):
    from capture import ROOM_VALUES, load_config

    for value in ROOM_VALUES:
        assert load_config(_rig_config(tmp_path, room=value))["room"] == value


def test_a_room_answer_the_loader_does_not_know_is_refused(tmp_path):
    """A misspelling would read as unclassified, which turns a declared `none`
    back into a measurement — and that measurement is convolved onto every model
    render before any figure is taken."""
    from capture import load_config

    with pytest.raises(ValueError, match="room is one of"):
        load_config(_rig_config(tmp_path, room="dry"))


def test_every_shipped_capture_answers_the_room_question_legibly():
    """Whatever they say, the loader has to understand it."""
    from capture import ROOM_VALUES, load_config

    shipped = shipped_captures()
    assert shipped, "no capture definitions found to check"
    for name in shipped:
        assert load_config(CAPTURE_DIR / f"{name}.json")["room"] in ROOM_VALUES


def _released_note(
    sr: int, *, preroll: float, gate: float, tail: float, rt60: float, f0: float = 220.0
) -> np.ndarray:
    """A held tone whose release is a plain envelope, damped faster up the stack.

    There is no room in this signal at all: every partial is switched on at the
    onset and falls under its own exponential from note-off. The tilt is the
    point — a release through a lowpass damps its highs faster than its lows,
    which is the one physical signature `estimate_room` has for telling a room
    from an instrument's own ring.
    """
    n = int(sr * (preroll + gate + tail))
    t = np.arange(n) / sr
    a, b = int(sr * preroll), int(sr * preroll) + int(sr * gate)
    since_off = np.arange(n - b) / sr
    out = np.zeros(n)
    for k in range(1, 25):
        hz = f0 * k
        if hz >= sr / 2:
            break
        rt_k = rt60 * (500.0 / max(hz, 500.0)) ** 0.35
        env = np.zeros(n)
        env[a:b] = 1.0
        env[b:] = 10.0 ** (-60.0 * since_off / rt_k / 20.0)
        out += np.sin(2.0 * np.pi * hz * t) * env / k
    return out * 0.25


def _release_corpus(root: Path, *, rt60: float) -> tuple[dict, Path]:
    """A four-note grid of `_released_note`, with the manifest `measure_rooms` reads."""
    from wavio import write_wav

    sr, preroll, gate, tail = 48000, 0.1, 3.0, 2.0
    notes = [48, 60, 72, 84]
    root.mkdir(parents=True, exist_ok=True)
    (root / "t").mkdir(exist_ok=True)
    renders = []
    for note in notes:
        rel = f"t/n{note:03d}_v100.wav"
        write_wav(
            root / rel,
            _released_note(
                sr,
                preroll=preroll,
                gate=gate,
                tail=tail,
                rt60=rt60,
                f0=440.0 * 2 ** ((note - 69) / 12),
            ),
            sr,
        )
        renders.append({"id": rel[:-4], "timbre": "t", "note": note, "velocity": 100, "path": rel})
    return {"renders": renders}, root


def test_a_release_with_no_room_in_it_is_recorded_as_one_when_nobody_says_otherwise(tmp_path):
    """The control half, and the reason the field had to exist.

    Every guard around the estimate separates a room from *one note's* decay, and
    a release is not one note's: it is one envelope generator, so it gives every
    note of the compass the same decay exactly as a room does and the agreement
    test passes on it perfectly. The plausibility gate does not catch it either —
    it refuses a ring that damps its highs no faster than its lows, and a release
    through a lowpass damps them faster, which is what a room looks like.
    """
    manifest, corpus = _release_corpus(tmp_path / "c", rt60=1.6)
    rooms = profile_module.measure_rooms(manifest, corpus, {"t"}, 0.1, 3.0)
    assert "t" in rooms, "the estimator no longer reads a release as a room"
    assert rooms["t"]["rt60_s"] > 0.35
    # The agreement guard is what it got past, so pin that rather than the count:
    # a majority of the compass reported the same space, which is the reading the
    # guard exists to require and the one a release satisfies by construction.
    entry = rooms["t"]
    assert entry["notes_measured"] * 2 > entry["notes_probed"]


def test_a_capture_that_answers_room_none_has_no_space_recorded(tmp_path):
    """The same audio, with the capture answering. Nothing is measured, because
    the answer is the only thing that can be right here."""
    from capture import ROOM_NONE

    manifest, corpus = _release_corpus(tmp_path / "c", rt60=1.6)
    rooms = profile_module.measure_rooms(manifest, corpus, {"t"}, 0.1, 3.0, ROOM_NONE)
    assert rooms == {}


def test_the_identity_overlay_matches_timbres_by_id(tmp_path):
    """The tracked half holds the method, the untracked half holds the product.

    Matching is by timbre id, so a rename on one side and not the other yields a
    config that loads, reports no error, and captures every slot with an empty
    preset — which on a rack is slot 1, four times over.
    """
    from capture import load_config, slot_channel

    (tmp_path / "c.json").write_text(
        json.dumps(
            {
                "id": "c",
                "label": "Concert grands, close",
                "notes": [60],
                "velocities": [100],
                "timbres": [
                    {"id": "grand-227", "label": "227 cm concert grand", "slot_channel": 1},
                    {"id": "grand-274", "label": "274 cm concert grand", "slot_channel": 2},
                ],
            }
        )
    )
    (tmp_path / "c.local.json").write_text(
        json.dumps(
            {
                "plugin": "aumu:xxxx:Vend",
                "timbres": [
                    {"id": "grand-227", "preset": "A/Close"},
                    {"id": "grand-274", "preset": "B/Close"},
                ],
            }
        )
    )
    cfg = load_config(tmp_path / "c.json")
    assert cfg["plugin"] == "aumu:xxxx:Vend"
    assert [t["preset"] for t in cfg["timbres"]] == ["A/Close", "B/Close"]
    # The tracked side still owns everything it declared.
    assert [slot_channel(t) for t in cfg["timbres"]] == [1, 2]
    assert [t["label"] for t in cfg["timbres"]] == ["227 cm concert grand", "274 cm concert grand"]


def test_a_capture_without_its_overlay_loads_and_carries_no_product(tmp_path):
    """A clone has no overlay, and that is the normal case rather than an error:
    everything downstream reads the committed reference profile."""
    from capture import load_config

    (tmp_path / "c.json").write_text(
        json.dumps(
            {
                "id": "c",
                "label": "Concert grands, close",
                "notes": [60],
                "velocities": [100],
                "timbres": [{"id": "grand-227", "label": "227 cm concert grand", "channel": 1}],
            }
        )
    )
    cfg = load_config(tmp_path / "c.json")
    assert "plugin" not in cfg
    assert cfg["timbres"][0]["id"] == "grand-227"


def test_the_shipped_captures_carry_no_plugin_identity():
    """The committed half must not say which commercial product was captured."""
    shipped = shipped_captures()
    assert shipped, "no capture definitions found to check"
    for name in shipped:
        assert_names_no_product(name, capture_dir=CAPTURE_DIR, reference_dir=REFERENCE_DIR)


def test_the_identity_guard_fails_on_a_capture_that_does_name_a_product(tmp_path):
    """Without this the guard above passes whether or not it can see anything.

    Both halves are checked, because the reference is written from the capture
    and a scrub that only cleaned the capture would leave the copy behind.
    """
    capture, reference = tmp_path / "capture", tmp_path / "reference"
    capture.mkdir()
    reference.mkdir()
    (capture / "x.json").write_text(
        json.dumps(
            {
                "id": "x",
                "program": 0,
                "plugin": "aumu:xxxx:Yyyy",
                "timbres": [{"id": "t"}],
            }
        )
    )
    assert shipped_captures(capture) == ["x"]
    with pytest.raises(AssertionError, match="names its plugin"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)

    (capture / "x.json").write_text(
        json.dumps(
            {
                "id": "x",
                "program": 0,
                "timbres": [{"id": "t", "preset": "Grand/Close.fxp"}],
            }
        )
    )
    with pytest.raises(AssertionError, match="names a preset"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)

    (capture / "x.json").write_text(json.dumps({"id": "x", "program": 0, "timbres": [{"id": "t"}]}))
    (reference / "x.json").write_text(json.dumps({"capture": {"plugin": "aumu:xxxx:Yyyy"}}))
    with pytest.raises(AssertionError, match="reference names its plugin"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)


def test_measure_records_the_method_and_not_the_captured_product(tmp_path):
    """The guard above checks the committed files; this checks what writes them.

    A corpus manifest is written from the *merged* configuration, so it holds
    the untracked overlay's half — the plugin triple, and the preset each slot
    was loaded from. Copying that block through is how a product name reaches a
    committed reference, and the file guard only sees it once someone has
    already measured and staged one.
    """
    tracked = {
        "id": "x",
        "label": "A method, stated without naming a product",
        "timbres": [{"id": "t", "label": "The registration, described"}],
    }
    manifest = {
        "plugin": "aumu:xxxx:Yyyy",
        "params": [],
        "sample_rate": 48000,
        "gate_ms": 1000,
        "tail": "2s",
        "preroll_ms": 100,
        "notes": [60],
        "velocities": [100],
        "timbres": [
            {"id": "t", "label": "Product Name 9 Concert", "preset": "Product/Close.vstpreset"},
            {"id": "model", "label": "libsonare, GM program 19"},
        ],
    }
    block = profile_module.committed_capture({"program": 19}, tracked, manifest)

    assert "plugin" not in block
    assert block["timbres"] == [{"id": "t", "label": "The registration, described"}]
    assert block["notes"] == [60] and block["gate_ms"] == 1000


def test_a_kit_is_recognised_from_the_channel_its_notes_are_played_on():
    """Which metric set a capture gets, decided where the distinction already lives.

    The failure this catches is silent and total: a kit whose channel went
    missing is measured with the pitched metric set, and every note of it comes
    back with a fundamental, a stretch and an inharmonicity, none of which a
    drum has. It reads as a successful measurement of the wrong instrument.
    """
    from capture import load_config

    here = Path(__file__).resolve().parent
    for name, percussion in (
        ("drums", True),
        ("piano", False),
        ("harpsichord", False),
        ("pipe_organ", False),
    ):
        cfg = load_config(here / "capture" / f"{name}.json")
        assert profile_module.is_percussion(cfg) is percussion, name

    # And from the committed reference, which is what `compare` actually reads:
    # the channel has to survive into the profile or a later comparison decides
    # differently from the measurement it is comparing against.
    reference = REFERENCE_DIR / "drums.json"
    if reference.exists():
        assert profile_module.is_percussion(json.loads(reference.read_text())["capture"])


def test_a_capture_that_mixes_a_kit_with_a_melodic_slot_is_refused():
    """One profile cannot be measured both ways, and picking one would be wrong twice."""
    with pytest.raises(ValueError, match="mixes percussion and melodic"):
        profile_module.is_percussion(
            {"timbres": [{"id": "kit", "channel": 10}, {"id": "lead", "channel": 1}]}
        )


def test_a_rack_slot_numbered_ten_holds_an_instrument_rather_than_a_kit():
    """The slot a rack keeps an instrument in says nothing about the instrument.

    A rack answers on sixteen channels and its tenth slot is a slot like any
    other, so whatever is loaded there is whatever was put there. Read as a
    semantic channel it is a drum map, and five melodic references were measured
    that way: a banjo, a flute, a glockenspiel, a steel guitar and a trombone
    came back with a band tilt and a crest per note and a fundamental for none.
    """
    from capture import load_config, slot_channel

    here = Path(__file__).resolve().parent / "capture"
    for name in ("banjo", "concert_flute", "glockenspiel", "steel_guitar", "trombone"):
        cfg = load_config(here / f"{name}.json")
        assert [slot_channel(t) for t in cfg["timbres"]] == [10], name
        assert not profile_module.is_percussion(cfg), name


def test_a_slot_number_written_as_the_semantic_channel_is_refused(tmp_path):
    """The guard on the field, rather than on the five captures that tripped it.

    Every rack capture here was written before the slot had a name of its own,
    so the same address was in the same field sixty-nine times over and only the
    five on slot 10 were measurably wrong. What the loader refuses is the shape:
    a channel that is neither of MIDI's two answers is an address.
    """
    from capture import load_config

    def written(**timbre) -> Path:
        path = tmp_path / "c.json"
        path.write_text(
            json.dumps(
                {
                    "id": "c",
                    "label": "x",
                    "notes": [60],
                    "velocities": [100],
                    "timbres": [{"id": "t", **timbre}],
                }
            )
        )
        return path

    with pytest.raises(ValueError, match="slot_channel"):
        load_config(written(channel=7))
    # The two MIDI does answer, and the address under its own name, all pass.
    assert not profile_module.is_percussion(load_config(written(channel=1)))
    assert profile_module.is_percussion(load_config(written(channel=10)))
    assert not profile_module.is_percussion(load_config(written(slot_channel=7)))


def test_every_shipped_capture_reads_as_one_kind_of_instrument():
    """All seventy, because the misread was found in five of them by hand.

    A capture that raises here is one whose timbres disagree about what a note
    number means; a melodic capture reading as percussion is measured with the
    kit metric set and reports it as a successful measurement.
    """
    from capture import load_config

    here = Path(__file__).resolve().parent / "capture"
    percussion = sorted(
        name
        for name in shipped_captures()
        if profile_module.is_percussion(load_config(here / f"{name}.json"))
    )
    assert len(shipped_captures()) >= 70
    # Four, and they are one kit read on two reference axes: `drums` is the
    # modern recording that answers its colour, and the three `drums_module`
    # grids the module that answers how it rings, damps and sits against
    # itself — one per gate, because a gate is what the analysis window is
    # promised to be and the kit's pieces do not all fill the same one.
    assert percussion == ["drums", "drums_module", "drums_module_hit", "drums_module_mid"]


def _corpus_manifest(root: Path, timbres: list[dict], *, config: str = "") -> Path:
    """A manifest thin enough for `load_corpus`, with no audio behind it.

    `load_corpus` resolves render paths and never opens them, so a grid of one
    slot per timbre is enough to ask what the corpus thinks its notes mean.
    """
    root.mkdir(parents=True, exist_ok=True)
    path = root / "manifest.json"
    path.write_text(
        json.dumps(
            {
                "id": "m",
                "config": config,
                "sample_rate": 48000,
                "gate_ms": 1000,
                "tail": "2s",
                "preroll_ms": 100,
                "notes": [60],
                "velocities": [100],
                "timbres": timbres,
                "renders": [
                    {
                        "id": f"{t['id']}/n060_v100",
                        "timbre": t["id"],
                        "note": 60,
                        "velocity": 100,
                        "path": f"{t['id']}/n060_v100.wav",
                        "seconds": 1.1,
                    }
                    for t in timbres
                ],
            }
        )
    )
    return path


def _tracked_capture(path: Path, timbres: list[dict]) -> str:
    path.write_text(
        json.dumps(
            {"id": path.stem, "label": "x", "notes": [60], "velocities": [100], "timbres": timbres}
        )
    )
    return str(path)


def test_a_corpus_captured_before_the_slot_had_a_name_reads_its_meaning_from_the_definition(
    tmp_path,
):
    """The manifest's copy is ambiguous where the definition is not.

    A manifest's timbre block is copied from the capture definition as it stood,
    so every corpus captured before the slot had a name of its own carries the
    rack slot under `channel`. Believing it makes the model's probe a drum probe
    and pairs the fit against the kit metric set — the same misread as the
    profile's, one file further on, and re-rendering 8.7 GB is not the fix.
    """
    from corpus import load_corpus

    config = _tracked_capture(
        tmp_path / "banjo.json", [{"id": "gm106", "label": "y", "slot_channel": 10}]
    )
    manifest = _corpus_manifest(
        tmp_path / "banjo", [{"id": "gm106", "label": "y", "channel": 10}], config=config
    )
    assert not load_corpus(manifest).percussive()

    # And a kit is still a kit, from the same ambiguous block.
    config = _tracked_capture(
        tmp_path / "drums.json", [{"id": "kit-a", "label": "y", "channel": 10}]
    )
    manifest = _corpus_manifest(
        tmp_path / "drums", [{"id": "kit-a", "label": "y", "channel": 10}], config=config
    )
    assert load_corpus(manifest).percussive()


def test_a_corpus_captured_since_the_split_answers_from_its_own_manifest(tmp_path):
    """`slot_channel` in the block is what says the two were separated when it
    was written, so `channel` beside it is the meaning and no definition has to
    be found to read it."""
    from corpus import load_corpus

    melodic = _corpus_manifest(tmp_path / "a", [{"id": "t", "slot_channel": 10}])
    assert not load_corpus(melodic).percussive()

    kit = _corpus_manifest(tmp_path / "b", [{"id": "t", "channel": 10, "slot_channel": 15}])
    assert load_corpus(kit).percussive()


def test_a_corpus_whose_definition_cannot_be_found_falls_back_to_its_own_block(tmp_path):
    """Which is every manifest written by hand or by a test, and the only answer
    left when a capture definition has been renamed out from under a corpus."""
    from corpus import load_corpus

    assert load_corpus(_corpus_manifest(tmp_path / "a", [{"id": "t", "channel": 10}])).percussive()
    assert load_corpus(
        _corpus_manifest(tmp_path / "b", [{"id": "t", "channel": 10}], config="nowhere/gone.json")
    ).percussive()


def test_the_definition_a_manifest_names_is_found_from_any_directory(monkeypatch, tmp_path):
    """A manifest records the path repo-relative, so a bare `Path.exists()`
    answers differently depending on where the harness was invoked from — and a
    miss here restores the ambiguity the lookup exists to resolve."""
    from corpus import _config_paths

    relative = "tools/voicematch/capture/drums.json"
    monkeypatch.chdir(tmp_path)
    assert not Path(relative).exists()
    assert [p.name for p in _config_paths(relative)] == ["drums.json"]
    assert list(_config_paths("")) == []


# --------------------------------------------------------------------------- #
# The captured layout, and the model's
# --------------------------------------------------------------------------- #
def test_a_capture_may_state_which_model_note_answers_each_of_its_own():
    """A drum note names an instrument, and a sampled kit need not use GM's order.

    The kit measured for `reference/drums.json` does not: its toms ascend
    45, 47, 48, 50, 41, 43. Without a map, a note-for-note comparison scores the
    low floor tom against the high one and reports a tuning error that is a
    mapping.
    """
    assert note_map({}) == {}
    assert note_map({"note_map": {"41": 45, "43": 47}}) == {41: 45, 43: 47}
    # Keys arrive from JSON as strings; both ends come back as ints so a caller
    # can look up a MIDI note without knowing where the config came from.
    mapped = note_map({"note_map": {"41": 45}})
    assert all(isinstance(k, int) and isinstance(v, int) for k, v in mapped.items())


def test_every_shipped_capture_s_note_map_names_notes_it_actually_captured():
    """A map entry for a note outside the grid is a typo that silently does nothing."""
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        mapping = note_map(cfg)
        if not mapping:
            continue
        captured = set(cfg.get("notes") or [])
        assert captured, f"{name} maps notes but lists none"
        unknown = sorted(set(mapping) - captured)
        assert not unknown, f"{name} maps notes it never captured: {unknown}"


# --------------------------------------------------------------------------- #
# The families inside a capture
# --------------------------------------------------------------------------- #
def test_every_shipped_tail_override_names_notes_the_capture_actually_holds():
    """A longer tail for a note the grid never records renders nothing at all.

    It reads as a decision — the belltree was named for ten seconds and the
    standard kit stops at 81 — and nothing in a capture run reports it, since
    the table is consulted per note of the grid and a note outside it is never
    looked up.
    """
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        table = cfg.get("tail_by_note") or {}
        if not table:
            continue
        captured = set(cfg.get("notes") or [])
        named: set[int] = set()
        for key in table:
            for part in str(key).split(","):
                part = part.strip()
                if "-" in part:
                    lo, hi = part.split("-", 1)
                    named |= set(range(int(lo), int(hi) + 1))
                elif part:
                    named.add(int(part))
        unknown = sorted(named - captured)
        assert not unknown, f"{name} gives a tail to notes it never captures: {unknown}"


def test_every_shipped_family_names_notes_the_capture_actually_holds():
    """A family naming a note outside the grid loses that member in silence."""
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups:
            continue
        captured = set(cfg.get("notes") or [])
        for family, notes in groups.items():
            assert len(notes) >= 2, f"{name}/{family} is not a family"
            unknown = sorted(set(notes) - captured)
            assert not unknown, f"{name}/{family} names uncaptured notes: {unknown}"


def test_a_reference_scores_no_kit_relation_against_itself():
    """The identity value, before any sweep of the term means anything.

    A relation term compares two contrast vectors, and a contrast is a
    subtraction against a median — arithmetic with several ways to come out
    non-zero on identical input. Exactly zero over a non-zero count is the only
    reading that says the term is measuring a difference rather than a method.
    """
    for name in shipped_captures():
        reference = REFERENCE_DIR / f"{name}.json"
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups or not reference.exists():
            continue
        rows = json.loads(reference.read_text())["rows"]
        value, count = _kit_terms(rows, rows, groups)
        assert count > 0, f"{name} declares families and none could be measured"
        assert value == 0.0


def test_every_shipped_family_holds_at_least_one_relation_in_its_own_rows():
    """A family whose members the reference cannot tell apart is not a family.

    Which relations a family has is measured rather than declared, so a group
    that survived nothing is one whose members are interchangeable in this
    capture — the whistle pair, whose lengths are the capture's gate — and
    declaring it puts a name in the file that scores nothing.
    """
    for name in shipped_captures():
        reference = REFERENCE_DIR / f"{name}.json"
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups or not reference.exists():
            continue
        rows = json.loads(reference.read_text())["rows"]
        held = {row["family"] for row in kit_report(rows, rows, groups)}
        assert set(groups) == held, f"{name}: {sorted(set(groups) - held)} hold nothing"
