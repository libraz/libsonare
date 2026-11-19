"""Focused parser and forwarding coverage for the Python ``pitch`` command."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from types import SimpleNamespace
from typing import Any, cast

import numpy as np
import pytest


def _parser() -> argparse.ArgumentParser:
    from libsonare import cli

    return cast(argparse.ArgumentParser, cli._build_parser())


def _pitch_args(*extra: str) -> argparse.Namespace:
    return _parser().parse_args(["pitch", "input.wav", *extra])


def _pitch_result() -> SimpleNamespace:
    return SimpleNamespace(
        f0=np.array([440.0]),
        voiced_flag=np.array([True]),
        median_f0=440.0,
        mean_f0=440.0,
    )


def test_pitch_parser_accepts_and_defaults_analysis_options() -> None:
    defaults = _pitch_args()
    assert defaults.algorithm == "pyin"
    assert defaults.threshold is None
    assert defaults.hop_length is None
    assert defaults.f0_min_hz is None
    assert defaults.f0_max_hz is None
    assert not hasattr(defaults, "output")

    explicit = _pitch_args(
        "--algorithm",
        "yin",
        "--threshold",
        "0.25",
        "--hop-length",
        "256",
        "--f0-min-hz",
        "80",
        "--f0-max-hz",
        "1000",
    )
    assert explicit.algorithm == "yin"
    assert explicit.threshold == 0.25
    assert explicit.hop_length == 256
    assert explicit.f0_min_hz == 80.0
    assert explicit.f0_max_hz == 1000.0


def test_pitch_omitted_options_are_left_to_the_library(monkeypatch: pytest.MonkeyPatch) -> None:
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
            "--f0-min-hz",
            "65.0",
            "--f0-max-hz",
            "2093.0",
        )
    )

    # Left out, nothing is passed; spelling the library's own values passes them.
    assert calls[0][1] == {"sample_rate": 22050}
    assert calls[1][1] == {
        "sample_rate": 22050,
        "hop_length": 512,
        "f0_min_hz": 65.0,
        "f0_max_hz": 2093.0,
        "threshold": 0.1,
    }


@pytest.mark.parametrize("option", ["-o", "--output"])
def test_pitch_rejects_output_at_parser_level(option: str) -> None:
    with pytest.raises(SystemExit) as exc_info:
        _pitch_args(option, "ignored.wav")
    assert exc_info.value.code == 2


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
        ("--f0-min-hz", "0"),
        ("--f0-min-hz", "-1"),
        ("--f0-min-hz", "nan"),
        ("--f0-max-hz", "0"),
        ("--f0-max-hz", "-1"),
        ("--f0-max-hz", "inf"),
        ("--f0-min-hz", "100", "--f0-max-hz", "99"),
    ],
)
def test_pitch_rejects_invalid_option_values(extra: tuple[str, ...]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        _pitch_args(*extra)
    assert exc_info.value.code == 2


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
        "--f0-min-hz",
        "90",
        "--f0-max-hz",
        "1200",
    )
    assert _cli_analysis.cmd_pitch(args) == 0

    assert calls == [
        (
            algorithm,
            {
                "sample_rate": 44100,
                "hop_length": 256,
                "f0_min_hz": 90.0,
                "f0_max_hz": 1200.0,
                "threshold": 0.35,
            },
        )
    ]


def test_pitch_frames_flag_defaults_off_and_adds_the_contour_to_json(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    import json

    import libsonare
    from libsonare import _cli_analysis

    assert _pitch_args().frames is False
    assert _pitch_args("--frames").frames is True

    monkeypatch.setattr(_cli_analysis, "_load_audio", lambda path: ([0.0], 22050))
    result = SimpleNamespace(
        f0=np.array([440.0, float("nan")]),
        voiced_flag=np.array([True, False]),
        voiced_prob=np.array([0.9, 0.0]),
        median_f0=440.0,
        mean_f0=440.0,
    )
    monkeypatch.setattr(libsonare, "pitch_pyin", lambda samples, **kwargs: result)

    _cli_analysis.cmd_pitch(_pitch_args("--json"))
    assert set(json.loads(capsys.readouterr().out)) == {
        "algorithm",
        "n_frames",
        "voiced_count",
        "voiced_ratio",
        "median_f0",
        "mean_f0",
    }

    _cli_analysis.cmd_pitch(_pitch_args("--json", "--frames"))
    payload = json.loads(capsys.readouterr().out)
    assert payload["times"] == [0.0, 512 / 22050]
    assert payload["f0"] == [440.0, None]
    assert payload["voiced_flag"] == [True, False]
    assert payload["voiced_prob"] == [0.9, 0.0]
