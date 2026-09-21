"""The captured corpus as a probe: the room it carries, which oracle may
carry one, the per-note analysis window and the hold-out check.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_corpus.py -q
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import itertools

import autofit
import loss as loss_module
from autofit import check_holdout_oracle, resolve_probe, validate
from autofit_test_fixtures import (
    _probe_args,
    _write_corpus,
)
from capture import (
    RIG_BAKED,
    RIG_NONE,
    RIG_UNCLASSIFIED,
    ROOM_NONE,
    ROOM_PRESENT,
    ROOM_UNCLASSIFIED,
)
from corpus import corpus_oracle, corpus_pattern, load_corpus
from loss import cli_weights, probe_rows
from patterns import (
    PATTERN_BUILDERS,
    analysis_window_end,
    build_pattern,
    pattern_length,
)
from render_oracle import oracle_may_carry_room
from room import DRY
from wavio import read_wav, write_wav


# --------------------------------------------------------------------------- #
# The room probe
# --------------------------------------------------------------------------- #
def test_the_room_probe_leaves_more_silence_than_it_makes_sound():
    assert "room-probe" in PATTERN_BUILDERS
    probe = build_pattern("room-probe", 19)
    assert probe.analysis_notes == []
    gaps = [b.start - (a.start + a.dur) for a, b in zip(probe.notes, probe.notes[1:])]
    assert gaps and min(gaps) >= 3.0
    assert max(n.dur for n in probe.notes) <= 0.5


# --------------------------------------------------------------------------- #
# What the probe overrides may be combined with
# --------------------------------------------------------------------------- #
def test_every_pattern_either_takes_a_probe_override_or_refuses_it_by_name():
    """`--pattern` offers every builder, so every builder has to answer both flags.

    Answering does not have to mean accepting. `drum-sequence` plays a written
    phrase and has neither axis — its notes and velocities ARE the phrase — so
    what it owes a caller is an error naming what it does take, not a
    `TypeError` about a parameter the flag never mentioned.
    """
    for name in PATTERN_BUILDERS:
        for axis, value in (("notes", (62,)), ("velocities", (90,))):
            try:
                assert build_pattern(name, 0, **{axis: value}).notes
            except ValueError as exc:
                assert f"has no {axis} axis" in str(exc)
                assert "sequence" in str(exc) or "neither" in str(exc)


def test_a_single_pitch_pattern_takes_the_override_as_its_pitch():
    assert {n.note for n in build_pattern("velocity", 0, notes=(62,)).notes} == {62}
    assert {n.velocity for n in build_pattern("sustain", 0, velocities=(90,)).notes} == {90}


def test_a_list_handed_to_a_single_pitch_pattern_is_refused_by_name():
    """It cannot mean anything, and it must not mean the first value silently."""
    with pytest.raises(ValueError, match="single value"):
        build_pattern("velocity", 0, notes=(60, 62))
    with pytest.raises(ValueError, match="single value"):
        build_pattern("scale", 0, velocities=(60, 90))


def test_a_pattern_with_neither_axis_names_what_it_takes(monkeypatch):
    def fixed(program: int, *, dur: float = 1.0):
        return build_pattern("sustain", program)

    monkeypatch.setitem(PATTERN_BUILDERS, "fixed", fixed)
    with pytest.raises(ValueError, match="neither notes nor velocities"):
        build_pattern("fixed", 0, notes=(60,))


# --------------------------------------------------------------------------- #
# The per-note analysis window
# --------------------------------------------------------------------------- #
class _FakeMetrics:
    """Stands in for a measurement so a window test costs no spectra."""

    def to_dict(self) -> dict:
        return {}


def _window_ends(pattern, monkeypatch) -> list[tuple[float, float]]:
    """(onset, window end) for every analysis note `probe_rows` measures."""
    seen: list[tuple[float, float]] = []

    def spy(mono, sr, note, end, **kwargs):
        seen.append((note.start, end))
        return _FakeMetrics()

    monkeypatch.setattr(loss_module, "analyze_note", spy)
    monkeypatch.setattr(loss_module, "analyze_hit", spy)
    monkeypatch.setattr(loss_module, "skeleton_note", lambda *a, **k: {})
    probe_rows(np.zeros(int(pattern_length(pattern) * 48000), dtype=np.float32),
               pattern, 48000)
    return seen


def test_the_default_probe_would_overrun_its_own_gap():
    """The clamp is not hypothetical: the tail outlasts the space before the next note."""
    probe = build_pattern("sustain", 0)
    first, second = probe.notes[0], probe.notes[1]
    assert first.start + first.dur + probe.tail > second.start
    assert analysis_window_end(probe, first) == second.start


def test_a_sustained_note_is_never_measured_into_the_next_one(monkeypatch):
    """Its release would otherwise be the next note's attack."""
    probe = build_pattern("sustain", 0)
    onsets = [n.start for n in probe.notes]
    measured = _window_ends(probe, monkeypatch)
    assert len(measured) == len(probe.analysis_notes)
    for start, end in measured:
        later = [s for s in onsets if s > start]
        assert not later or end <= min(later)


def test_a_drum_hit_keeps_the_window_it_always_had(monkeypatch):
    probe = build_pattern("drum", 0)
    onsets = [n.start for n in probe.notes]
    measured = _window_ends(probe, monkeypatch)
    assert len(measured) == len(probe.analysis_notes)
    for start, end in measured:
        later = [s for s in onsets if s > start]
        assert end == (min(later) if later else pattern_length(probe))


# --------------------------------------------------------------------------- #
# Which oracle carries a room
# --------------------------------------------------------------------------- #
def _oracle_args(**kwargs) -> argparse.Namespace:
    base = {"oracle_wav": "", "au": "", "au_dry": False, "room": "auto"}
    base.update(kwargs)
    return argparse.Namespace(**base)


def test_an_au_oracle_is_treated_as_wet_unless_it_was_asked_to_be_dry():
    """The AU route is the most likely of the three to arrive in a hall."""
    assert oracle_may_carry_room(_oracle_args(au="Pianoteq"))
    assert not oracle_may_carry_room(_oracle_args(au="Pianoteq", au_dry=True))


def test_the_fluidsynth_oracle_is_dry_by_construction():
    assert not oracle_may_carry_room(_oracle_args())
    assert oracle_may_carry_room(_oracle_args(oracle_wav="rendered.wav"))


def test_an_au_oracle_has_its_room_measured(monkeypatch):
    """The measurement is what the model is then convolved to match."""
    measured = []
    monkeypatch.setattr(autofit, "obtain_oracle",
                        lambda *a, **k: np.zeros((48000, 1), dtype=np.float32))
    monkeypatch.setattr(autofit, "probe_rows", lambda *a, **k: [])
    monkeypatch.setattr(autofit, "estimate_room",
                        lambda *a, **k: measured.append(1) or DRY)
    args = _probe_args(au="Pianoteq", au_dry=False, oracle_wav="", room="auto")
    resolve_probe(args)
    autofit.oracle_reference(args)
    assert measured == [1]


# --------------------------------------------------------------------------- #
# The hold-out check
# --------------------------------------------------------------------------- #
def test_a_hold_out_against_a_fixed_wav_is_refused_before_the_fit_starts():
    """The WAV holds the fitted notes; scoring the held-out ones against it is fiction."""
    with pytest.raises(ValueError, match="needs its own reference"):
        resolve_probe(_probe_args(oracle_wav="probe.wav", validate_notes="43,55,67"))


def test_a_hold_out_reference_satisfies_the_check():
    resolve_probe(_probe_args(oracle_wav="probe.wav", validate_notes="43,55,67",
                              validate_oracle_wav="holdout.wav"))


def test_a_re_rendering_oracle_route_needs_no_hold_out_reference():
    """fluidsynth and the AU host both render whatever score they are handed."""
    resolve_probe(_probe_args(validate_notes="43,55,67"))
    resolve_probe(_probe_args(au="Pianoteq", validate_notes="43,55,67"))


def test_a_hold_out_reference_without_a_fit_reference_is_refused():
    with pytest.raises(ValueError, match="belongs with --oracle-wav"):
        check_holdout_oracle(_probe_args(validate_oracle_wav="holdout.wav"))


def test_the_hold_out_is_scored_against_its_own_reference(monkeypatch):
    seen: dict[str, str] = {}

    def fake_oracle(holdout):
        seen["wav"] = holdout.oracle_wav
        seen["notes"] = holdout.notes
        # Rows, audio, room, band edge — the edge is a percussion quantity and
        # this is a pitched probe, so None is what the real one returns here.
        return [], None, None, None

    monkeypatch.setattr(autofit, "oracle_reference", fake_oracle)
    args = _probe_args(oracle_wav="probe.wav", validate_notes="43,55,67",
                       validate_oracle_wav="holdout.wav")
    resolve_probe(args)
    assert validate(args, Path("."), [], [], [], None) is None
    assert seen == {"wav": "holdout.wav", "notes": "43,55,67"}


# --------------------------------------------------------------------------- #
# The captured corpus as a probe
# --------------------------------------------------------------------------- #
def test_a_capture_that_lays_its_instruments_out_differently_is_answered_note_for_note(
    tmp_path,
):
    """The kit reference ascends its six toms as 45, 47, 48, 50, 41, 43, so a
    model that follows General MIDI has to be struck on the note of the same
    RANK or every tom is fitted against a different sized drum. `profile.py
    compare` has always read `note_map`; the corpus the fit scores against did
    not, so the two disagreed about which drum a number meant."""
    root = _write_corpus(tmp_path / "c", notes=(60, 72), velocities=(56,),
                         note_map={"60": 72, "72": 60})
    corpus = load_corpus(root)
    assert corpus.note_map == {60: 72, 72: 60}
    # The probe strikes the model's notes, in the captured notes' order.
    probe = corpus_pattern(corpus)
    assert [n.note for n in probe.notes] == [72, 60]
    assert [corpus.capture_slot(n.note) for n in probe.notes] == [60, 72]

    # The reference is what it was recorded as. "Applied to the oracle side
    # only" means the MODEL moves to meet it, so the assembly is untouched and
    # a slot still holds the note it was captured on: the first is 60's pitch,
    # under a probe that strikes 72 there.
    plain_root = _write_corpus(tmp_path / "d", notes=(60, 72), velocities=(56,))
    plain_corpus = load_corpus(plain_root)
    assert np.allclose(corpus_oracle(corpus, probe, 48000),
                       corpus_oracle(plain_corpus, corpus_pattern(plain_corpus), 48000))
    first = corpus_oracle(corpus, probe, 48000)[4800:24800, 0]
    peak = float(np.argmax(np.abs(np.fft.rfft(first)))) * 48000.0 / len(first)
    assert peak == pytest.approx(440.0 * 2 ** ((60 - 69) / 12.0), rel=0.02)

    # A capture that declared nothing is untouched: both numberings agree.
    bare = load_corpus(_write_corpus(tmp_path / "e"))
    assert bare.note_map == {}
    assert bare.played_notes() == bare.notes
    assert [n.note for n in corpus_pattern(bare).notes] == [60, 60, 72, 72]


def test_a_corpus_probe_is_laid_out_by_the_capture_not_by_a_builder(tmp_path):
    """Its notes, its velocities and its gate all come from the manifest."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus)
    assert [(n.note, n.velocity) for n in probe.notes] == [
        (60, 56), (60, 120), (72, 56), (72, 120)
    ]
    assert {n.dur for n in probe.notes} == {8.0}
    assert probe.analysis_notes == probe.notes


def test_corpus_slots_are_spaced_by_the_capture_s_own_length(tmp_path):
    """So a note's analysis window is exactly the audio recorded for it.

    A gap chosen independently of the capture would either cut the reference's
    tail off or leave the model ringing into the next slot, and either one is a
    decay metric measuring the layout rather than the voice.
    """
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus)
    starts = [n.start for n in probe.notes]
    assert all(b - a == pytest.approx(corpus.slot_s) for a, b in itertools.pairwise(starts))
    # 10.1 s captured minus the 0.1 s preroll that is dropped on assembly.
    assert corpus.slot_s == pytest.approx(10.0)
    for note in probe.notes:
        assert analysis_window_end(probe, note) == pytest.approx(note.start + corpus.slot_s)


def test_a_note_captured_for_longer_is_analysed_for_longer(tmp_path):
    """A grid with a per-note tail gives each slot the window it was recorded in.

    A kit records eight seconds for a ride and two for a kick, because a cymbal's
    wash is most of what makes it a cymbal. Reduced to one slot length the whole
    grid takes the short one, and six seconds of ride are dropped before any
    metric sees them — which reads as a model whose bands all decay too fast,
    on the notes the longer tail was captured for.
    """
    root = _write_corpus(tmp_path / "c", gate_ms=1000,
                         seconds={60: 4.1, 72: 10.1})
    corpus = load_corpus(root)
    probe = corpus_pattern(corpus, velocities=(56,))
    starts = [n.start for n in probe.notes]
    # The short note is followed by its own 4 s, not by the grid's longest and
    # not by the flat gate + tail that a single length would have fallen back to.
    assert starts == pytest.approx([0.0, 4.0])
    assert probe.tail == pytest.approx(10.0 - 1.0)

    assert corpus.slot_for(60, 56) == pytest.approx(4.0)
    assert corpus.slot_for(72, 56) == pytest.approx(10.0)
    for note in probe.notes:
        assert analysis_window_end(probe, note) == pytest.approx(
            note.start + corpus.slot_for(note.note, note.velocity)
        )


def test_the_long_note_keeps_its_tail_when_the_grid_also_holds_short_ones(tmp_path):
    """The assembled oracle carries the full capture, not the shortest slot's worth."""
    root = _write_corpus(tmp_path / "c", gate_ms=1000,
                         seconds={60: 4.1, 72: 10.1})
    corpus = load_corpus(root)
    probe = corpus_pattern(corpus, notes=(72,), velocities=(56,))
    audio = corpus_oracle(corpus, probe, 48000)
    # 10.1 s captured less the 0.1 s preroll that assembly drops.
    assert len(audio) / 48000.0 == pytest.approx(10.0, abs=0.01)


def test_the_corpus_oracle_places_each_capture_at_its_own_onset(tmp_path):
    """The preroll is dropped, so a captured onset lands where the model's does."""
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    probe = corpus_pattern(corpus, notes=(60,), velocities=(120,))
    audio = corpus_oracle(corpus, probe, 48000)
    mono = np.abs(audio).mean(axis=1)
    onset = int(np.argmax(mono > 0.01)) / 48000.0
    assert onset == pytest.approx(probe.notes[0].start, abs=0.005)


def test_the_corpus_oracle_takes_out_the_slack_the_capture_guard_let_through(tmp_path):
    """A slot is placed where it sounds, not a nominal preroll in.

    `capture.ONSET_SLACK_MS` refuses a render for sounding LATE and cannot
    refuse one for being early, so what survives it is one-sided: the reference
    arrives after the model on every slot of the grid, by up to the slack, and
    every timing measurement taken against it carries that the same way. The
    model's own render has no such offset, being written straight from the
    score, so none of it cancels.
    """
    sr = 48000
    late_s = 0.008
    root = _write_corpus(tmp_path / "c", notes=(60,), velocities=(120,))
    path = root / "t" / "n060_v120.wav"
    audio, _ = read_wav(path)
    pad = np.zeros((int(late_s * sr), audio.shape[1]), dtype=np.float32)
    write_wav(path, np.concatenate([pad, audio[: -len(pad)]]).astype(np.float32), sr)

    corpus = load_corpus(root)
    probe = corpus_pattern(corpus, notes=(60,), velocities=(120,))
    mono = np.abs(corpus_oracle(corpus, probe, sr)).mean(axis=1)
    onset = int(np.argmax(mono > 0.01)) / sr

    assert onset == pytest.approx(probe.notes[0].start, abs=0.003)


def test_a_corpus_run_refuses_a_grid_it_has_no_captures_for(tmp_path):
    corpus = load_corpus(_write_corpus(tmp_path / "c"))
    with pytest.raises(ValueError, match="no capture"):
        corpus_pattern(corpus, notes=(60, 61), velocities=(56,))


def test_a_corpus_run_refuses_the_oracle_routes_that_would_contradict_it(tmp_path):
    args = _probe_args(corpus=str(_write_corpus(tmp_path / "c")), oracle_wav="/tmp/x.wav")
    with pytest.raises(ValueError, match="both name the reference"):
        resolve_probe(args)
    args = _probe_args(corpus=str(tmp_path / "c"), drum_note=38)
    with pytest.raises(ValueError, match="pitched single notes"):
        resolve_probe(args)


def test_a_kit_corpus_and_a_drum_note_go_together_and_each_needs_the_other(tmp_path):
    """The pairing is decided by the capture's channel, not by the flag alone.

    A kit corpus IS a grid the drum probe has captures for, which is what makes
    the two compatible; the refusal that used to be unconditional was written
    before one existed. Both one-sided combinations stay refused: a drum probe
    over a pitched corpus scores every slot against silence, and a kit corpus
    with no drum note has no single patch to move, since a kit has one per note.
    """
    kit = _write_corpus(tmp_path / "kit", notes=(38, 42), velocities=(56, 120), channel=10)
    corpus = load_corpus(kit)
    assert corpus.percussive()
    probe = corpus_pattern(corpus, notes=(42,), velocities=(56, 120))
    assert probe.percussive and probe.channel == 9

    args = _probe_args(corpus=str(kit), drum_note=42)
    resolve_probe(args)
    assert args.percussive

    args = _probe_args(corpus=str(kit))
    with pytest.raises(ValueError, match="pass --drum-note"):
        resolve_probe(args)


def test_a_corpus_probe_reports_its_dryness_from_the_capture_config(tmp_path):
    """A wet capture has to be measured; a dry one must not have a room invented for it."""
    assert load_corpus(_write_corpus(tmp_path / "dry", dry=True)).dry is True
    assert load_corpus(_write_corpus(tmp_path / "wet", dry=False)).dry is False


def test_a_corpus_carries_what_its_capture_answered_about_a_space(tmp_path):
    """`dry` and `room` are separate answers and only the second is about a room.

    A plugin advertising no effect section answers `dry: false` for want of
    anything to switch off, which is the shape of most captures here — and read
    on its own it puts a room on a reference that was measured to have none.
    """
    assert load_corpus(_write_corpus(tmp_path / "u")).room == ROOM_UNCLASSIFIED
    assert load_corpus(_write_corpus(tmp_path / "n", room=ROOM_NONE)).room == ROOM_NONE
    assert load_corpus(_write_corpus(tmp_path / "p", room=ROOM_PRESENT)).room == ROOM_PRESENT
    # The pair that motivated the field, and the one the room gate has to read
    # as no space: nothing to switch off, and nothing there to switch it in.
    both = load_corpus(_write_corpus(tmp_path / "b", dry=False, room=ROOM_NONE))
    assert both.dry is False and both.room == ROOM_NONE
    # Same rule as the rig: an answer nothing understands is an unanswered one,
    # since reading it as `none` would skip the correction on a wet reference.
    assert load_corpus(_write_corpus(tmp_path / "typo", room="dry")).room == ROOM_UNCLASSIFIED


def test_a_corpus_carries_what_its_capture_answered_about_a_rig(tmp_path):
    """And a manifest that never answered reads as unclassified, not as `none`."""
    assert load_corpus(_write_corpus(tmp_path / "u")).rig == RIG_UNCLASSIFIED
    assert load_corpus(_write_corpus(tmp_path / "n", rig=RIG_NONE)).rig == RIG_NONE
    assert load_corpus(_write_corpus(tmp_path / "b", rig=RIG_BAKED)).rig == RIG_BAKED
    # Dry and rigged together is the pair the record exists for: a close-mic'd
    # amplifier has no tail, so dryness reads clean with the whole rig in it.
    rigged = load_corpus(_write_corpus(tmp_path / "amp", dry=True, rig=RIG_BAKED))
    assert rigged.dry is True and rigged.rig == RIG_BAKED
    # An answer nothing understands is an unanswered one. `load_config` refuses a
    # misspelling outright; a hand-edited manifest never goes through it, and
    # reading "DI" as `none` would let a rigged reference into a fit.
    assert load_corpus(_write_corpus(tmp_path / "typo", rig="DI")).rig == RIG_UNCLASSIFIED


def test_the_model_stops_on_the_same_side_of_the_boundary_the_reference_did():
    """The rig record drives the model render, not only the fit refusal.

    A direct reference is compared against the model's direct signal; one
    recorded through an amplifier against the model plus its rig. Getting this
    backwards measures the amplifier as if it were the string, and the model
    side is exactly where nothing would complain.
    """
    from capture import model_rig

    assert model_rig(RIG_NONE) is False
    assert model_rig(RIG_BAKED) is True
    # Unanswered gets the rig, which is the product sound. Comparing is what an
    # unclassified reference is still allowed to do; fitting is what it is not,
    # and that refusal is `check_rig`'s rather than this function's.
    assert model_rig(RIG_UNCLASSIFIED) is True


def test_a_fit_against_a_rigged_reference_is_refused(tmp_path):
    """A rig has no inverse, so unlike a room it cannot be measured out of the
    reference; the fit would reproduce an amplifier with the instrument's own
    parameters and lose the values the moment the rig became a stage."""
    root = _write_corpus(tmp_path / "amp", rig=RIG_BAKED)
    with pytest.raises(ValueError, match="carries a rig"):
        resolve_probe(_probe_args(program=30, corpus=str(root)))
    # `baked` is an answer wherever it is written, so the family table does not
    # get to overrule one.
    with pytest.raises(ValueError, match="carries a rig"):
        resolve_probe(_probe_args(program=0, corpus=str(root)))


def test_an_unanswered_rig_stops_a_fit_only_where_a_rig_is_possible(tmp_path):
    """Seventy captures predate the question, and answering them all is not the
    price of fitting a flute. Where a rig IS possible the silence is the hazard:
    the rigged reference is usually the one that already exists."""
    root = _write_corpus(tmp_path / "u")
    with pytest.raises(ValueError, match="nothing says whether"):
        resolve_probe(_probe_args(program=30, corpus=str(root)))
    for program in (0, 19, 73):  # a piano, a church organ's building, a flute
        args = _probe_args(program=program, corpus=str(root))
        resolve_probe(args)
        assert args.pattern == "corpus"


def test_a_reference_captured_at_the_instrument_s_boundary_fits(tmp_path):
    """`none` is the answer a fit is for, on the family that could have said otherwise."""
    args = _probe_args(program=30, corpus=str(_write_corpus(tmp_path / "di", rig=RIG_NONE)))
    resolve_probe(args)
    assert args.pattern == "corpus"


def test_comparing_and_diagnosing_a_rigged_reference_are_unaffected(tmp_path):
    """The gate is on the fit alone. Checking the instrument-and-rig against a
    reference that carries one is the acceptance measurement the rule asks for,
    and a diagnosis reports which knobs reach which term rather than moving any
    of them towards the reference."""
    root = _write_corpus(tmp_path / "amp", rig=RIG_BAKED)
    args = _probe_args(program=30, corpus=str(root), diagnose=True)
    resolve_probe(args)
    assert args.pattern == "corpus"
    corpus = load_corpus(root)
    assert corpus_oracle(corpus, corpus_pattern(corpus), 48000).size


def test_the_rig_refusal_can_be_pushed_through_and_says_so(tmp_path, capsys):
    """Explicitly, and loudly: the values a forced run produces transfer to nothing."""
    args = _probe_args(program=30, allow_rigged_oracle=True,
                       corpus=str(_write_corpus(tmp_path / "amp", rig=RIG_BAKED)))
    resolve_probe(args)
    assert args.pattern == "corpus"
    assert "--allow-rigged-oracle" in capsys.readouterr().err


def test_an_oracle_route_with_no_rig_record_warns_where_a_rig_is_possible(capsys):
    """`--oracle-wav` and `--au` cannot answer the question a capture answers,
    and the hazard is the same size. Warned rather than refused, since nothing on
    this route knows the answer — but not silent, because being unclassifiable
    and being safe are different things."""
    for route in ({"oracle_wav": "amp.wav"}, {"au": "SomeAmpSim"}):
        resolve_probe(_probe_args(program=30, **route))
        assert "carries no record of a rig" in capsys.readouterr().err
    # Not on a family nobody is waiting on.
    resolve_probe(_probe_args(program=73, oracle_wav="flute.wav"))
    assert "rig" not in capsys.readouterr().err


def test_the_built_in_gm_oracle_is_refused_on_a_family_it_cannot_be_a_di_for():
    """Not "unclassified" — known. General MIDI defines these programs by the
    sound of an amplified instrument, so a sample set's recording of one has the
    cabinet in it, and no capture field can change that answer."""
    with pytest.raises(ValueError, match="cannot be a DI"):
        resolve_probe(_probe_args(program=30))
    with pytest.raises(ValueError, match="cannot be a DI"):
        resolve_probe(_probe_args(program=33, sf2="general.sf2"))
    # A flute is not waiting on anyone, here as everywhere.
    resolve_probe(_probe_args(program=73))


def test_the_gm_oracle_refusal_takes_the_same_override_as_the_capture_one(capsys):
    """One flag, because the certainty is not uniform across the family — 29 and
    30 are definitional while 33-37 are only usual — and a second table splitting
    the sure from the likely is the thing that drifts."""
    args = _probe_args(program=33, allow_rigged_oracle=True)
    resolve_probe(args)
    err = capsys.readouterr().err
    assert "--allow-rigged-oracle" in err and "cannot be a DI" in err


def test_a_corpus_carries_the_families_its_capture_identified(tmp_path):
    """A kit's own note numbers say nothing about which of them are one drum."""
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    assert load_corpus(kit).groups == {"toms": (41, 43, 45)}
    # A capture that named none gives none rather than a guess from note numbers,
    # and so does a manifest written before the block existed.
    assert load_corpus(_write_corpus(tmp_path / "plain")).groups == {}


def test_the_kit_relations_are_dropped_when_a_probe_has_no_family_to_read(tmp_path):
    """`--drum-note N` narrows the grid to that one note unless --notes says
    otherwise, so the default drum fit has no family in it at all. The class
    default supplies `kit` for every drum fit and it turns itself off there,
    rather than scoring 0.0 — which is also its best value.
    """
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    one = _probe_args(corpus=str(kit), drum_note=41)
    resolve_probe(one)
    assert one.notes == "41"
    assert one.has_kit_groups is False
    assert "kit" not in cli_weights(one)

    whole = _probe_args(corpus=str(kit), drum_note=41, notes="41,43,45")
    resolve_probe(whole)
    assert whole.has_kit_groups is True
    assert cli_weights(whole)["kit"] == 1.0


def test_an_explicit_kit_weight_is_refused_rather_than_scored_at_its_best(tmp_path):
    """Dropping a class default is right; dropping what someone asked for is not."""
    kit = _write_corpus(tmp_path / "kit", notes=(41, 43, 45), velocities=(56, 120),
                        channel=10, groups={"toms": [41, 43, 45]})
    args = _probe_args(corpus=str(kit), drum_note=41, w_kit=1.0)
    with pytest.raises(ValueError, match="relations inside a kit"):
        resolve_probe(args)
