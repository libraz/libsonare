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
from metrics import harmonic_share, midi_to_hz  # noqa: E402
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


def test_a_slot_is_asked_for_by_channel_only_when_the_notes_are_ours(monkeypatch):
    """A score carries its own channels, so the flag would be refused there.

    The slot then has to be selected by what the score is written on. Sending
    the flag anyway is not a no-op: aubounce refuses the pair, so a rack's every
    reference render would fail — and before it refused, the flag was dropped
    and every slot of the rack rendered as whichever one the score's channel
    happened to select.
    """
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    source = AuSource(plugin="x:y:z", channel=2)
    assert source.argv(Path("/tmp/o.wav")).count("--channel") == 1
    assert source.argv(Path("/tmp/o.wav"), midi=Path("/tmp/probe.mid")).count("--channel") == 0


def test_a_capture_declaring_no_sends_renders_exactly_as_it_always_did(monkeypatch, tmp_path):
    """The opt-in has to leave every reference measured before it existed alone.

    Those files are ground truth rather than something regenerable, so a change
    that quietly moved the render path would redefine what they contain without
    touching them.
    """
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    source = AuSource(plugin="x:y:z", channel=3)
    argv = capture._note_argv(source, tmp_path / "o.wav", 60, 100, 2000)
    assert "--midi" not in argv
    assert argv[argv.index("--note") + 1] == "60"
    assert argv[argv.index("--channel") + 1] == "3"


def test_a_capture_declaring_sends_carries_them_and_its_slot_in_the_score(monkeypatch, tmp_path):
    """A plugin that advertises no effect parameter can still be dried by CC.

    The score is the only path that reaches a controller, and it is also the
    only one that can carry the slot, since aubounce refuses `--channel` beside
    `--midi`.
    """
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    source = AuSource(plugin="x:y:z", channel=3)
    argv = capture._note_argv(source, tmp_path / "o.wav", 60, 100, 2000, sends=(0, 0, 0))
    assert "--note" not in argv
    assert "--channel" not in argv
    score = Path(argv[argv.index("--midi") + 1]).read_bytes()
    # Status nibble 0xB is control change; the slot is channel 3, so 0xB2.
    assert bytes([0xB2, 91, 0]) in score
    assert bytes([0x92, 60, 100]) in score


def test_sends_must_name_all_three_controllers():
    with pytest.raises(ValueError):
        capture.config_sends({"id": "x", "sends": [0, 0]})
    assert capture.config_sends({"id": "x"}) is None
    assert capture.config_sends({"id": "x", "sends": [0, 0, 0]}) == (0, 0, 0)


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
    # The argv is built but never executed, so the binary only has to be
    # nameable: without this the case passes on a checkout with an aubounce
    # sibling beside it and fails everywhere else.
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
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
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    summary = capture._render_note(AuSource(plugin="aumu:test:test"), out, 38, 100, 50,
                                   floor_peak=0.0, preroll_ms=100.0, sample_rate=SR)
    assert len(calls) == 1
    assert summary["attempts"] == 1


def _body_file(path: Path, body: np.ndarray, *, onset_ms: float = 100.0, sr: int = SR) -> Path:
    """A capture-shaped WAV: digital silence, then `body`."""
    start = int(onset_ms / 1000.0 * sr)
    audio = np.zeros((start + len(body), 2), dtype=np.float32)
    audio[start:, 0] = body
    audio[start:, 1] = body
    write_wav(path, audio, sr)
    return path


def test_a_quiet_render_that_carries_the_note_is_kept(tmp_path, monkeypatch):
    """A level test alone cuts into instruments louder-ranged than the piano.

    The floor is a ratio measured from a piano's 24 dB of velocity range. A
    clarinet's is 49, so its softest layer sits under the floor at every note
    from 62 up, and seven legitimate cells were rejected five attempts each at a
    stable value. What separates them is not level: this render is 40 dB below
    its own note's loudest and every bit of its energy is on that note's series.
    """
    out = tmp_path / "n086_v032.wav"
    calls = []

    def fake_run(argv, **kwargs):
        calls.append(len(calls))
        _body_file(out, _harmonic(midi_to_hz(86), seconds=1.0) * 0.0024)
        return SimpleNamespace(
            returncode=0, stdout=json.dumps({"peak": 0.0024, "seconds": 1.1}), stderr="")

    monkeypatch.setattr(capture.subprocess, "run", fake_run)
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    summary = capture._render_note(AuSource(plugin="aumu:test:test"), out, 86, 32, 50,
                                   floor_peak=0.237, preroll_ms=100.0, sample_rate=SR)
    assert len(calls) == 1
    assert summary["attempts"] == 1
    assert summary["quiet_tone_share"] >= capture.QUIET_TONE_SHARE


def test_a_quiet_render_with_no_note_in_it_still_fails(tmp_path, monkeypatch):
    """The positive control: the rescue above must still be able to reject.

    Same level as the case above and the same floor, differing only in carrying
    no series — which is what a render whose samples never arrived looks like.
    Without this the rescue would be indistinguishable from deleting the guard.
    """
    out = tmp_path / "n086_v032.wav"
    calls = []
    noise = (np.random.default_rng(11).standard_normal(SR) * 0.0008).astype(np.float32)

    def fake_run(argv, **kwargs):
        calls.append(len(calls))
        _body_file(out, noise)
        return SimpleNamespace(
            returncode=0, stdout=json.dumps({"peak": 0.0024, "seconds": 1.1}), stderr="")

    monkeypatch.setattr(capture.subprocess, "run", fake_run)
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    with pytest.raises(capture.AuRenderError, match="the samples did not arrive"):
        capture._render_note(AuSource(plugin="aumu:test:test"), out, 86, 32, 50,
                             floor_peak=0.237, preroll_ms=100.0, sample_rate=SR)
    assert len(calls) == 5


def test_a_silent_render_is_a_failure_rather_than_an_unmeasurable_share(tmp_path, monkeypatch):
    """`harmonic_share` reports None on silence, and None must not read as pass.

    Silence is the original failure this guard was written for, so the one
    answer it may never give is the benefit of the doubt.
    """
    out = tmp_path / "n086_v032.wav"

    def fake_run(argv, **kwargs):
        write_wav(out, np.zeros((SR, 2), dtype=np.float32), SR)
        return SimpleNamespace(
            returncode=0, stdout=json.dumps({"peak": 0.0, "seconds": 1.0}), stderr="")

    monkeypatch.setattr(capture.subprocess, "run", fake_run)
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    with pytest.raises(capture.AuRenderError, match="no tone at all"):
        capture._render_note(AuSource(plugin="aumu:test:test"), out, 86, 32, 50,
                             floor_peak=0.237, preroll_ms=100.0, sample_rate=SR)


def _calibration_cfg(tmp_path) -> Path:
    """A capture definition thin enough for `calibrate` and complete enough to load."""
    path = tmp_path / "probe.json"
    path.write_text(json.dumps({
        "id": "probe", "plugin": "aumu:test:test", "program": 0, "dry": False,
        "sample_rate": SR, "settle_ms": 8000, "realtime": True, "preroll_ms": 100,
        "gate_ms": 2000, "tail": "500ms", "notes": [60], "velocities": [100],
        "timbres": [{"id": "one", "channel": 1}],
    }))
    return path


def _settling_plugin(out_root: Path, *, needs_ms: int, weak: float = 0.12,
                     late_ms: float = 2800.0):
    """A fake host whose plugin is only intact once it has had `needs_ms` to load.

    Under that it renders the failure this rack really produced: the note arrives
    late and far below the level it reaches once settled, but nowhere near silent.
    """
    def fake_run(argv, **kwargs):
        wav = Path(argv[argv.index("-o") + 1])
        settle = int(argv[argv.index("--settle-ms") + 1])
        intact = settle >= needs_ms
        peak = 0.1733 if intact else 0.1733 * weak
        onset = 100.0 if intact else late_ms
        _body_file(wav, _harmonic(midi_to_hz(60), seconds=1.2) * peak, onset_ms=onset)
        return SimpleNamespace(
            returncode=0, stdout=json.dumps({"peak": peak, "seconds": 2.6, "dropout_ms": 0}),
            stderr="")
    return fake_run


def test_calibrate_will_not_recommend_a_settle_that_degrades_the_render(tmp_path, monkeypatch):
    """The failure this guard exists for, and the one it used to recommend.

    An under-settled render is weak rather than silent, so a floor set to catch
    silence passes it: 0.0209 against a settled 0.1733 is 18 dB down and still
    three times the floor. Recommending that settle sets every grid built after
    it on broken audio.
    """
    cfg = _calibration_cfg(tmp_path)
    monkeypatch.setattr(capture.subprocess, "run", _settling_plugin(tmp_path, needs_ms=8000))
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    report = capture.calibrate(capture.load_config(cfg), tmp_path / "out",
                               note=60, velocity=100, verbose=False)
    assert report["settle_min_ms"] >= 8000
    assert report["settle_recommended_ms"] >= 8000
    quiet = [r for r in report["settle"] if r["settle_ms"] < 8000]
    assert quiet, "the bisection has to have tried a settle under the plugin's need"
    assert not any(r["ok"] for r in quiet)


def test_calibrate_still_accepts_a_plugin_that_loads_at_once(tmp_path, monkeypatch):
    """The null: without it the comparison could be rejecting every settle.

    A plugin intact from the first probe must still bisect down to a small
    minimum, or the check has replaced one wrong answer with another.
    """
    cfg = _calibration_cfg(tmp_path)
    monkeypatch.setattr(capture.subprocess, "run", _settling_plugin(tmp_path, needs_ms=0))
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    report = capture.calibrate(capture.load_config(cfg), tmp_path / "out",
                               note=60, velocity=100, verbose=False)
    assert report["settle_min_ms"] <= 500
    assert all(r["ok"] for r in report["settle"])


def test_calibrate_rejects_a_settle_whose_render_is_merely_late(tmp_path, monkeypatch):
    """Level and timing are separate failures, and only one is a level.

    A render that reaches the right peak but starts seconds in is the note the
    plugin owed the previous probe, and the settle that produced it is not one
    to recommend.
    """
    cfg = _calibration_cfg(tmp_path)
    monkeypatch.setattr(capture.subprocess, "run",
                        _settling_plugin(tmp_path, needs_ms=8000, weak=1.0, late_ms=2800.0))
    monkeypatch.setattr(au_oracle, "find_aubounce", lambda: Path("/bin/true"))
    report = capture.calibrate(capture.load_config(cfg), tmp_path / "out",
                               note=60, velocity=100, verbose=False)
    assert report["settle_min_ms"] >= 8000
    late = [r for r in report["settle"] if r["settle_ms"] < 8000]
    assert late and not any(r["ok"] for r in late)
    assert all(r["onset_ms"] and r["onset_ms"] > 1000 for r in late)


def test_the_settle_peak_ratio_sits_between_the_two_measured_readings():
    """Both ends are a measurement, so the constant is not free to drift.

    The rack that forced this reads 0.0209 under-settled against 0.1733 settled,
    a ratio of 0.12, and reads the settled figure repeatably once it is there.
    Under that ratio the check admits the broken render; at 1.0 it rejects any
    plugin that is not bit-repeatable across settle times.
    """
    assert 0.12 < capture.SETTLE_PEAK_RATIO < 1.0


def test_the_quiet_rescue_sits_between_the_two_measured_populations():
    """Both bounds are a measurement, so neither is free to drift.

    A real note reads 0.9996 to 1.0000 across six slots of one rack and 40 dB of
    level; broadband noise reads 0.43 at both -42 and -60 dBFS, which is the
    fraction a flat spectrum lands inside the partial bands by construction. Set
    under the noise figure this admits failed loads, and up at the real one it
    rejects any instrument noisier than the ones it was measured on.
    """
    assert 0.5 < capture.QUIET_TONE_SHARE < 0.99


# --------------------------------------------------------------------------
# the per-note tail, and the sides that have to render over it


def test_a_named_note_carries_its_own_tail_and_the_rest_keep_the_flat_one():
    """`tail_by_note` is read as ranges and as single notes, both inclusive."""
    cfg = {"tail": "2s", "tail_by_note": {"49-59": "8s", "80-81": "6s", "84": "10s"}}
    assert capture.tail_seconds(cfg, 35) == pytest.approx(2.0)
    assert capture.tail_seconds(cfg, 48) == pytest.approx(2.0)
    assert capture.tail_seconds(cfg, 49) == pytest.approx(8.0)
    assert capture.tail_seconds(cfg, 54) == pytest.approx(8.0)
    assert capture.tail_seconds(cfg, 59) == pytest.approx(8.0)
    assert capture.tail_seconds(cfg, 60) == pytest.approx(2.0)
    assert capture.tail_seconds(cfg, 81) == pytest.approx(6.0)
    assert capture.tail_seconds(cfg, 84) == pytest.approx(10.0)


def test_a_capture_with_no_per_note_table_answers_its_flat_tail():
    """Which is every pitched capture, and the pipe organ's flat tail is not 2 s.

    Worth asserting rather than assuming: the render sides used to hardcode two
    seconds, so a capture that recorded four had its model measured over half
    the window its reference was measured over, with nothing reporting it.
    """
    assert capture.tail_seconds({"tail": "4s"}, 60) == pytest.approx(4.0)
    assert capture.tail_seconds({"tail": "2s", "tail_by_note": {}}, 60) == pytest.approx(2.0)


# --------------------------------------------------------------------------
# identifying what is loaded in a rack slot


def test_channel_spec_reads_ranges_and_singles_without_repeats():
    assert capture.parse_channels("1-8,11-13,15,16") == (1, 2, 3, 4, 5, 6, 7, 8,
                                                         11, 12, 13, 15, 16)
    assert capture.parse_channels("10") == (10,)
    # A repeat is the probe order the caller wrote, deduplicated rather than
    # rendered twice; out-of-range numbers are not MIDI channels.
    assert capture.parse_channels("10,10,1") == (10, 1)
    assert capture.parse_channels("0,17,3") == (3,)
    assert capture.parse_channels("") == ()


def test_where_a_rack_slot_sits_is_separate_from_what_its_notes_mean():
    """A kit on a slot other than 10 must still measure as a kit.

    One field cannot be both: addressing the slot on channel 10 plays whatever
    the rack happens to keep there, and declaring the kit on channel 15 sends
    the whole drum map through the pitched metric set, which reads as a
    successful measurement of the wrong instrument.
    """
    # Absent, the semantic channel addresses the slot — every capture written
    # before the split behaves exactly as it did.
    assert capture.slot_channel({"channel": 10}) == 10
    assert capture.slot_channel({"channel": 2}) == 2
    assert capture.slot_channel({}) == 1
    # Present, it addresses the slot and leaves the meaning alone.
    import profile as profile_module

    kit_b = {"channel": 10, "slot_channel": 15}
    assert capture.slot_channel(kit_b) == 15
    assert int(kit_b["channel"]) == profile_module.PERCUSSION_CHANNEL
    assert profile_module.is_percussion({"timbres": [{"channel": 10}, kit_b]})


def test_a_slot_is_a_kit_when_its_notes_land_on_their_own_peak_bands():
    # The rack this was written against: every percussion slot reads 8 to 13
    # distinct bands over thirteen diagnostic notes, so each is a whole map.
    assert capture.holds_a_whole_kit(10, 13)
    assert capture.holds_a_whole_kit(8, 13)
    # A single drum mapped across the keys answers every one of them the same
    # way. Taking one as a reference would score a whole kit against one piece,
    # which the band distance on its own cannot warn about.
    assert not capture.holds_a_whole_kit(1, 13)
    assert not capture.holds_a_whole_kit(2, 13)
    # The floor of three outranks the halving rule on a short probe, so four
    # notes reading two bands is not a kit even though two is half of four.
    assert not capture.holds_a_whole_kit(2, 4)
    assert capture.holds_a_whole_kit(3, 4)
    # Nothing measured is not a verdict.
    assert not capture.holds_a_whole_kit(0, 0)


def _harmonic(f0: float, partials: int = 8, sr: int = SR, seconds: float = 1.0) -> np.ndarray:
    t = np.arange(int(sr * seconds)) / sr
    y = np.zeros_like(t)
    for k in range(1, partials + 1):
        y += np.sin(2.0 * np.pi * f0 * k * t) / k
    return (y / np.max(np.abs(y))).astype(np.float32)


@pytest.mark.parametrize("note", [36, 48, 60, 72])
def test_harmonic_share_is_near_one_for_the_pitch_that_was_played(note):
    """The positive half of the slot probe: a slot that plays the key it is sent."""
    share = harmonic_share(_harmonic(midi_to_hz(note)), SR, midi_to_hz(note))
    assert share is not None and share > 0.95


def test_harmonic_share_is_low_for_a_sound_unrelated_to_the_key():
    """The negative half, and the one that decides a kit.

    A drum answers every key with the same instrument, so the render's series
    belongs to that instrument and not to the key. Scored here as a series a
    tritone-and-a-bit away from the key, which no partial of the key lands on.
    """
    played = midi_to_hz(60)
    share = harmonic_share(_harmonic(played * 1.41), SR, played)
    assert share is not None and share < 0.2


def test_harmonic_share_declines_to_answer_a_segment_it_cannot_resolve():
    """Too short to resolve the tolerance is unmeasured, not zero."""
    assert harmonic_share(_harmonic(midi_to_hz(60), seconds=0.01), SR, midi_to_hz(60)) is None
    assert harmonic_share(_harmonic(midi_to_hz(60)), SR, 0.0) is None
