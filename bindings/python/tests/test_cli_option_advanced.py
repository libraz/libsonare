"""Regression coverage for advanced CLI option forwarding."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from types import SimpleNamespace

import pytest


def _args(**options: object) -> argparse.Namespace:
    return argparse.Namespace(file="input.wav", json=True, **options)


def test_rhythm_and_dynamics_forward_analysis_options(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """CLI analysis knobs reach the corresponding Python analysis facades."""
    import libsonare
    from libsonare import _cli_advanced

    monkeypatch.setattr(_cli_advanced, "_load_audio", lambda _path: ([0.0, 0.25], 22050))
    calls: dict[str, tuple[object, ...] | dict[str, object]] = {}

    def rhythm_spy(samples: list[float], sample_rate: int, **kwargs: object) -> object:
        calls["rhythm"] = (samples, sample_rate, kwargs)
        return SimpleNamespace(
            bpm=120.0,
            time_signature=SimpleNamespace(numerator=4, denominator=4, confidence=0.9),
            groove_type="straight",
            syncopation=0.0,
            pattern_regularity=1.0,
            tempo_stability=1.0,
            beat_intervals=[],
        )

    def dynamics_spy(samples: list[float], sample_rate: int, **kwargs: object) -> object:
        calls["dynamics"] = (samples, sample_rate, kwargs)
        return SimpleNamespace(
            dynamic_range_db=1.0,
            peak_db=-1.0,
            rms_db=-6.0,
            crest_factor=1.2,
            loudness_range_db=0.5,
            is_compressed=False,
            loudness_rms_db=[],
        )

    monkeypatch.setattr(libsonare, "analyze_rhythm", rhythm_spy)
    monkeypatch.setattr(libsonare, "analyze_dynamics", dynamics_spy)

    assert (
        _cli_advanced.cmd_rhythm(
            _args(start_bpm=123.0, bpm_min=55.0, bpm_max=240.0, n_fft=1024, hop_length=256)
        )
        == 0
    )
    assert _cli_advanced.cmd_dynamics(_args(window_sec=0.75)) == 0
    capsys.readouterr()

    assert calls["rhythm"] == (
        [0.0, 0.25],
        22050,
        {
            "start_bpm": 123.0,
            "bpm_min": 55.0,
            "bpm_max": 240.0,
            "n_fft": 1024,
            "hop_length": 256,
        },
    )
    assert calls["dynamics"] == ([0.0, 0.25], 22050, {"window_sec": 0.75})


def test_nnls_chroma_forwards_hop_length(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """The CLI preserves the optional ABI facade's hop-length extension."""
    from libsonare import _cli_advanced, _conversions

    monkeypatch.setattr(_cli_advanced, "_load_audio", lambda _path: ([0.0], 22050))
    calls: dict[str, object] = {}

    def nnls_spy(
        samples: list[float], sample_rate: int, **kwargs: object
    ) -> tuple[int, list[float]]:
        calls["args"] = (samples, sample_rate, kwargs)
        return 1, [0.0] * 12

    monkeypatch.setattr(_conversions, "nnls_chroma", nnls_spy)

    assert _cli_advanced.cmd_nnls_chroma(_args(hop_length=256)) == 0
    capsys.readouterr()

    assert calls["args"] == ([0.0], 22050, {"hop_length": 256})


def test_tempogram_forwards_mel_and_window_options(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Tempogram computes its envelope with the requested Mel front-end."""
    from libsonare import _cli_advanced, _conversions

    monkeypatch.setattr(_cli_advanced, "_load_audio", lambda _path: ([0.0, 0.5], 22050))
    calls: dict[str, object] = {}

    def onset_spy(samples: list[float], **kwargs: object) -> list[float]:
        calls["onset"] = (samples, kwargs)
        return [0.1, 0.2, 0.3]

    def tempogram_spy(envelope: list[float], **kwargs: object) -> tuple[int, list[float]]:
        calls["tempogram"] = (envelope, kwargs)
        return 3, [0.0] * (3 * 96)

    monkeypatch.setattr(_conversions, "onset_envelope", onset_spy)
    monkeypatch.setattr(_conversions, "tempogram", tempogram_spy)

    assert (
        _cli_advanced.cmd_tempogram(_args(n_fft=1024, hop_length=256, n_mels=40, win_length=96))
        == 0
    )
    payload = json.loads(capsys.readouterr().out)

    assert calls["onset"] == (
        [0.0, 0.5],
        {"sample_rate": 22050, "n_fft": 1024, "hop_length": 256, "n_mels": 40},
    )
    assert calls["tempogram"] == (
        [0.1, 0.2, 0.3],
        {"sample_rate": 22050, "hop_length": 256, "win_length": 96},
    )
    assert payload["win_length"] == 96


def test_plp_forwards_mel_tempo_and_window_options(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """PLP forwards both its onset front-end and tempo-band configuration."""
    from libsonare import _cli_advanced, _conversions

    monkeypatch.setattr(_cli_advanced, "_load_audio", lambda _path: ([0.0, 0.5], 22050))
    calls: dict[str, object] = {}

    def onset_spy(samples: list[float], **kwargs: object) -> list[float]:
        calls["onset"] = (samples, kwargs)
        return [0.1, 0.2]

    def plp_spy(envelope: list[float], **kwargs: object) -> list[float]:
        calls["plp"] = (envelope, kwargs)
        return [0.0, 1.0]

    monkeypatch.setattr(_conversions, "onset_envelope", onset_spy)
    monkeypatch.setattr(_conversions, "plp", plp_spy)

    assert (
        _cli_advanced.cmd_plp(
            _args(
                n_fft=4096,
                hop_length=128,
                n_mels=64,
                tempo_min=70.0,
                tempo_max=180.0,
                win_length=192,
            )
        )
        == 0
    )
    json.loads(capsys.readouterr().out)

    assert calls["onset"] == (
        [0.0, 0.5],
        {"sample_rate": 22050, "n_fft": 4096, "hop_length": 128, "n_mels": 64},
    )
    assert calls["plp"] == (
        [0.1, 0.2],
        {
            "sample_rate": 22050,
            "hop_length": 128,
            "tempo_min": 70.0,
            "tempo_max": 180.0,
            "win_length": 192,
        },
    )


def test_native_rhythm_handlers_use_explicit_mel_envelopes() -> None:
    """Native rhythm handlers consume n-fft, hop-length, and n-mels."""
    source_path = Path(__file__).parents[3] / "tools" / "cli" / "sonare_cli_features_metering.cpp"
    source = source_path.read_text(encoding="utf-8")

    tempogram = source[source.index("int cmd_tempogram(") : source.index("int cmd_plp(")]
    plp = source[source.index("int cmd_plp(") : source.index("int cmd_cqt(")]
    for function, call in (
        (tempogram, "tempogram(envelope, audio.sample_rate(), config)"),
        (plp, "plp(envelope, config)"),
    ):
        assert "mel_config.n_fft = args.n_fft;" in function
        assert "mel_config.hop_length = args.hop_length;" in function
        assert "mel_config.n_mels = args.n_mels;" in function
        assert "compute_onset_strength(audio, mel_config, onset_config)" in function
        assert call in function

    onset = source[
        source.index("int cmd_onset_envelope(") : source.index("int cmd_fourier_tempogram(")
    ]
    assert "mel_config.n_mels = args.n_mels;" in onset
