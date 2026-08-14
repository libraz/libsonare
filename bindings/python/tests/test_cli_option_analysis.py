"""Focused forwarding coverage for analysis CLI options."""

from __future__ import annotations

import argparse
from types import SimpleNamespace
from typing import Any, cast

import pytest


def _parser() -> argparse.ArgumentParser:
    from libsonare import cli

    return cast(argparse.ArgumentParser, cli._build_parser())


def _key_args(**overrides: object) -> argparse.Namespace:
    values: dict[str, object] = {
        "file": "input.wav",
        "n_fft": 4096,
        "hop_length": 512,
        "use_hpss": False,
        "hpss": False,
        "loudness_weighted": False,
        "high_pass_hz": 0.0,
        "modes": "",
        "profile": "",
        "genre_hint": "",
        "candidates": 0,
        "json": True,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def _key_result() -> SimpleNamespace:
    return SimpleNamespace(
        root=SimpleNamespace(value=0),
        mode=SimpleNamespace(value=0),
        confidence=0.8,
    )


def test_key_hpss_canonical_and_legacy_spellings_forward_identically(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare
    from libsonare import _cli_analysis

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    calls: list[dict[str, object]] = []

    def fake_detect_key(samples: Any, **kwargs: object) -> SimpleNamespace:
        calls.append(kwargs)
        return _key_result()

    monkeypatch.setattr(libsonare, "detect_key", fake_detect_key)

    assert _cli_analysis.cmd_key(_key_args(use_hpss=True)) == 0
    assert _cli_analysis.cmd_key(_key_args(hpss=True)) == 0

    assert calls[0] == calls[1]
    assert calls[0]["use_hpss"] is True


def test_key_candidates_default_is_empty_and_bare_legacy_value_means_five(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare
    from libsonare import _cli_analysis

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    key_calls: list[dict[str, object]] = []
    candidate_calls: list[dict[str, object]] = []

    def fake_detect_key(samples: Any, **kwargs: object) -> SimpleNamespace:
        key_calls.append(kwargs)
        return _key_result()

    def fake_detect_key_candidates(samples: Any, **kwargs: object) -> list[object]:
        candidate_calls.append(kwargs)
        return []

    monkeypatch.setattr(libsonare, "detect_key", fake_detect_key)
    monkeypatch.setattr(libsonare, "detect_key_candidates", fake_detect_key_candidates)

    assert _cli_analysis.cmd_key(_key_args()) == 0
    assert candidate_calls == []

    assert _cli_analysis.cmd_key(_key_args(candidates=True)) == 0
    assert len(candidate_calls) == 1
    assert candidate_calls[0] == key_calls[0]


def test_chords_forwards_smoothing_window_and_beat_sync_flag(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare
    from libsonare import _cli_analysis

    args = _parser().parse_args(
        [
            "chords",
            "input.wav",
            "--smoothing-window",
            "1.25",
            "--no-beat-sync",
            "--json",
        ]
    )
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    calls: list[dict[str, object]] = []

    def fake_detect_chords(samples: Any, **kwargs: object) -> SimpleNamespace:
        calls.append(kwargs)
        return SimpleNamespace(chords=[])

    monkeypatch.setattr(libsonare, "detect_chords", fake_detect_chords)

    assert _cli_analysis.cmd_chords(args) == 0
    assert calls == [
        {
            "sample_rate": 22050,
            "min_duration": 0.3,
            "smoothing_window": 1.25,
            "threshold": 0.5,
            "use_triads_only": False,
            "n_fft": 2048,
            "hop_length": 512,
            "use_beat_sync": False,
            "use_hmm": False,
            "hmm_beam_width": 24,
            "use_key_context": False,
            "key_root": 0,
            "key_mode": 0,
            "detect_inversions": False,
            "chroma_method": "stft",
        }
    ]


def test_mel_forwards_htk_flag(monkeypatch: pytest.MonkeyPatch) -> None:
    import libsonare
    from libsonare import _cli_analysis

    args = _parser().parse_args(["mel", "input.wav", "--htk", "--json"])
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    calls: list[dict[str, object]] = []

    def fake_mel(samples: Any, **kwargs: object) -> SimpleNamespace:
        calls.append(kwargs)
        return SimpleNamespace(n_mels=128, n_frames=1, sample_rate=22050, hop_length=512)

    monkeypatch.setattr(libsonare, "mel_spectrogram", fake_mel)

    assert _cli_analysis.cmd_mel(args) == 0
    assert calls == [
        {
            "sample_rate": 22050,
            "n_fft": 2048,
            "hop_length": 512,
            "n_mels": 128,
            "fmin": 0.0,
            "fmax": 0.0,
            "htk": True,
        }
    ]


@pytest.mark.parametrize("algorithm", ["yin", "pyin"])
def test_pitch_forwards_threshold_hop_and_frequency_bounds(
    monkeypatch: pytest.MonkeyPatch, algorithm: str
) -> None:
    import libsonare
    from libsonare import _cli_analysis

    args = _parser().parse_args(
        [
            "pitch",
            "input.wav",
            "--algorithm",
            algorithm,
            "--threshold",
            "0.35",
            "--hop-length",
            "256",
            "--fmin",
            "90",
            "--fmax",
            "1200",
            "--json",
        ]
    )
    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 44100))
    calls: list[tuple[str, dict[str, object]]] = []

    def fake_pitch(name: str):
        def run(samples: Any, **kwargs: object) -> SimpleNamespace:
            calls.append((name, kwargs))
            return SimpleNamespace(n_frames=1, voiced_flag=[True], median_f0=440.0, mean_f0=440.0)

        return run

    monkeypatch.setattr(libsonare, "pitch_yin", fake_pitch("yin"))
    monkeypatch.setattr(libsonare, "pitch_pyin", fake_pitch("pyin"))

    assert _cli_analysis.cmd_pitch(args) == 0
    assert calls == [
        (
            algorithm,
            {
                "sample_rate": 44100,
                "hop_length": 256,
                "fmin": 90.0,
                "fmax": 1200.0,
                "threshold": 0.35,
            },
        )
    ]
