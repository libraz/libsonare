"""Focused parser and forwarding coverage for the Python ``pitch`` command."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from types import SimpleNamespace
from typing import Any, cast

import pytest


def _parser() -> argparse.ArgumentParser:
    from libsonare import cli

    return cast(argparse.ArgumentParser, cli._build_parser())


def _pitch_args(*extra: str) -> argparse.Namespace:
    return _parser().parse_args(["pitch", "input.wav", *extra])


def _pitch_result() -> SimpleNamespace:
    return SimpleNamespace(
        n_frames=1,
        voiced_flag=[True],
        median_f0=440.0,
        mean_f0=440.0,
    )


def test_pitch_parser_accepts_and_defaults_analysis_options() -> None:
    defaults = _pitch_args()
    assert defaults.algorithm == "pyin"
    assert defaults.threshold == 0.1
    assert defaults.hop_length == 512
    assert defaults.fmin == 65.0
    assert defaults.fmax == 2093.0
    assert not hasattr(defaults, "output")

    explicit = _pitch_args(
        "--algorithm",
        "yin",
        "--threshold",
        "0.25",
        "--hop-length",
        "256",
        "--fmin",
        "80",
        "--fmax",
        "1000",
    )
    assert explicit.algorithm == "yin"
    assert explicit.threshold == 0.25
    assert explicit.hop_length == 256
    assert explicit.fmin == 80.0
    assert explicit.fmax == 1000.0


def test_pitch_omitted_options_are_legacy_equivalent(monkeypatch: pytest.MonkeyPatch) -> None:
    import libsonare
    from libsonare import _cli_analysis

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    calls: list[tuple[str, dict[str, object]]] = []

    def fake_pyin(samples: Any, **kwargs: object) -> SimpleNamespace:
        calls.append(("pyin", kwargs))
        return _pitch_result()

    monkeypatch.setattr(libsonare, "pitch_pyin", fake_pyin)

    _cli_analysis.cmd_pitch(_pitch_args())
    _cli_analysis.cmd_pitch(
        _pitch_args(
            "--threshold",
            "0.1",
            "--hop-length",
            "512",
            "--fmin",
            "65.0",
            "--fmax",
            "2093.0",
        )
    )

    assert calls[0] == calls[1]
    assert calls[0][1] == {
        "sample_rate": 22050,
        "hop_length": 512,
        "fmin": 65.0,
        "fmax": 2093.0,
        "threshold": 0.1,
    }


@pytest.mark.parametrize("option", ["-o", "--output"])
@pytest.mark.parametrize(
    ("legacy", "expected_code"),
    [(False, 2), (True, 1)],
    ids=["normal-exit", "legacy-exit"],
)
def test_pitch_rejects_output_at_parser_level(
    monkeypatch: pytest.MonkeyPatch,
    option: str,
    legacy: bool,
    expected_code: int,
) -> None:
    if legacy:
        monkeypatch.setenv("SONARE_LEGACY_EXIT", "1")
    else:
        monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)

    with pytest.raises(SystemExit) as exc_info:
        _pitch_args(option, "ignored.wav")
    assert exc_info.value.code == expected_code


@pytest.mark.parametrize(
    "extra",
    [
        ("--threshold", "nan"),
        ("--threshold", "0"),
        ("--threshold", "1.01"),
        ("--threshold", "inf"),
        ("--hop-length", "0"),
        ("--hop-length", "-1"),
        ("--hop-length", "not-an-integer"),
        ("--fmin", "0"),
        ("--fmin", "-1"),
        ("--fmin", "nan"),
        ("--fmax", "0"),
        ("--fmax", "-1"),
        ("--fmax", "inf"),
        ("--fmin", "100", "--fmax", "99"),
    ],
)
@pytest.mark.parametrize(
    ("legacy", "expected_code"),
    [(False, 2), (True, 1)],
    ids=["normal-exit", "legacy-exit"],
)
def test_pitch_rejects_invalid_option_values(
    monkeypatch: pytest.MonkeyPatch,
    extra: tuple[str, ...],
    legacy: bool,
    expected_code: int,
) -> None:
    if legacy:
        monkeypatch.setenv("SONARE_LEGACY_EXIT", "1")
    else:
        monkeypatch.delenv("SONARE_LEGACY_EXIT", raising=False)

    with pytest.raises(SystemExit) as exc_info:
        _pitch_args(*extra)
    assert exc_info.value.code == expected_code


@pytest.mark.parametrize("algorithm", ["yin", "pyin"])
def test_pitch_forwards_nondefault_options_to_selected_core_call(
    monkeypatch: pytest.MonkeyPatch, algorithm: str
) -> None:
    import libsonare
    from libsonare import _cli_analysis

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 44100))
    calls: list[tuple[str, dict[str, object]]] = []

    def fake_pitch(name: str) -> Callable[..., SimpleNamespace]:
        def run(samples: Any, **kwargs: object) -> SimpleNamespace:
            calls.append((name, kwargs))
            return _pitch_result()

        return run

    monkeypatch.setattr(libsonare, "pitch_yin", fake_pitch("yin"))
    monkeypatch.setattr(libsonare, "pitch_pyin", fake_pitch("pyin"))

    args = _pitch_args(
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
    )
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
