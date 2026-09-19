"""The shared onset detector.

`sound_onset_s` places the analysis window for all three metric sets — the
percussion hit, the live probe's per-note anchor and the profile's captured
note — so a mistake here is not one instrument's: it moves the window every
timing measurement in the bank is read through.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from metrics_signal import sound_onset_s

SR = 48000
PREROLL_S = 0.1


def _tone(seconds: float, hz: float = 440.0, level: float = 1.0) -> np.ndarray:
    t = np.arange(int(seconds * SR)) / SR
    return (np.sin(2 * np.pi * hz * t) * level).astype(np.float64)


def _with_preroll(body: np.ndarray) -> np.ndarray:
    return np.concatenate([np.zeros(int(PREROLL_S * SR)), body])


def test_a_swelling_voice_is_not_re_onset_by_a_notch_in_its_own_envelope():
    """One frame under the floor is a ripple, not a beginning.

    Measured on the cached corpus, `lead_square` sounds from the end of its
    preroll at a sixth of its eventual peak and crests 1094 ms in; three frames
    around 730 ms graze 50 dB under that crest, which a rule reading the LAST
    sub-floor frame before the peak takes as the onset. The window then opens
    630 ms inside a note that had been sounding the whole time.
    """
    body = _tone(1.4)
    # A swell to the crest, so the floor is 50 dB under something far later.
    body *= np.linspace(0.16, 1.0, len(body))
    # Wide enough that one RMS window lies wholly inside it, which is what it
    # takes to reach the floor: the three frames `lead_square` grazes it on
    # span 1.5 ms, and a 2 ms window cannot read 54 dB down on less.
    notch = int(0.6 * SR)
    body[notch:notch + int(0.0025 * SR)] = 0.0

    got = sound_onset_s(_with_preroll(body), SR, 0.0, 1.5, search_s=1.2)

    assert got == pytest.approx(PREROLL_S, abs=0.003)


def test_a_voice_with_a_gap_reports_its_first_sound_and_not_its_loudest():
    """A silence between two events is not the render's lead-in.

    `bird_tweet` is the shape: two chirps with 2 ms of -160 dB between them and
    the second one the louder. Reading the last sub-floor frame before the peak
    returns the head of the loudest chirp — 755 ms past the note-on, and the
    window then misses the first chirp entirely.
    """
    body = np.concatenate([
        _tone(0.2, level=0.01),          # first chirp, 40 dB under the second
        np.zeros(int(0.5 * SR)),         # the gap
        _tone(0.2, level=1.0),           # the loudest chirp
    ])

    got = sound_onset_s(_with_preroll(body), SR, 0.0, 1.5, search_s=1.2)

    assert got == pytest.approx(PREROLL_S, abs=0.003)


def test_a_waveform_trough_is_not_a_gap_in_the_sound():
    """A 2 ms window cannot smooth a 15 ms period, and must not be asked to.

    `lead_square`'s bottom octave is the case: a 65 Hz pulse read through the
    envelope grid swings between -9 and -64 dB of its own peak every period, so
    its first unbroken 5 ms over the floor is 38 ms after it began sounding —
    and only once the voice has grown enough for its troughs to clear a floor
    set by a crest a second later. The control is the same call with the gap
    rule switched off, which is what a hold alone does.
    """
    n = int(1.2 * SR)
    t = np.arange(n) / SR
    pulse = np.zeros(n)
    period = int(SR / 65.0)
    for k in range(0, n, period):
        pulse[k:k + int(0.001 * SR)] = 0.25
    # A continuous component that starts under the floor and grows over it, so
    # a hold alone finds its first unbroken stretch well inside the note.
    body = pulse + np.sin(2 * np.pi * 300.0 * t) * np.linspace(1e-4, 0.02, n)

    got = sound_onset_s(_with_preroll(body), SR, 0.0, 1.4, search_s=1.3)
    hold_only = sound_onset_s(_with_preroll(body), SR, 0.0, 1.4, search_s=1.3, gap_ms=0.0)

    assert hold_only > PREROLL_S + 0.020
    assert got == pytest.approx(PREROLL_S, abs=0.003)


def test_a_swell_keeps_its_own_beginning_rather_than_being_cut_to_its_peak():
    """The property the percussion path was written for, kept under the new rule.

    A crash and a vibraslap reach their loudest hundreds of milliseconds after
    the strike. The detector has to find the strike, not the crest — the same
    demand `test_a_hit_that_swells_keeps_its_onset_rather_than_being_cut_to_its_peak`
    makes of `measure_hit`, asked of the primitive underneath it.
    """
    n = int(0.9 * SR)
    swell = np.random.default_rng(1).normal(0, 0.2, n)
    swell *= np.minimum(1.0, np.arange(n) / (0.3 * SR)) * np.exp(-np.arange(n) / (0.6 * SR))

    got = sound_onset_s(_with_preroll(swell), SR, 0.0, 1.1)

    assert got == pytest.approx(PREROLL_S, abs=0.006)


def test_a_sound_too_short_to_hold_is_still_found():
    """A hold requirement must not refuse the shortest thing in the bank.

    A woodblock at the top of its range is over in 17 ms and a single-sample
    impulse leaves one RMS window's worth of envelope. Neither can satisfy a
    hold, and returning the start of the file for them would put the window a
    preroll early rather than a ripple late.
    """
    body = np.zeros(int(0.5 * SR))
    body[: int(0.002 * SR)] = _tone(0.002)

    got = sound_onset_s(_with_preroll(body), SR, 0.0, 0.6)

    assert got == pytest.approx(PREROLL_S, abs=0.003)


def test_a_render_already_sounding_at_the_window_start_reports_the_start():
    """Nothing to trim: the answer is the window's own origin, not its first frame."""
    got = sound_onset_s(_tone(0.5), SR, 0.0, 0.5)
    assert got == 0.0


def test_a_silent_render_falls_back_to_the_start_it_was_given():
    assert sound_onset_s(np.zeros(int(0.5 * SR)), SR, 0.25, 0.5) == 0.25
