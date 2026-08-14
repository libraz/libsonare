"""Focused handler tests for the v2 effect/acoustic CLI options."""

from __future__ import annotations

import argparse
from types import SimpleNamespace

import pytest

import libsonare
import libsonare._cli_effects as effects


def _args(**values: object) -> argparse.Namespace:
    defaults = {
        "file": "input.wav",
        "output": "output.wav",
        "json": False,
    }
    defaults.update(values)
    return argparse.Namespace(**defaults)


def _capture_emit(monkeypatch: pytest.MonkeyPatch) -> list[dict[str, object]]:
    calls: list[dict[str, object]] = []

    def emit(args: argparse.Namespace, result: list[float], sr: int, **kwargs: object) -> int:
        calls.append({"args": args, "result": result, "sample_rate": sr, **kwargs})
        return 0

    monkeypatch.setattr(effects, "_emit_effect_result", emit)
    return calls


def test_pitch_shift_and_time_stretch_forward_fft_options(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1, 0.2], 22050))
    emitted = _capture_emit(monkeypatch)
    calls: list[tuple[str, dict[str, object]]] = []

    def fake_pitch_shift(samples: list[float], **kwargs: object) -> list[float]:
        calls.append(("pitch_shift", kwargs))
        assert samples == [0.1, 0.2]
        return [0.2, 0.3]

    def fake_time_stretch(samples: list[float], **kwargs: object) -> list[float]:
        calls.append(("time_stretch", kwargs))
        assert samples == [0.1, 0.2]
        return [0.3, 0.4]

    monkeypatch.setattr(libsonare, "pitch_shift", fake_pitch_shift)
    assert effects.cmd_pitch_shift(_args(semitones=3.0, n_fft=1024, hop_length=256)) == 0
    monkeypatch.setattr(libsonare, "time_stretch", fake_time_stretch)
    assert effects.cmd_time_stretch(_args(rate=1.2, n_fft=1024, hop_length=256)) == 0

    assert calls == [
        (
            "pitch_shift",
            {"sample_rate": 22050, "semitones": 3.0, "n_fft": 1024, "hop_length": 256},
        ),
        (
            "time_stretch",
            {"sample_rate": 22050, "rate": 1.2, "n_fft": 1024, "hop_length": 256},
        ),
    ]
    assert [call["result"] for call in emitted] == [[0.2, 0.3], [0.3, 0.4]]


def test_pitch_correct_forwards_requested_pitch_to_constant_facade(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 44100))
    emitted = _capture_emit(monkeypatch)
    calls: dict[str, object] = {}

    def fake_pitch_correct(samples: list[float], **kwargs: object) -> list[float]:
        calls.update({"samples": samples, **kwargs})
        return [0.5]

    monkeypatch.setattr(libsonare, "pitch_correct_to_midi", fake_pitch_correct)
    assert effects.cmd_pitch_correct(_args(current_midi=60.0, target_midi=64.0)) == 0

    assert calls == {
        "samples": [0.1],
        "sample_rate": 44100,
        "current_midi": 60.0,
        "target_midi": 64.0,
    }
    assert emitted[0]["result"] == [0.5]


def test_hpss_forwards_all_controls_and_writes_selected_artifacts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1, 0.2], 22050))
    calls: list[dict[str, object]] = []
    writes: list[tuple[str, list[float], int]] = []

    def fake_hpss(samples: list[float], **kwargs: object) -> SimpleNamespace:
        calls.append(kwargs)
        return SimpleNamespace(
            harmonic=[0.4, 0.5], percussive=[0.1, 0.2], length=2, sample_rate=22050
        )

    monkeypatch.setattr(libsonare, "hpss", fake_hpss)
    monkeypatch.setattr(
        effects,
        "_write_wav",
        lambda path, samples, sample_rate: writes.append((path, list(samples), sample_rate)),
    )

    assert (
        effects.cmd_hpss(
            _args(
                output="stems.wav",
                kernel_harmonic=15,
                kernel_percussive=21,
                n_fft=1024,
                hop_length=256,
                hard_mask=True,
                harmonic_only=True,
            )
        )
        == 0
    )
    assert calls == [
        {
            "sample_rate": 22050,
            "kernel_harmonic": 15,
            "kernel_percussive": 21,
            "n_fft": 1024,
            "hop_length": 256,
            "hard_mask": True,
        }
    ]
    assert writes == [("stems.wav", [0.4, 0.5], 22050)]


def test_hpss_residual_mode_writes_three_artifacts_and_conflicts_are_rejected(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 22050))
    writes: list[str] = []
    monkeypatch.setattr(
        effects,
        "_write_wav",
        lambda path, _samples, _sample_rate: writes.append(path),
    )
    monkeypatch.setattr(
        libsonare,
        "hpss_with_residual",
        lambda _samples, **_kwargs: {
            "harmonic": [0.1],
            "percussive": [0.2],
            "residual": [0.3],
            "sampleRate": 22050,
        },
    )

    assert effects.cmd_hpss(_args(output="split.wav", with_residual=True)) == 0
    assert writes == ["split_harmonic.wav", "split_percussive.wav", "split_residual.wav"]

    with pytest.raises(ValueError, match="mutually exclusive"):
        effects.cmd_hpss(_args(harmonic_only=True, percussive_only=True))


def test_normalize_mode_and_trim_controls_forward_to_the_matching_facade(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 22050))
    emitted = _capture_emit(monkeypatch)
    normalize_calls: list[tuple[str, dict[str, object]]] = []

    monkeypatch.setattr(
        libsonare,
        "normalize",
        lambda _samples, **kwargs: normalize_calls.append(("peak", kwargs)) or [0.2],
    )
    monkeypatch.setattr(
        libsonare,
        "normalize_rms",
        lambda _samples, **kwargs: normalize_calls.append(("rms", kwargs)) or [0.3],
    )
    assert effects.cmd_normalize(_args(mode="rms", target_db=-12.0)) == 0
    assert normalize_calls == [("rms", {"sample_rate": 22050, "target_db": -12.0})]

    trim_calls: list[tuple[str, dict[str, object]]] = []
    monkeypatch.setattr(
        libsonare,
        "trim_silence",
        lambda _samples, **kwargs: trim_calls.append(("top", kwargs)) or ([0.4], 0, 1),
    )
    assert (
        effects.cmd_trim_silence(_args(threshold_db=None, top_db=24.0, n_fft=1024, hop_length=256))
        == 0
    )
    assert trim_calls == [("top", {"top_db": 24.0, "frame_length": 1024, "hop_length": 256})]
    assert [call["result"] for call in emitted] == [[0.3], [0.4]]


def test_trim_threshold_and_top_db_conflict_is_semantic_error(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 22050))
    with pytest.raises(ValueError, match="threshold-db.*top-db"):
        effects.cmd_trim_silence(_args(threshold_db=-40.0, top_db=24.0))


def test_resample_accepts_target_sr_alias_as_canonical_target_rate(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1, 0.2], 44100))
    calls: list[tuple[list[float], int, int]] = []
    writes: list[tuple[str, int]] = []
    monkeypatch.setattr(
        effects,
        "_resample",
        lambda samples, source_rate, target_rate: (
            calls.append((samples, source_rate, target_rate)) or [0.1]
        ),
    )
    monkeypatch.setattr(
        effects,
        "_write_wav",
        lambda path, _samples, sample_rate: writes.append((path, sample_rate)),
    )

    assert effects.cmd_resample(_args(target_sr=16000, target_rate=None)) == 0
    assert calls == [([0.1, 0.2], 44100, 16000)]
    assert writes == [("output.wav", 16000)]


def test_acoustic_handlers_forward_ir_and_blind_controls(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 48000))
    calls: list[tuple[str, dict[str, object]]] = []
    result = SimpleNamespace(
        rt60=0.1,
        edt=0.1,
        c50=0.0,
        c80=0.0,
        d50=0.5,
        confidence=1.0,
        is_blind=False,
        rt60_bands=[],
        edt_bands=[],
        c50_bands=[],
        c80_bands=[],
    )
    monkeypatch.setattr(
        libsonare,
        "analyze_impulse_response",
        lambda _samples, **kwargs: calls.append(("ir", kwargs)) or result,
    )
    monkeypatch.setattr(
        libsonare,
        "detect_acoustic",
        lambda _samples, **kwargs: calls.append(("blind", kwargs)) or result,
    )

    assert (
        effects.cmd_acoustic(
            _args(ir=True, n_bands=4, min_decay_db=24.0, noise_floor_margin_db=8.0)
        )
        == 0
    )
    assert (
        effects.cmd_acoustic(
            _args(ir=False, n_bands=5, min_decay_db=18.0, noise_floor_margin_db=6.0)
        )
        == 0
    )
    assert calls == [
        (
            "ir",
            {"sample_rate": 48000, "n_octave_bands": 4, "min_decay_db": 24.0},
        ),
        (
            "blind",
            {
                "sample_rate": 48000,
                "n_octave_bands": 5,
                "min_decay_db": 18.0,
                "noise_floor_margin_db": 6.0,
            },
        ),
    ]


def test_estimate_room_forwards_band_controls_and_uses_zero_default(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(effects, "_load_audio", lambda _path: ([0.1], 48000))
    calls: list[dict[str, object]] = []
    result = SimpleNamespace(
        volume=10.0,
        length=3.0,
        width=2.0,
        height=1.7,
        drr_db=0.0,
        confidence=0.5,
        rt60_bands=[],
        absorption_bands=[],
    )
    monkeypatch.setattr(
        libsonare,
        "estimate_room",
        lambda _samples, **kwargs: calls.append(kwargs) or result,
    )

    args = _args(
        aspect_lw=1.0,
        aspect_lh=1.0,
        reference_absorption=0.15,
        sabine=False,
        n_octave_bands=None,
    )
    assert effects.cmd_estimate_room(args) == 0
    assert calls == [
        {
            "sample_rate": 48000,
            "aspect_hint_lw": 1.0,
            "aspect_hint_lh": 1.0,
            "reference_absorption": 0.15,
            "prefer_eyring": True,
            "n_octave_bands": 0,
            "min_decay_db": 0.0,
            "noise_floor_margin_db": 0.0,
        }
    ]


@pytest.mark.parametrize(
    "kwargs",
    [
        {"preset": "bright-idol", "preset_json": "preset.json"},
        {"preset": "bright-idol", "pitch_semitones": 2.0},
        {"preset_pack": "pack.json", "pitch_semitones": 2.0},
        {"set": ["dsp.outputGainDb=-2"]},
    ],
)
def test_voice_change_rejects_noop_selector_combinations(
    monkeypatch: pytest.MonkeyPatch, kwargs: dict[str, object]
) -> None:
    monkeypatch.setattr(
        effects,
        "_load_audio",
        lambda _path: pytest.fail("semantic rejection must happen before audio loading"),
    )
    with pytest.raises(ValueError):
        effects.cmd_voice_change(_args(**kwargs))
