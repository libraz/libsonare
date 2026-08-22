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
    DRY, Room, apply_room, estimate_room, fit_room_ir, room_distance, synth_room_ir,
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
