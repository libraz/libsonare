"""Tests for the plugin-capture path: the oracle bridge and the measurements.

Nothing here renders anything or needs a plugin. The measurements are checked
by round trip — a signal is synthesised with a known inharmonicity, a known
double decay or a known damper release, and the measurement has to recover the
number it was built from. A measurement checked against its own output on real
audio would agree with whatever it happens to compute.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import au_oracle  # noqa: E402
import capture  # noqa: E402
from au_oracle import AuSource, _strip_preroll  # noqa: E402
from metrics import midi_to_hz  # noqa: E402
from profile import double_decay, find_partials, measure_note  # noqa: E402
from wavio import write_wav  # noqa: E402

SR = 48000


def stiff_string(note: int, b: float, sr: int = SR, seconds: float = 1.5,
                 n_partials: int = 14, decay_per_partial: float = 1.6) -> np.ndarray:
    """A struck stiff string: partials at n*f0*sqrt(1 + B n^2), each decaying.

    The higher partials decay faster, as they do on a real string, so the test
    signal is not one the fit could pass by ignoring time.
    """
    f0 = midi_to_hz(note)
    t = np.arange(int(seconds * sr)) / sr
    out = np.zeros_like(t)
    for n in range(1, n_partials + 1):
        fn = n * f0 * np.sqrt(1.0 + b * n * n)
        if fn > 0.45 * sr:
            break
        out += (1.0 / n) * np.exp(-decay_per_partial * n * t) * np.sin(2 * np.pi * fn * t)
    return (out / np.abs(out).max()).astype(np.float32)


# --------------------------------------------------------------------------
# the measurements


@pytest.mark.parametrize("note,b", [(36, 8e-5), (48, 1.2e-4), (60, 3.0e-4), (72, 9.0e-4)])
def test_inharmonicity_round_trip(note, b):
    """B is recovered from a signal built with it, across the piano's range."""
    got = find_partials(stiff_string(note, b), SR, note)
    assert got, "no partials found in a synthetic string"
    assert got["inharmonicity_b"] == pytest.approx(b, rel=0.25)


@pytest.mark.parametrize("note,b", [(36, 8e-5), (60, 3.0e-4)])
def test_f0_recovered_despite_stiffness(note, b):
    """The fitted f0 is the string's, not the frequency of its first partial.

    Partial 1 of a stiff string sits above f0 by sqrt(1 + B), so a measurement
    that reads it directly reports the note as sharp — which would then be
    written into a stretch curve as a property of the tuning.
    """
    got = find_partials(stiff_string(note, b), SR, note)
    assert abs(got["cents_vs_et"]) < 3.0


def test_inharmonicity_zero_for_a_harmonic_series():
    """A harmonic series must not be reported as a stiff string."""
    f0 = midi_to_hz(60)
    t = np.arange(int(1.5 * SR)) / SR
    y = sum((1.0 / n) * np.exp(-1.6 * n * t) * np.sin(2 * np.pi * n * f0 * t)
            for n in range(1, 15)).astype(np.float32)
    got = find_partials(y / np.abs(y).max(), SR, 60)
    assert got["inharmonicity_b"] < 2e-5


def test_double_decay_finds_the_knee():
    """Two slopes joined at a knee are recovered as two slopes and a knee."""
    t = np.linspace(0.0, 6.0, 600)
    env_db = np.where(t < 2.0, -12.0 * t, -24.0 - 3.0 * (t - 2.0))
    got = double_decay(env_db, t)
    assert got["decay_early_db_s"] == pytest.approx(-12.0, abs=1.0)
    assert got["decay_late_db_s"] == pytest.approx(-3.0, abs=1.0)
    assert got["decay_knee_s"] == pytest.approx(2.0, abs=0.3)


def test_damper_release_is_measured_from_note_off():
    """The release window starts at note-off, not at the end of the file."""
    preroll, gate, tail = 0.1, 1.0, 1.0
    t = np.arange(int((preroll + gate + tail) * SR)) / SR
    tone = np.sin(2 * np.pi * midi_to_hz(60) * t)
    env = np.ones_like(t)
    env[t < preroll] = 0.0
    off = preroll + gate
    # 40 dB down 150 ms after note-off.
    env[t >= off] = 10.0 ** (-40.0 / 20.0 * (t[t >= off] - off) / 0.150)
    got = measure_note((tone * env).astype(np.float32), SR, 60,
                       preroll_s=preroll, gate_s=gate)
    assert got["damper_release_ms"] == pytest.approx(150.0, abs=40.0)
    assert got["damper_capped"] is False


def test_measure_note_returns_nothing_for_silence():
    silence = np.zeros(int(2.0 * SR), dtype=np.float32)
    assert measure_note(silence, SR, 60, preroll_s=0.1, gate_s=1.0) == {}


# --------------------------------------------------------------------------
# the oracle bridge


def test_preroll_is_stripped_exactly():
    """The host writes a known preroll, so the alignment is arithmetic."""
    audio = np.arange(SR, dtype=np.float32)[:, None]
    assert _strip_preroll(audio, 100, SR).shape[0] == SR - int(0.1 * SR)


def test_preroll_longer_than_the_render_is_empty_not_negative():
    audio = np.zeros((10, 2), dtype=np.float32)
    assert _strip_preroll(audio, 100, SR).shape[0] == 0


def _identity(**kw) -> str:
    return json.dumps(AuSource(plugin="x:y:z", **kw).identity(), sort_keys=True)


def test_every_host_setting_is_part_of_the_cache_key():
    """A changed setting is a different recording, never a stale hit.

    The settle time and the real-time flag are in here because they are the two
    that decide whether the render contains the instrument at all.
    """
    base = _identity()
    for kw in ({"settle_ms": 8000}, {"realtime": False}, {"params": ("Reverb On/Off=0",)},
               {"preroll_ms": 250}, {"tail": "6s"}, {"sample_rate": 44100}, {"program": 3}):
        assert _identity(**kw) != base, f"{kw} does not reach the cache key"


def test_resolve_preset_takes_an_existing_path_as_given(tmp_path):
    p = tmp_path / "some.vstpreset"
    p.write_bytes(b"")
    assert au_oracle.resolve_preset(str(p)) == p


def test_resolve_preset_refuses_an_ambiguous_fragment(tmp_path, monkeypatch):
    for name in ("Close/Take.vstpreset", "Player/Take.vstpreset"):
        f = tmp_path / name
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"")
    monkeypatch.setattr(au_oracle, "PRESET_ROOTS", (tmp_path,))
    with pytest.raises(ValueError, match="matches 2 files"):
        au_oracle.resolve_preset("take")
    with pytest.raises(FileNotFoundError):
        au_oracle.resolve_preset("nothing-by-this-name")


def test_argv_carries_realtime_and_settle(monkeypatch):
    """The two settings a silent-but-plausible render depends on reach the host."""
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    argv = AuSource(plugin="x:y:z", settle_ms=4000, realtime=True).argv(Path("/tmp/o.wav"))
    assert "--realtime" in argv
    assert argv[argv.index("--settle-ms") + 1] == "4000"
    assert AuSource(plugin="x:y:z", realtime=False).argv(Path("/tmp/o.wav")).count("--realtime") == 0


# --------------------------------------------------------------------------- #
# A render that arrived late is not the note
# --------------------------------------------------------------------------- #
def _render_file(path: Path, onset_ms: float, *, peak: float = 0.4,
                 seconds: float = 2.0, sr: int = SR) -> Path:
    """A capture-shaped WAV: digital silence, then a decaying burst."""
    n = int(seconds * sr)
    audio = np.zeros((n, 2), dtype=np.float32)
    start = int(onset_ms / 1000.0 * sr)
    t = np.arange(n - start) / sr
    body = (peak * np.sin(2 * np.pi * 300.0 * t) * np.exp(-t / 0.1)).astype(np.float32)
    audio[start:, 0] = body
    audio[start:, 1] = body
    write_wav(path, audio, sr)
    return path


def test_a_render_is_timed_from_where_its_audio_actually_begins(tmp_path):
    assert capture._onset_ms(_render_file(tmp_path / "a.wav", 100.0), SR) == pytest.approx(100.0, abs=0.5)
    assert capture._onset_ms(_render_file(tmp_path / "b.wav", 280.0), SR) == pytest.approx(280.0, abs=0.5)
    # Silence has no onset, and says so rather than answering zero — which is
    # the one answer that would read as a render arriving perfectly on time.
    write_wav(tmp_path / "c.wav", np.zeros((SR, 2), dtype=np.float32), SR)
    assert capture._onset_ms(tmp_path / "c.wav", SR) is None


def test_the_onset_test_reads_the_first_sample_over_a_floor_not_the_peak():
    """A slow-attack instrument must not be mistaken for a late render.

    The threshold is absolute and far under anything an instrument radiates, so
    what it finds is where the render stopped being digital silence — which is
    the note-on however long the swell after it takes.
    """
    assert capture.ONSET_FLOOR_DBFS <= -60.0
    # Wide enough that a real preroll's jitter never trips it, narrow enough
    # that the failure it was written for — 150 ms at the very least, and up to
    # 843 — cannot get through.
    assert 10.0 <= capture.ONSET_SLACK_MS <= 100.0


def test_a_late_render_is_retried_rather_than_recorded(tmp_path, monkeypatch):
    """The failure the quiet-retry is structurally unable to see.

    A render whose samples did not arrive is quiet, and that is what the ratio
    catches. This one is LOUDER than the note — 4 to 25 dB, uncorrelated with
    the correct render, beginning after a stretch of digital silence — so every
    level test passes it. Only when it arrived says anything.
    """
    out = tmp_path / "n042_v127.wav"
    calls = []

    def fake_run(argv, **kwargs):
        calls.append(len(calls))
        # Late and loud the first two times, then the real note.
        _render_file(out, 280.0 if len(calls) <= 2 else 100.0,
                     peak=0.9 if len(calls) <= 2 else 0.1)
        return SimpleNamespace(
            returncode=0, stdout=json.dumps({"peak": 0.9 if len(calls) <= 2 else 0.1,
                                             "seconds": 2.0}), stderr="")

    monkeypatch.setattr(capture.subprocess, "run", fake_run)
    src = AuSource(plugin="aumu:test:test")
    summary = capture._render_note(src, out, 42, 127, 50, floor_peak=0.0,
                                   preroll_ms=100.0, sample_rate=SR)
    assert len(calls) == 3
    assert summary["attempts"] == 3
    assert summary["onset_ms"] == pytest.approx(100.0, abs=1.0)


def test_a_render_on_time_is_taken_first_try(tmp_path, monkeypatch):
    """The null: without it the guard could be rejecting everything."""
    out = tmp_path / "n038_v100.wav"
    calls = []

    def fake_run(argv, **kwargs):
        calls.append(len(calls))
        _render_file(out, 100.0)
        return SimpleNamespace(returncode=0,
                               stdout=json.dumps({"peak": 0.4, "seconds": 2.0}), stderr="")

    monkeypatch.setattr(capture.subprocess, "run", fake_run)
    summary = capture._render_note(AuSource(plugin="aumu:test:test"), out, 38, 100, 50,
                                   floor_peak=0.0, preroll_ms=100.0, sample_rate=SR)
    assert len(calls) == 1
    assert summary["attempts"] == 1
