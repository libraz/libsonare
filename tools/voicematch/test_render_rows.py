"""Mono mode is consistent from captured oracle through model subprocesses."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import render_rows
from _repo import REPO_ROOT
from autofit import Evaluator, holdout_scorer
from autofit_test_fixtures import _fit_args, _write_corpus
from corpus import Corpus, corpus_oracle
from loss import probe_rows
from metrics import MONO_MODES, normalize_rms, to_mono
from patterns import Pattern
from render_model import loaded_library_path, render_model
from render_rows import render_model_rows_subprocess
from smf import Note, write_smf
from wavio import write_wav

SR = 48000
TUNING_BUILD = REPO_ROOT / "build-tuning"


@pytest.fixture(scope="module")
def tuning_build():
    from build_lib import dylib_path

    if dylib_path(TUNING_BUILD) is None:
        pytest.skip("build-tuning/lib/libsonare.dylib is unavailable")
    return TUNING_BUILD


def _stereo_piano_reference(build_dir: Path):
    from autofit_resolve import _score

    pattern, total, _ = _score(0, "sustain", "60", "80")
    smf = write_smf(pattern.notes, program=0, channel=pattern.channel, end_pad=pattern.tail)
    stereo = render_model(smf, total, SR, rig=False)
    return pattern, stereo


def test_child_subprocess_uses_each_mono_mode_for_rows_and_dumped_audio(tuning_build, monkeypatch):
    from build_lib import dylib_path

    library = dylib_path(tuning_build)
    assert library is not None
    monkeypatch.setenv("SONARE_LIB_PATH", str(library))
    pattern, stereo = _stereo_piano_reference(tuning_build)
    assert loaded_library_path() == str(library)
    expected_audio = {}
    expected_rows = {}

    for mode in MONO_MODES:
        raw = to_mono(stereo, mode)
        expected_audio[mode] = normalize_rms(raw).astype(np.float32)
        expected_rows[mode] = probe_rows(normalize_rms(raw), pattern, SR, raw=raw)
        rows, audio = render_model_rows_subprocess(
            tuning_build,
            0,
            "sustain",
            "60",
            velocities_csv="80",
            want_audio=True,
            mono_mode=mode,
        )
        np.testing.assert_allclose(audio, expected_audio[mode], rtol=0.0, atol=1e-7)
        assert rows[0]["peak_dbfs"] == pytest.approx(expected_rows[mode][0]["peak_dbfs"])
        np.testing.assert_allclose(
            rows[0]["harmonics_db"], expected_rows[mode][0]["harmonics_db"], atol=1e-9
        )

    assert np.max(np.abs(expected_audio["mean"] - expected_audio["left"])) > 0.05
    assert np.max(np.abs(expected_audio["left"] - expected_audio["loudest"])) > 0.01


def _cancelling_stereo_corpus(tmp_path: Path) -> tuple[Corpus, Pattern]:
    sr = SR
    n = sr
    time_s = np.arange(n, dtype=np.float64) / sr
    audio = np.zeros((n, 2), dtype=np.float32)
    early = (time_s >= 0.22) & (time_s < 0.48)
    early_tone = 0.35 * np.sin(2.0 * np.pi * 880.0 * (time_s - 0.22))
    audio[early, 0] = early_tone[early]
    audio[early, 1] = -early_tone[early]
    late = (time_s >= 0.58) & (time_s < 0.98)
    late_tone = 0.22 * np.sin(2.0 * np.pi * 440.0 * (time_s - 0.58))
    audio[late, 0] = late_tone[late]
    audio[late, 1] = late_tone[late]
    path = tmp_path / "cancel.wav"
    write_wav(path, audio, sr)
    corpus = Corpus(
        root=tmp_path,
        timbre="stereo-test",
        sample_rate=sr,
        gate_s=0.5,
        preroll_s=0.2,
        slot_s=0.8,
        renders={(60, 100): path},
        notes=(60,),
        velocities=(100,),
        dry=True,
        channel=1,
        slots={(60, 100): 0.8},
    )
    note = Note(60, 100, 0.0, 0.5)
    pattern = Pattern("stereo-test", [note], analysis_notes=[note], tail=0.3)
    return corpus, pattern


def _peak_hz(segment: np.ndarray, sr: int) -> float:
    spectrum = np.abs(np.fft.rfft(segment))
    return float(np.fft.rfftfreq(len(segment), 1.0 / sr)[np.argmax(spectrum)])


def test_corpus_preroll_detection_uses_the_configured_mono_mode(tmp_path):
    corpus, pattern = _cancelling_stereo_corpus(tmp_path)

    mean_audio = corpus_oracle(corpus, pattern, SR)
    left_audio = corpus_oracle(corpus, pattern, SR, mono_mode="left")

    assert _peak_hz(mean_audio[: int(0.15 * SR), 0], SR) == pytest.approx(440.0, abs=5.0)
    assert _peak_hz(left_audio[: int(0.15 * SR), 0], SR) == pytest.approx(880.0, abs=5.0)


def test_cache_identity_includes_mono_mode(tmp_path):
    from autofit_test_fixtures import _bounded_knob

    build_dir = tmp_path / "build"
    library_dir = build_dir / "lib"
    library_dir.mkdir(parents=True)
    (library_dir / "libsonare.dylib").write_bytes(b"test-library")
    knob = _bounded_knob("x.k", 0.0, 1.0, 0.5)

    def signature(mode):
        args = _fit_args(
            program=0,
            pattern="sustain",
            notes="60",
            velocities="80",
            mono_mode=mode,
        )
        return Evaluator([knob], {}, [], None, args, build_dir).cache_signature()

    assert signature("mean") != signature("left")


@pytest.mark.parametrize(("mode", "expected"), [(None, "mean"), ("left", "left")])
def test_new_fit_record_names_its_mono_mode(tmp_path, mode, expected):
    from report import report_result

    args = _fit_args(
        program=0,
        pattern="sustain",
        notes="60",
        velocities="80",
        out=str(tmp_path / f"{expected}.json"),
        dry_run=True,
    )
    if mode is not None:
        args.mono_mode = mode
    evaluator = Evaluator([], {}, [], None, args, tmp_path)

    report_result([], {}, [], evaluator, args)

    record = json.loads(Path(args.out).read_text())
    assert record["mono_mode"] == expected


def test_training_render_passes_mode_to_the_real_child_process(tuning_build, monkeypatch):
    from build_lib import dylib_path

    library = dylib_path(tuning_build)
    assert library is not None
    monkeypatch.setenv("SONARE_LIB_PATH", str(library))
    original = render_rows.render_model_rows_subprocess
    expected_rows, _ = original(
        tuning_build,
        0,
        "sustain",
        "60",
        velocities_csv="80",
        mono_mode="left",
    )
    seen = []

    def observe(*args, **kwargs):
        seen.append(kwargs.get("mono_mode", "mean"))
        return original(*args, **kwargs)

    monkeypatch.setattr(autofit, "render_model_rows_subprocess", observe)
    args = _fit_args(
        program=0,
        pattern="sustain",
        notes="60",
        velocities="80",
        mono_mode="left",
    )
    evaluator = Evaluator([], {}, expected_rows, None, args, tuning_build)

    terms = evaluator._render_terms([])

    assert seen == ["left"]
    assert terms is not None


def test_holdout_render_passes_mode_to_the_real_child_process(tmp_path, tuning_build, monkeypatch):
    from build_lib import dylib_path

    library = dylib_path(tuning_build)
    assert library is not None
    monkeypatch.setenv("SONARE_LIB_PATH", str(library))
    corpus_path = _write_corpus(
        tmp_path / "corpus",
        notes=(60, 72),
        velocities=(100,),
        gate_ms=500,
        seconds=1.0,
        preroll_ms=200,
    )
    original = render_rows.render_model_rows_subprocess
    seen = []

    def observe(*args, **kwargs):
        seen.append(kwargs.get("mono_mode", "mean"))
        return original(*args, **kwargs)

    monkeypatch.setattr(autofit, "render_model_rows_subprocess", observe)
    args = _fit_args(
        program=0,
        pattern="sustain",
        notes="60",
        velocities="100",
        corpus=str(corpus_path),
        corpus_timbre="t",
        validate_notes="72",
        mono_mode="loudest",
    )

    scorer, axis, held = holdout_scorer(args, tuning_build, [], None)
    result = scorer([])

    assert (axis, held) == ("notes", "72")
    assert np.isfinite(result)
    assert seen == ["loudest"]


def test_room_dry_probe_passes_mode_to_the_real_child_process(tmp_path, tuning_build, monkeypatch):
    from types import SimpleNamespace

    from build_lib import dylib_path
    from knobs import Knob

    library = dylib_path(tuning_build)
    assert library is not None
    monkeypatch.setenv("SONARE_LIB_PATH", str(library))
    spec = tmp_path / "spec.json"
    spec.write_text(
        json.dumps({"weights": {}, "knobs": [{"tunable": "test.k", "min": 0, "max": 1}]})
    )
    seen = []
    original = render_rows.render_model_rows_subprocess

    def observe(*args, **kwargs):
        seen.append(kwargs.get("mono_mode", "mean"))
        return original(*args, **kwargs)

    monkeypatch.setattr(autofit, "render_model_rows_subprocess", observe)
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("head", []))
    monkeypatch.setattr(
        autofit,
        "build_knobs",
        lambda *_args: [
            Knob(label="test.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="test.k")
        ],
    )
    monkeypatch.setattr(autofit, "configure_build", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(autofit, "build_shared", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        autofit,
        "oracle_reference",
        lambda _args: ([], None, SimpleNamespace(to_dict=dict), None),
    )
    monkeypatch.setattr(autofit, "fit_room_ir", lambda *_args, **_kwargs: np.ones(1))
    monkeypatch.setattr(autofit.Evaluator, "check_overrides_reach", lambda *_args: None)
    monkeypatch.setattr(autofit, "run_diagnosis", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(autofit, "_fold_tree_provenance", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "autofit.py",
            "--spec",
            str(spec),
            "--program",
            "0",
            "--build-dir",
            str(tuning_build),
            "--pattern",
            "sustain",
            "--notes",
            "60",
            "--velocities",
            "80",
            "--mono-mode",
            "left",
            "--diagnose",
        ],
    )

    assert autofit.main() == 0
    assert seen == ["left"]
