"""Round-trip tests for the ambience model.

The estimator's job is to be right about whether there is a room at all, and
roughly right about how big it is. Both matter in the same direction: every
model render is convolved with what it reports, so inventing a room corrupts
the metrics it exists to protect, and missing one leaves them corrupted anyway.

    python -m pytest tools/voicematch/test_room.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from room import (
    DRY, Room, apply_room, estimate_room, fit_room_ir, match_sends, measurable_room,
    place_model_in, room_distance, room_span_distance, synth_room_ir,
)

SR = 48000


def _pluck(sr: int, freq: float, dur: float, decay_s: float) -> np.ndarray:
    """A dry decaying harmonic tone: an instrument with no room around it.

    Deliberately bright-but-fast: harmonics decay *faster* than the
    fundamental, the way a real instrument's do, which is what makes the
    estimator's high-frequency plausibility gate meaningful.
    """
    t = np.arange(int(dur * sr)) / sr
    out = np.zeros_like(t)
    for k in (1, 2, 3, 4, 5):
        out += (1.0 / k) * np.sin(2 * np.pi * freq * k * t) * np.exp(-t / (decay_s / k))
    return out.astype(np.float32)


def _score(sr: int, notes: int = 4, gap: float = 2.5) -> tuple[np.ndarray, list[tuple[float, float]]]:
    """A few spaced notes plus their (start, end) spans."""
    total = int((notes * gap + 3.0) * sr)
    audio = np.zeros((total, 2), dtype=np.float32)
    spans: list[tuple[float, float]] = []
    for i in range(notes):
        on = 0.5 + i * gap
        start = int(on * sr)
        tone = _pluck(sr, 220.0 * (1 + 0.25 * i), 1.0, 0.35)
        audio[start : start + len(tone), 0] += tone
        audio[start : start + len(tone), 1] += tone
        spans.append((on, on + 1.0))
    return audio, spans


def test_dry_signal_reads_as_dry():
    audio, spans = _score(SR)
    room = estimate_room(audio, SR, spans)
    assert room.is_dry(), f"dry tones reported a room: {room}"


def test_silence_reads_as_dry():
    assert estimate_room(np.zeros((SR, 2), dtype=np.float32), SR, [(0.0, 0.2)]).is_dry()


def test_no_offsets_reads_as_dry():
    audio, _ = _score(SR)
    assert estimate_room(audio, SR, []).is_dry()


@pytest.mark.parametrize("rt60", [0.8, 1.6, 3.0])
def test_rt60_round_trips(rt60: float):
    """A known room convolved onto dry tones is measured back to within 25 %.

    Loose on purpose: the measurement window holds the instrument's own decay
    as well as the room's, so the estimate is biased long by construction. What
    it has to get right is the order of magnitude and the direction.
    """
    audio, spans = _score(SR)
    truth = Room(rt60_s=rt60, hf_ratio=0.5, tail_db=0.0, predelay_ms=15.0)
    wet = apply_room(audio, synth_room_ir(truth, SR))
    got = estimate_room(wet, SR, spans)
    assert not got.is_dry()
    assert 0.75 * rt60 <= got.rt60_s <= 1.35 * rt60, f"{rt60} -> {got.rt60_s}"


@pytest.mark.parametrize("rt60", [0.8, 1.6, 3.0])
def test_fitted_ir_lands_the_model_in_the_measured_room(rt60: float):
    """The production path: measure an oracle's space, then reproduce it.

    This is the property that matters — not that the fit recovers the room's
    true parameters (it cannot; the instrument's own ring is inside the
    window), but that the model, run through the fitted response, *measures*
    the same as the oracle did through the same estimator. Only then are the
    timbre deltas between them free of the room.
    """
    audio, spans = _score(SR)
    oracle = apply_room(audio, synth_room_ir(Room(rt60, 0.5, 0.0, 15.0), SR))
    target = estimate_room(oracle, SR, spans)
    assert not target.is_dry()

    roomed = apply_room(audio, fit_room_ir(audio, SR, spans, target))
    got = estimate_room(roomed, SR, spans)
    assert room_distance(got, target) < 0.5, f"{got} vs {target}"


def test_fitted_ir_for_a_dry_target_is_a_passthrough():
    audio, spans = _score(SR)
    assert np.array_equal(apply_room(audio, fit_room_ir(audio, SR, spans, DRY)), audio)


def test_tail_level_orders_correctly():
    """A wetter room leaves more energy after note-off, so `tail_db` falls."""
    audio, spans = _score(SR)
    near = estimate_room(
        apply_room(audio, synth_room_ir(Room(1.5, 0.5, 6.0, 15.0), SR)), SR, spans
    )
    far = estimate_room(
        apply_room(audio, synth_room_ir(Room(1.5, 0.5, -6.0, 15.0), SR)), SR, spans
    )
    assert far.tail_db < near.tail_db


def test_hf_brighter_than_lf_is_rejected():
    """A decay that rings brighter for longer is the instrument, not a room.

    Air absorption and every real absorber damp high frequencies faster than
    low, so an inverted ratio cannot come from a space.
    """
    audio, spans = _score(SR)
    inverted = Room(rt60_s=2.0, hf_ratio=1.5, tail_db=0.0, predelay_ms=15.0)
    wet = apply_room(audio, synth_room_ir(inverted, SR))
    assert estimate_room(wet, SR, spans).is_dry()


def test_dry_room_ir_is_a_passthrough():
    audio, _ = _score(SR)
    out = apply_room(audio, synth_room_ir(DRY, SR))
    assert np.array_equal(out, audio)


def test_apply_room_preserves_length():
    audio, _ = _score(SR)
    out = apply_room(audio, synth_room_ir(Room(2.0, 0.5, 0.0, 15.0), SR))
    assert out.shape == audio.shape


def test_room_distance_is_zero_for_itself():
    r = Room(1.8, 0.5, -4.0, 15.0)
    assert room_distance(r, r) == 0.0


def test_room_distance_grows_with_error():
    target = Room(1.8, 0.5, -4.0, 15.0)
    near = Room(1.9, 0.5, -4.5, 15.0)
    far = Room(4.0, 0.5, -12.0, 15.0)
    assert room_distance(near, target) < room_distance(far, target)


def test_a_value_between_two_references_is_as_close_as_the_corpus_can_define():
    refs = [Room(3.19, 0.68, 19.3, 15.0), Room(3.69, 0.47, 20.9, 15.0)]
    assert room_span_distance(Room(3.4, 0.5, 20.0, 15.0), refs) == 0.0
    assert room_span_distance(Room(3.19, 0.5, 19.3, 15.0), refs) == 0.0
    assert room_span_distance(Room(2.88, 0.5, 18.3, 15.0), refs) > 0.0


def test_the_span_still_ranks_two_points_outside_it():
    refs = [Room(3.19, 0.68, 19.3, 15.0), Room(3.69, 0.47, 20.9, 15.0)]
    near = room_span_distance(Room(2.9, 0.5, 19.5, 15.0), refs)
    far = room_span_distance(Room(1.7, 0.5, 19.5, 15.0), refs)
    assert 0.0 < near < far


def test_one_reference_scores_exactly_as_the_pairwise_distance_did():
    only = Room(3.19, 0.68, 19.3, 15.0)
    got = Room(2.5, 0.5, 17.7, 15.0)
    assert room_span_distance(got, [only]) == room_distance(got, only)


def test_the_search_prefers_inside_the_span_over_nearest_one_reference():
    """The defect the span fixes, at the numbers that exposed it.

    Aimed at plenum-a alone the search picked the 2.88 s tank over the 3.60 s
    one, and 2.88 is outside both references. Nothing in a pairwise distance can
    see that, because 2.88 genuinely is nearer to 3.19 than 3.60 is.
    """
    refs = [Room(3.19, 0.68, 19.3, 15.0), Room(3.69, 0.47, 20.9, 15.0)]
    short, inside = Room(2.88, 0.5, 18.3, 15.0), Room(3.60, 0.5, 18.0, 15.0)
    assert room_distance(short, refs[0]) < room_distance(inside, refs[0])
    assert room_span_distance(inside, refs) < room_span_distance(short, refs)


def test_match_sends_takes_a_list_and_scores_the_span(monkeypatch):
    refs = [Room(3.19, 0.68, 19.3, 15.0), Room(3.69, 0.47, 20.9, 15.0)]
    grid = {}

    def measure(cc91: int, decay_scale: float) -> Room:
        # RT60 rises with the tank, tail level falls with the send: the measured
        # separation this search relies on, reduced to a stub.
        got = Room(1.5 + 2.2 * decay_scale, 0.5, 22.0 - cc91 * 0.05, 15.0)
        grid[(cc91, decay_scale)] = got
        return got

    result = match_sends(refs, measure, log=None)
    assert not result["dry"]
    assert result["residual"] == 0.0
    reached = result["measured"]
    assert 3.19 <= reached["rt60_s"] <= 3.69
    assert 19.3 <= reached["tail_db"] <= 20.9


def test_match_sends_is_dry_only_when_every_reference_is():
    called = []

    def measure(cc91, decay_scale):
        called.append((cc91, decay_scale))
        return Room(2.0, 0.5, 18.0, 15.0)

    assert match_sends([DRY, DRY], measure)["dry"] is True
    assert not called
    assert match_sends([DRY, Room(3.2, 0.6, 19.0, 15.0)], measure)["dry"] is False
    assert called


def _hits(sr: int, gate: float, ring: float, notes: int = 3, gap: float = 2.0):
    """Percussive hits and their (start, end) spans, held for `gate` seconds.

    The span is the gate, exactly as a drum probe writes it: on a drum channel
    the note-off carries no information, so the key is released long before the
    instrument has finished ringing.
    """
    total = int((notes * gap + 3.0) * sr)
    audio = np.zeros((total, 2), dtype=np.float32)
    spans: list[tuple[float, float]] = []
    rng = np.random.default_rng(0xD5)
    for i in range(notes):
        on = 0.1 + i * gap
        t = np.arange(int(ring * sr)) / sr
        hit = (rng.standard_normal(len(t)) * np.exp(-t / (ring / 6.0))).astype(np.float32)
        audio[int(on * sr) : int(on * sr) + len(hit), 0] += hit
        audio[int(on * sr) : int(on * sr) + len(hit), 1] += hit
        spans.append((on, on + gate))
    return audio, spans


def test_a_gated_probe_is_refused_rather_than_fitted():
    """A 50 ms gate cannot measure a room, and says so instead of inventing one.

    The instrument outlasts its own note window by a factor of ten, so the split
    `tail_db` makes at note-off puts almost all of the drum on the tail side and
    reports how briefly the key was held.
    """
    audio, spans = _hits(SR, gate=0.05, ring=0.35)
    wet = apply_room(audio, synth_room_ir(Room(1.2, 0.6, 6.0, 15.0), SR))
    room = estimate_room(wet, SR, spans)
    assert not room.is_dry(), "the test needs a room to be found before it can be refused"
    assert room.gated(), f"a 50 ms gate against RT60 {room.rt60_s:.2f}s was accepted: {room}"


def test_a_probe_that_holds_its_notes_is_not_refused():
    """The pitched patterns must keep their correction: they hold notes for seconds."""
    audio, spans = _score(SR)
    wet = apply_room(audio, synth_room_ir(Room(1.2, 0.6, 6.0, 15.0), SR))
    room = estimate_room(wet, SR, spans)
    assert not room.is_dry() and not room.gated(), f"a 1 s note against {room.rt60_s:.2f}s: {room}"


def test_a_constructed_room_is_never_reported_as_gated():
    """`gated` is a property of a measurement; a hand-built Room has no windows."""
    assert not Room(1.2, 0.6, 6.0, 15.0).gated()
    assert not DRY.gated()


def test_measurable_room_refuses_the_two_cases_it_must():
    """The gate function returns None exactly where the correction is unsafe."""
    dry, spans = _score(SR)
    assert measurable_room(dry, SR, spans) is None, "a dry reference was accepted"

    hits, gated_spans = _hits(SR, gate=0.05, ring=0.35)
    wet_hits = apply_room(hits, synth_room_ir(Room(1.2, 0.6, 6.0, 15.0), SR))
    assert measurable_room(wet_hits, SR, gated_spans) is None, "a gated probe was accepted"

    wet = apply_room(dry, synth_room_ir(Room(1.2, 0.6, 6.0, 15.0), SR))
    room = measurable_room(wet, SR, spans)
    assert room is not None and room.rt60_s > 0.35


def test_a_reused_ir_places_a_probe_that_could_not_measure_its_own_room():
    """A gated phrase is corrected by the IR a held-note phrase from the same
    reference gave up, and that is not what `Room.gated` refuses.

    Without this every short-note take of a wet capture is read dry against a
    wet reference, which is a difference of tens of dB in the tail bands and
    lands on every one of them at once.
    """
    long_dry, long_spans = _score(SR)
    room_ir = synth_room_ir(Room(1.2, 0.6, 6.0, 15.0), SR)
    room = measurable_room(apply_room(long_dry, room_ir), SR, long_spans)
    assert room is not None
    _, ir = place_model_in(long_dry, SR, long_spans, room)

    short, short_spans = _hits(SR, gate=0.05, ring=0.35)
    assert measurable_room(apply_room(short, room_ir), SR, short_spans) is None
    placed, reused = place_model_in(short, SR, short_spans, room, ir)
    assert reused is ir, "the fit ran again instead of reusing the IR"
    assert float(np.abs(placed).sum()) > float(np.abs(short).sum()), "the IR did not reach the audio"
