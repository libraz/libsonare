"""CLI tests for ``vocal-notes``: the read-only listing of a vocal-edit session's notes."""

from __future__ import annotations

import argparse
import json
import math
import os
import tempfile
from typing import cast

import pytest

# ruff: noqa: F403,F405
from ._analyzer_helpers import *
from ._analyzer_helpers import _run_cli, _write_test_wav

_RATE = 22050
# Three sung-like tones with short gaps: A4, C5, and an E5 sung 30 cents sharp.
_TONES_HZ = (440.0, 523.2511306011972, 659.2551138257398 * 2.0 ** (30.0 / 1200.0))
_EXPECTED_NAMES = ("A4", "C5", "E5")
_EXPECTED_CENTS = (0.0, 0.0, 30.0)


def _write_take(path: str) -> None:
    samples: list[float] = []
    for hz in _TONES_HZ:
        samples += [
            0.5 * math.sin(2.0 * math.pi * hz * i / _RATE) for i in range(_RATE * 28 // 100)
        ]
        samples += [0.0] * (_RATE * 3 // 100)
    _write_test_wav(path, samples, _RATE)


def _parser() -> argparse.ArgumentParser:
    from libsonare import cli

    return cast(argparse.ArgumentParser, cli._build_parser())


def _parse(*extra: str) -> argparse.Namespace:
    return _parser().parse_args(["vocal-notes", "take.wav", *extra])


def test_vocal_notes_options_default_to_absence() -> None:
    """An omitted setting reaches the handler as None, so the session default applies."""
    args = _parse()
    for dest in (
        "segmentation_threshold_cents",
        "min_note_ms",
        "frame_length_samples",
        "hop_length_samples",
        "fmin_hz",
        "fmax_hz",
        "yin_threshold",
        "voiced_threshold",
        "reference_hz",
    ):
        assert getattr(args, dest) is None, dest
    assert args.no_centered is False


def test_vocal_notes_omitted_settings_are_not_passed(monkeypatch: pytest.MonkeyPatch) -> None:
    import libsonare
    from libsonare import _cli_note_edit

    monkeypatch.setattr(_cli_note_edit, "_load_audio", lambda path: ([0.0] * 4096, _RATE))
    calls: list[dict[str, object]] = []

    class _Session:
        def notes(self) -> tuple[tuple[object, ...], tuple[object, ...]]:
            return (), ()

        def close(self) -> None:
            pass

    def fake_create(samples: object, sample_rate: int, **kwargs: object) -> _Session:
        calls.append(kwargs)
        return _Session()

    monkeypatch.setattr(libsonare, "create_vocal_edit_session", fake_create)

    assert _cli_note_edit.cmd_vocal_notes(_parse()) == 0
    assert (
        _cli_note_edit.cmd_vocal_notes(
            _parse("--min-note-ms", "80", "--fmax-hz", "1000", "--no-centered")
        )
        == 0
    )
    assert calls == [{}, {"min_note_ms": 80.0, "fmax_hz": 1000.0, "centered": False}]


@pytest.mark.parametrize(
    "extra",
    [
        ("--segmentation-threshold-cents", "0"),
        ("--min-note-ms", "-1"),
        ("--frame-length-samples", "0"),
        ("--hop-length-samples", "0"),
        ("--hop-length-samples", "not-an-integer"),
        ("--fmin-hz", "0"),
        ("--fmax-hz", "0"),
        ("--yin-threshold", "0"),
        ("--yin-threshold", "1.5"),
        ("--voiced-threshold", "1.5"),
        ("--voiced-threshold", "-0.1"),
        ("--reference-hz", "0"),
        ("--reference-hz", "nan"),
        ("--fmin-hz", "3000"),
        ("--fmax-hz", "50"),
        ("--fmin-hz", "100", "--fmax-hz", "99"),
    ],
)
def test_vocal_notes_rejects_out_of_domain_values(extra: tuple[str, ...]) -> None:
    with pytest.raises(SystemExit) as exc_info:
        _parse(*extra)
    assert exc_info.value.code == 2


def test_vocal_notes_reports_each_note_with_its_name_and_offset() -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "take.wav")
        _write_take(wav_path)

        result = _run_cli(["vocal-notes", wav_path, "--json"])
        assert result.returncode == 0, result.stderr
        payload = json.loads(result.stdout)
        assert set(payload) == {"sample_rate", "note_count", "notes"}
        assert payload["sample_rate"] == _RATE
        assert payload["note_count"] == 3
        assert [note["note_name"] for note in payload["notes"]] == list(_EXPECTED_NAMES)

        previous_end = 0.0
        for index, note in enumerate(payload["notes"]):
            assert note["index"] == index
            assert note["has_pitch"] is True
            assert note["cents"] == pytest.approx(_EXPECTED_CENTS[index], abs=12.0)
            midi = note["center_midi"]
            assert note["cents"] == pytest.approx((midi - math.floor(midi + 0.5)) * 100.0)
            assert note["start_sec"] == pytest.approx(note["source_start_sample"] / _RATE)
            assert note["end_sec"] == pytest.approx(note["source_end_sample"] / _RATE)
            assert note["start_sec"] >= previous_end
            assert note["end_sec"] > note["start_sec"]
            previous_end = note["end_sec"]
            assert note["median_hz"] > 0.0
            assert note["amplitude"]

        # Spelling every default is the same as spelling none.
        spelled = _run_cli(
            [
                "vocal-notes",
                wav_path,
                "--segmentation-threshold-cents",
                "50",
                "--min-note-ms",
                "30",
                "--frame-length-samples",
                "2048",
                "--hop-length-samples",
                "512",
                "--fmin-hz",
                "65",
                "--fmax-hz",
                "2093",
                "--yin-threshold",
                "0.1",
                "--voiced-threshold",
                "0.5",
                "--reference-hz",
                "440",
                "--json",
            ]
        )
        assert spelled.returncode == 0, spelled.stderr
        assert spelled.stdout == result.stdout

        # Notes shorter than the floor no longer stand alone.
        long_floor = _run_cli(["vocal-notes", wav_path, "--min-note-ms", "400", "--json"])
        assert long_floor.returncode == 0, long_floor.stderr
        assert json.loads(long_floor.stdout)["note_count"] < 3

        uncentered = _run_cli(["vocal-notes", wav_path, "--no-centered", "--json"])
        assert uncentered.returncode == 0, uncentered.stderr
        assert json.loads(uncentered.stdout)["note_count"] == 3

        text = _run_cli(["vocal-notes", wav_path])
        assert text.returncode == 0, text.stderr
        assert "Vocal notes: 3" in text.stdout
        assert "A4" in text.stdout and "E5" in text.stdout and "cents" in text.stdout
