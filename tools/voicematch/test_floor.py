"""The recording floor, the valleys between partials, bed clearance and the body censor."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import metrics_floor
from metrics_floor import (
    AT_FLAT_FLOOR,
    AT_FLOOR_UNDER_MODULATION,
    BODY_FLOOR_MARGIN_DB,
    _partial_bands,
    body_at_floor,
    body_floor_state,
    partial_clearance,
    recording_floor,
    valley_profile,
)
from metrics_note import BODY_WINDOW_S, band_power_db, body_below_f0_db
from profile_measure import measure_note, partial_decay

SR = 44100


def _noise(seconds: float, sigma: float, seed: int = 1) -> np.ndarray:
    return np.random.default_rng(seed).normal(0.0, sigma, int(seconds * SR))


def _psd_band_db(sigma: float, lo: float, hi: float) -> float:
    """Band power of white noise of std `sigma`, from its analytic one-sided PSD."""
    return float(10.0 * np.log10(sigma**2 * (hi - lo) / (SR / 2.0)))


# --------------------------------------------------------------------------- #
# band power and the body readings
# --------------------------------------------------------------------------- #


def test_band_power_is_independent_of_segment_length():
    sigma = 0.05
    expected = _psd_band_db(sigma, 100.0, 4000.0)
    for seconds in (0.1, 0.5, 2.0):
        got = band_power_db(_noise(seconds, sigma), SR, 100.0, 4000.0)
        assert got is not None
        assert abs(got - expected) < 0.5, (seconds, got, expected)


def test_band_power_none_for_empty_or_silent_band():
    assert band_power_db(np.zeros(4096), SR, 100.0, 200.0) is None
    assert band_power_db(_noise(0.1, 0.1), SR, SR, SR + 100.0) is None


def _old_body_below_f0_db(x, sr, f0):
    if f0 < 120.0:
        return None
    a, b = int(BODY_WINDOW_S[0] * sr), int(BODY_WINDOW_S[1] * sr)
    seg = x[a:b]
    if seg.size < sr // 8:
        return None
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1.0 / sr)

    def band(lo, hi):
        sel = (freqs >= lo) & (freqs < hi)
        return float(np.sqrt(np.sum(spec[sel] ** 2))) if sel.any() else 0.0

    under, fundamental = band(40.0, f0 * 0.7), band(f0 * 0.8, f0 * 1.25)
    if fundamental <= 0.0 or under <= 0.0:
        return None
    return round(float(20.0 * np.log10(under / fundamental)), 2)


def test_body_below_f0_db_matches_the_original_formula():
    rng = np.random.default_rng(7)
    t = np.arange(int(1.5 * SR)) / SR
    for i in range(12):
        f0 = float(rng.uniform(100.0, 1200.0))
        x = rng.normal(0.0, 0.01, t.size) + rng.uniform(0.1, 1.0) * np.sin(2 * np.pi * f0 * t)
        got, want = body_below_f0_db(x, SR, f0), _old_body_below_f0_db(x, SR, f0)
        if want is None:
            assert got is None
        else:
            assert got is not None and abs(got - want) <= 0.01 + 1e-9, (i, got, want)
    assert body_below_f0_db(np.zeros(100), SR, 440.0) is None


# --------------------------------------------------------------------------- #
# the recording floor
# --------------------------------------------------------------------------- #


def _note(seconds: float, f0: float = 440.0, amp: float = 0.5, decay_db_s: float = 0.0):
    t = np.arange(int(seconds * SR)) / SR
    return amp * 10.0 ** (-decay_db_s * t / 20.0) * np.sin(2 * np.pi * f0 * t)


def test_preroll_floor_reads_the_injected_psd():
    sigma = 1e-3
    pre = 0.2
    mono = np.concatenate([_noise(pre, sigma), _note(1.0)])
    got = recording_floor(mono, SR, pre, -9.0, f0=440.0)
    assert got["floor_source"] == "preroll"
    assert abs(got["floor_db"] - _psd_band_db(sigma, 40.0, 16000.0)) < 1.0
    assert abs(got["floor_body_db"] - _psd_band_db(sigma, 40.0, 0.7 * 440.0)) < 1.0
    octaves = got["floor_octaves_db"]
    assert len(octaves) == 9 and all(v is not None for v in octaves)
    assert abs(octaves[4] - _psd_band_db(sigma, 1000 / 2**0.5, 1000 * 2**0.5)) < 1.5


def test_octave_bands_above_nyquist_are_none():
    sr = 16000
    mono = np.concatenate([np.random.default_rng(2).normal(0, 1e-3, int(0.2 * sr)), np.zeros(sr)])
    got = recording_floor(mono, sr, 0.2, -9.0)
    assert got["floor_octaves_db"][-1] is None
    assert "floor_body_db" not in got


def test_digital_silence_preroll_is_no_floor():
    mono = np.concatenate([np.zeros(int(0.2 * SR)), _note(1.0, decay_db_s=60.0)])
    assert recording_floor(mono, SR, 0.2, -9.0, f0=440.0) == {"floor_source": None}


def test_release_tail_is_not_a_floor():
    # A module capture: no preroll, and the file ends on the note's own tail 40 dB down.
    mono = np.concatenate([_note(1.0), 0.01 * _note(0.3)])
    assert recording_floor(mono, SR, 0.0, -9.0, f0=440.0) == {"floor_source": None}


def test_deep_tail_is_a_floor_when_there_is_no_preroll():
    sigma = 1e-5
    mono = np.concatenate([_note(1.0), _noise(0.3, sigma)])
    got = recording_floor(mono, SR, 0.0, -9.0, f0=440.0)
    assert got["floor_source"] == "tail"
    assert abs(got["floor_db"] - _psd_band_db(sigma, 40.0, 16000.0)) < 1.0


# --------------------------------------------------------------------------- #
# valleys
# --------------------------------------------------------------------------- #

F0 = 220.0


def _ladder(seed: int = 3, n: int = 12):
    rng = np.random.default_rng(seed)
    levels = -2.0 * np.arange(n) + rng.uniform(-8.0, 8.0, n)
    return F0 * np.arange(1, n + 1), levels


def _tones(freqs, levels, seconds: float = 1.5, amp: float = 0.1) -> np.ndarray:
    t = np.arange(int(seconds * SR)) / SR
    x = np.zeros(t.size)
    for f, db in zip(freqs, levels):
        x += amp * 10.0 ** (db / 20.0) * np.sin(2 * np.pi * f * t)
    return x


def test_flat_noise_does_not_follow_the_ladder():
    freqs, levels = _ladder()
    x = _tones(freqs, levels) + _noise(1.5, 1e-4)
    got = valley_profile(x, SR, list(freqs), list(levels))
    assert len(got["valley_db"]) == 11
    assert abs(got["valley_ladder_coupling"]) < 0.3
    assert abs(got["valley_slope_db_oct"]) < 3.0


def test_noise_shaped_by_the_ladder_couples_to_it():
    freqs, levels = _ladder()
    n = int(1.5 * SR)
    spec = np.fft.rfft(_noise(1.5, 1.0, seed=5))
    f = np.fft.rfftfreq(n, 1.0 / SR)
    env_db = np.interp(f, freqs, levels)
    shaped = np.fft.irfft(spec * 10.0 ** (env_db / 20.0), n) * 1e-3
    x = _tones(freqs, levels) + shaped
    got = valley_profile(x, SR, list(freqs), list(levels))
    assert got["valley_ladder_coupling"] > 0.7


def test_too_few_valleys_write_nothing():
    freqs, levels = _ladder(n=4)
    x = _tones(freqs, levels) + _noise(1.5, 1e-4)
    assert valley_profile(x, SR, list(freqs), list(levels)) == {}


# --------------------------------------------------------------------------- #
# the body censor
# --------------------------------------------------------------------------- #


def _ref(**kw):
    row = {
        "floor_source": "preroll",
        "body_under_db": -80.0,
        "floor_body_db": -82.0,
        "valley_ladder_coupling": 0.1,
    }
    row.update(kw)
    return row


def test_body_censor_cases():
    assert body_at_floor(_ref())
    assert not body_at_floor(_ref(valley_ladder_coupling=0.9))
    assert not body_at_floor(_ref(floor_source=None))
    assert not body_at_floor(_ref(body_under_db=-82.0 + BODY_FLOOR_MARGIN_DB + 4.0))
    assert not body_at_floor({})
    assert not body_at_floor(_ref(valley_ladder_coupling=None))


def test_a_modulated_row_is_censored_on_the_floor_alone():
    """Vibrato fills the valleys toward flat, so under it only the floor decides."""
    fm = {"tnr_mask": {"fm": [12.8, 4.3, 5.7]}}
    # At the floor: censored whatever the valleys say, with the reason named.
    assert body_floor_state(_ref(valley_ladder_coupling=0.9, **fm)) == AT_FLOOR_UNDER_MODULATION
    assert body_floor_state(_ref(**fm)) == AT_FLOOR_UNDER_MODULATION
    assert body_floor_state(_ref()) == AT_FLAT_FLOOR
    # Well above the floor: never censored, however flat vibrato makes the valleys.
    assert body_floor_state(_ref(body_under_db=-50.0, valley_ladder_coupling=-1.3, **fm)) is None


def test_vibrato_made_flatness_does_not_censor_a_band_above_its_floor():
    """C3 (the body reading starts at 120 Hz) at 50 cents of vibrato over -30 dB noise: not censored."""
    sr, pre, gate, note = 48000, 0.1, 2.0, 48
    f0 = 440.0 * 2 ** ((note - 69) / 12)
    t = np.arange(int((pre + gate + 0.3) * sr)) / sr
    rate, cents = 5.5, 50.0
    # The phase term whose derivative swings f0 by +/- `cents` (peak).
    dev = f0 * (2 ** (cents / 1200) - 1) / (2 * np.pi * rate) * np.sin(2 * np.pi * rate * t)
    tone = sum(np.sin(2 * np.pi * k * (f0 * t + dev)) / k for k in range(1, 41))
    rng = np.random.default_rng(3)
    held = (t >= pre) & (t < pre + gate)
    x = 0.05 * tone * held + 0.05 * 10 ** (-30 / 20) * rng.standard_normal(t.size) * held
    x = x + 10 ** (-110 / 20) * rng.standard_normal(t.size)
    row = measure_note(x.astype(np.float32), sr, note, preroll_s=pre, gate_s=gate)
    # The control's own depth, measured: about 100 cents peak to peak.
    assert row["tnr_mask"]["fm"][0] == pytest.approx(2 * cents, rel=0.15)
    assert row.get("floor_source") == "preroll"
    assert row["body_under_db"] > row["floor_body_db"] + BODY_FLOOR_MARGIN_DB
    assert body_floor_state(row) is None


# --------------------------------------------------------------------------- #
# bed clearance
# --------------------------------------------------------------------------- #

RATE = 100.0


def _decaying_into_floor():
    win = max(1024, int(0.093 * SR))
    f0 = 20.0 * SR / win
    t = np.arange(int(1.5 * SR)) / SR
    tone = 0.5 * 10.0 ** (-RATE * t / 20.0) * np.sin(2 * np.pi * 2 * f0 * t)
    others = sum(0.05 * np.sin(2 * np.pi * k * f0 * t) for k in (1, 3))
    # Periodic at the analysis hop, so every frame sees the same flat line spectrum.
    hop = win // 2
    ph = np.random.default_rng(11).uniform(0.0, 2 * np.pi, hop // 2 + 1)
    period = np.fft.irfft(np.exp(1j * ph), hop)
    period *= 0.013 / period.std()
    floor = np.tile(period, t.size // hop + 1)[: t.size]
    return tone + others + floor, floor, [f0, 2 * f0, 3 * f0], win


def test_partial_clear_s_lands_on_the_analytic_crossing():
    x, floor, partials, win = _decaying_into_floor()
    hop_s = (win // 2) / SR
    got = partial_clearance(x, SR, partials)
    clear = got["partial_clear_s"][1]
    # Bed level as the same band filter reads it on the floor alone, at the valley.
    _, bands = _partial_bands(floor, SR, [2.5 * partials[0]])
    bed_db = float(np.median(20.0 * np.log10(bands[0])))
    tone0_db = 20.0 * np.log10(0.5 * win / 4.0)
    crossing = (tone0_db - bed_db - metrics_floor.BED_CLEAR_DB) / RATE - win / (2.0 * SR)
    assert clear is not None and abs(clear - crossing) <= hop_s, (clear, crossing)


def test_clear_restricted_decay_recovers_the_rate():
    x, _floor, partials, _win = _decaying_into_floor()
    got = partial_clearance(x, SR, partials)
    restricted = got["partial_decay_clear_db_s"][1]
    plain = partial_decay(x, SR, partials)[1]
    assert restricted is not None
    assert abs(restricted + RATE) <= 0.1 * RATE, restricted
    # The floor flattens the unrestricted fit; it is allowed to be biased, not better.
    assert abs(restricted + RATE) <= abs(plain + RATE)


def test_clear_fit_is_partial_decay_when_nothing_is_floor():
    win = max(1024, int(0.093 * SR))
    f0 = 20.0 * SR / win
    t = np.arange(int(1.5 * SR)) / SR
    x = sum(
        0.2 * 10.0 ** (-20.0 * t / 20.0) * np.sin(2 * np.pi * k * f0 * t) for k in (1, 2, 3)
    ) + _noise(1.5, 1e-7)
    got = partial_clearance(x, SR, [f0, 2 * f0, 3 * f0])
    want = partial_decay(x, SR, [f0, 2 * f0, 3 * f0])
    assert got["partial_decay_clear_db_s"] == want[:3]


def test_never_clear_partial_is_none():
    win = max(1024, int(0.093 * SR))
    f0 = 20.0 * SR / win
    t = np.arange(int(1.5 * SR)) / SR
    # A louder tone in partial 2's valley: nothing stands 8 dB over it.
    x = sum(0.1 * np.sin(2 * np.pi * k * f0 * t) for k in (1, 2, 3))
    x = x + 0.5 * np.sin(2 * np.pi * 2.5 * f0 * t) + _noise(1.5, 1e-6)
    got = partial_clearance(x, SR, [f0, 2 * f0, 3 * f0])
    assert got["partial_clear_s"][1] is None
    assert got["partial_decay_clear_db_s"][1] is None
