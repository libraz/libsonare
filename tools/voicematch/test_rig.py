"""Tests for the rig evidence measurements.

The measurement's job is to be right about the direction of its own evidence: a
shared filter has to read as shared, a rotor as anti-phase, and a question that
cannot be put has to say so rather than come back as a "no". The last of those
is the one that matters most, since a vacuous negative is what would put `none`
into a capture that never earned it.

    python -m pytest tools/voicematch/test_rig.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from corpus import load_corpus  # noqa: E402
from rig import curve_distance, measure_rotary, measure_skirt  # noqa: E402
from wavio import write_wav  # noqa: E402

SR = 48000
NOTES = (40, 47, 54)
VELOCITY = 100


def _pluck(freq: float, seconds: float = 2.0) -> np.ndarray:
    """A bright plucked tone: partials all the way up, so a filter has something to cut."""
    t = np.arange(int(SR * seconds)) / SR
    out = np.zeros_like(t)
    for harmonic in range(1, 120):
        f = freq * harmonic
        if f >= SR / 2:
            break
        out += np.sin(2 * np.pi * f * t + harmonic) / harmonic**0.5
    return out * np.exp(-t * 1.5) / 12.0


def _lowpass(x: np.ndarray, cutoff: float, order: int) -> np.ndarray:
    """A steep zero-phase lowpass, standing in for a cabinet."""
    spectrum = np.fft.rfft(x)
    freq = np.fft.rfftfreq(x.size, 1.0 / SR)
    with np.errstate(divide="ignore"):
        response = 1.0 / np.sqrt(1.0 + (np.maximum(freq, 1e-9) / cutoff) ** (2 * order))
    return np.fft.irfft(spectrum * response, x.size)


def _corpus(tmp_path: Path, name: str, render, gate_ms: int = 1500) -> object:
    """Write a small grid as a corpus and load it back."""
    import json

    root = tmp_path / name
    (root / "t").mkdir(parents=True)
    records = []
    for note in NOTES:
        rel = f"t/n{note:03d}_v{VELOCITY:03d}.wav"
        audio = render(440.0 * 2.0 ** ((note - 69) / 12.0))
        write_wav(root / rel, audio, SR)
        frames = audio.shape[0] if audio.ndim > 1 else audio.size
        records.append({"timbre": "t", "note": note, "velocity": VELOCITY,
                        "path": rel, "seconds": frames / SR})
    (root / "manifest.json").write_text(json.dumps({
        "id": name, "sample_rate": SR, "gate_ms": gate_ms, "tail": "500ms",
        "preroll_ms": 0, "timbres": [{"id": "t", "label": name, "slot_channel": 1}],
        "renders": records,
    }))
    return load_corpus(root)


def test_a_cabinet_reads_as_a_steep_skirt_and_a_bare_pickup_does_not(tmp_path):
    bare = measure_skirt(_corpus(tmp_path, "bare", lambda f: _lowpass(_pluck(f), 9000.0, 1)))
    cabinet = measure_skirt(_corpus(tmp_path, "cab", lambda f: _lowpass(_pluck(f), 4500.0, 6)))
    assert cabinet.slope_db_per_octave < -30.0
    assert bare.slope_db_per_octave > cabinet.slope_db_per_octave + 15.0
    assert cabinet.knee_hz < 9000.0


def test_one_filter_over_several_instruments_reads_as_shared(tmp_path):
    """The evidence that a dark spectrum is a rig rather than a dull instrument."""

    def through_cabinet(detune):
        return lambda f: _lowpass(_pluck(f * detune), 4500.0, 6)

    first = measure_skirt(_corpus(tmp_path, "a", through_cabinet(1.0)))
    second = measure_skirt(_corpus(tmp_path, "b", through_cabinet(1.5)))
    unamped = measure_skirt(_corpus(tmp_path, "c", lambda f: _lowpass(_pluck(f), 3000.0, 1)))
    assert curve_distance(first, second) < curve_distance(first, unamped) / 3.0


def test_a_rotor_is_anti_phase_across_the_mics_and_a_beat_is_not(tmp_path):
    def modulated(anti: bool):
        def render(f):
            # The two mics hear a spread source, as a real stereo capture does —
            # without that the in-phase case is dual mono and there is nothing
            # to correlate either way.
            t = np.arange(int(SR * 3.0)) / SR
            left_tone = _pluck(f, 3.0) * 4.0
            right_tone = _pluck(f * 1.002, 3.0) * 4.0
            depth = 0.35
            phase = np.pi if anti else 0.0
            left = left_tone * (1.0 + depth * np.sin(2 * np.pi * 6.6 * t))
            right = right_tone * (1.0 + depth * np.sin(2 * np.pi * 6.6 * t + phase))
            return np.stack([left, right], axis=1)

        return render

    rotor = measure_rotary(_corpus(tmp_path, "rot", modulated(True), gate_ms=3000))
    beat = measure_rotary(_corpus(tmp_path, "beat", modulated(False), gate_ms=3000))
    assert rotor.stereo and beat.stereo
    assert rotor.interchannel_correlation < -0.5
    assert beat.interchannel_correlation > 0.5
    assert rotor.rate_hz == pytest.approx(6.6, abs=0.5)


def test_a_dual_mono_render_reports_the_question_as_unanswerable(tmp_path):
    """A vacuous negative is the failure that would put `none` into a capture."""

    def render(f):
        one = _pluck(f, 3.0)
        return np.stack([one, one], axis=1)

    rotary = measure_rotary(_corpus(tmp_path, "mono", render, gate_ms=3000))
    assert not rotary.stereo
    assert rotary.channel_separation_db < -40.0


def test_a_note_that_dies_inside_the_gate_is_not_read_as_modulation(tmp_path):
    """A short note under a long gate is mostly silence, and silence has a shape."""

    def render(f):
        one = np.concatenate([_pluck(f, 0.4), np.zeros(int(SR * 2.6))])
        return np.stack([one, one * 0.999], axis=1)

    rotary = measure_rotary(_corpus(tmp_path, "short", render, gate_ms=3000))
    assert rotary.depth_db < 6.0
