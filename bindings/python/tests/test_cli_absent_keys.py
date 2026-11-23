"""JSON output omits a value nothing measured, rather than writing null or 0 for it."""

from __future__ import annotations

import argparse
import json
from types import SimpleNamespace
from typing import Any

import pytest

import libsonare
import libsonare._cli_acoustic as acoustic
from libsonare import _cli_note_edit, _cli_project


def _json_out(capsys: pytest.CaptureFixture[str]) -> Any:
    return json.loads(capsys.readouterr().out)


def _acoustic_result(*, blind: bool) -> SimpleNamespace:
    measured = None if blind else 0.3
    bands = None if blind else [0.3]
    return SimpleNamespace(
        rt60=0.4,
        edt=measured,
        c50=measured,
        c80=measured,
        d50=measured,
        confidence=0.5,
        is_blind=blind,
        rt60_bands=[0.4],
        edt_bands=bands,
        c50_bands=bands,
        c80_bands=bands,
    )


_IR_ONLY_KEYS = ("edt", "c50", "c80", "d50", "edt_bands", "c50_bands", "c80_bands")


@pytest.mark.parametrize("blind", [True, False])
def test_acoustic_omits_what_only_an_impulse_response_measures(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], blind: bool
) -> None:
    monkeypatch.setattr(acoustic, "_load_audio", lambda _path: ([0.1], 48000))
    result = _acoustic_result(blind=blind)
    monkeypatch.setattr(libsonare, "detect_acoustic", lambda _samples, **_kwargs: result)
    monkeypatch.setattr(libsonare, "analyze_impulse_response", lambda _samples, **_kwargs: result)
    args = argparse.Namespace(file="in.wav", json=True, ir=not blind, n_octave_bands=1)

    assert acoustic.cmd_acoustic(args) == 0
    payload = _json_out(capsys)
    for key in _IR_ONLY_KEYS:
        assert (key in payload) is not blind, key
    # The absent keys are the statement; no flag beside them says it twice.
    assert "is_blind" not in payload
    assert payload["rt60"] == pytest.approx(0.4)


def _vocal_note(*, pitched: bool) -> SimpleNamespace:
    return SimpleNamespace(
        id=1,
        source_start_sample=0,
        source_end_sample=100,
        analysis_frame_start=0,
        analysis_frame_end=2,
        median_hz=440.0 if pitched else None,
        center_midi=69.0 if pitched else None,
        f0_stability=0.9 if pitched else None,
        amplitude=[0.5],
    )


_PITCH_KEYS = ("median_hz", "center_midi", "f0_stability", "note_name", "cents")


def test_vocal_notes_omit_every_pitch_key_of_an_unpitched_note(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    notes = (_vocal_note(pitched=True), _vocal_note(pitched=False))

    class _Session:
        def notes(self) -> libsonare.VocalNotesResult:
            return libsonare.VocalNotesResult(notes=notes, transitions=())

        def close(self) -> None:
            pass

    monkeypatch.setattr(_cli_note_edit, "_load_audio", lambda _path: ([0.0] * 100, 22050))
    monkeypatch.setattr(libsonare, "VocalEditSession", lambda *_a, **_k: _Session())
    from libsonare import cli

    parser = cli._build_parser()

    assert (
        _cli_note_edit.cmd_vocal_notes(parser.parse_args(["vocal-notes", "t.wav", "--json"])) == 0
    )
    pitched, unpitched = _json_out(capsys)["notes"]
    assert all(key in pitched for key in _PITCH_KEYS)
    assert pitched["note_name"] == "A4"
    assert not any(key in unpitched for key in _PITCH_KEYS)
    # The absent pitch keys are the statement; no flag beside them says it twice.
    assert "has_pitch" not in pitched and "has_pitch" not in unpitched


def test_polyphonic_notes_omit_the_median_of_a_note_with_no_usable_pitch(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    def note(median_hz: float | None) -> SimpleNamespace:
        return SimpleNamespace(
            onset_sample=0,
            offset_sample=10,
            frame_start=0,
            frame_end=1,
            median_hz=median_hz,
            median_cents=0.0,
            f0_stability=1.0,
            amplitude=[0.1],
        )

    class _Analysis:
        frame_count = 1

        @classmethod
        def analyze(cls, _samples: object, _sr: int) -> _Analysis:
            return cls()

        def __enter__(self) -> _Analysis:
            return self

        def __exit__(self, *_exc: object) -> None:
            return None

        def polyphony(self) -> list[int]:
            return [2]

        def notes(self) -> list[SimpleNamespace]:
            return [note(220.0), note(None)]

        def note_f0(self, _index: int) -> list[float]:
            return [0.0]

        def note_salience(self, _index: int) -> list[float]:
            return [0.0]

    monkeypatch.setattr(_cli_note_edit, "_load_audio", lambda _path: ([0.0] * 10, 22050))
    monkeypatch.setattr(libsonare, "PolyphonicAnalysis", _Analysis)

    assert _cli_note_edit.cmd_polyphonic_notes(argparse.Namespace(file="t.wav", json=True)) == 0
    measured, unmeasured = _json_out(capsys)["notes"]
    assert measured["median_hz"] == pytest.approx(220.0)
    assert "median_hz" not in unmeasured


def _compile_with(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    diagnostics: tuple[SimpleNamespace, ...],
    *,
    has_timeline: bool,
) -> tuple[int, dict[str, Any], list[str]]:
    closed: list[str] = []

    class _Timeline:
        def close(self) -> None:
            closed.append("timeline")

    result = SimpleNamespace(
        diagnostics=diagnostics, timeline=_Timeline() if has_timeline else None
    )

    class _Project:
        def compile(self, *, with_timeline: bool = False) -> SimpleNamespace:
            return result

        def close(self) -> None:
            closed.append("project")

    monkeypatch.setattr(_cli_project, "_load_project", lambda _path: _Project())
    args = argparse.Namespace(project_command="compile", input="p.json", json=True, output=None)
    code = _cli_project.cmd_project(args)
    return code, _json_out(capsys), closed


def test_project_compile_omits_the_target_of_a_project_wide_diagnostic(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    code, payload, _ = _compile_with(
        monkeypatch,
        capsys,
        (
            SimpleNamespace(code=1, severity="error", target_id=7, message="clip"),
            SimpleNamespace(code=2, severity="error", target_id=None, message="project"),
        ),
        has_timeline=False,
    )
    assert code != 0
    assert payload["diagnostic_count"] == 2
    assert payload["messages"] == "clip\nproject"
    targeted, project_wide = payload["diagnostics"]
    assert targeted["target_id"] == 7
    assert "target_id" not in project_wide


def test_project_compile_reports_the_timeline_it_got_not_one_inferred_from_severity(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    error = SimpleNamespace(code=1, severity="error", target_id=None, message="late")
    code, payload, closed = _compile_with(monkeypatch, capsys, (error,), has_timeline=True)
    assert payload["has_timeline"] is True
    assert code == 0
    assert closed == ["timeline", "project"]

    code, payload, closed = _compile_with(monkeypatch, capsys, (), has_timeline=False)
    assert payload["has_timeline"] is False
    assert payload["diagnostic_count"] == 0
    assert code != 0
    assert closed == ["project"]


def test_the_writer_holds_the_native_level_convention() -> None:
    from libsonare._cli_common import _absent_if_none, _strict_json_dumps

    payload = {
        "measured": 1.5,
        "nan": float("nan"),
        "none": _absent_if_none(None),
        "kept_none": None,
        "floor": float("-inf"),
        "nested": {"nan": float("nan"), "ceiling": float("inf")},
        "levels": [float("-inf"), 2.0],
    }
    assert json.loads(_strict_json_dumps(payload)) == {
        "measured": 1.5,
        "kept_none": None,
        "floor": None,
        "nested": {"ceiling": None},
        "levels": [None, 2.0],
    }


@pytest.mark.parametrize("entry", [float("nan"), "absent"])
def test_an_array_cannot_carry_a_not_measured_value(entry: object) -> None:
    from libsonare._cli_common import _absent_if_none, _strict_json_dumps

    value = _absent_if_none(None) if entry == "absent" else entry
    with pytest.raises(ValueError, match="not-measured"):
        _strict_json_dumps({"levels": [1.0, value]})


def test_measured_pairs_skip_only_unmeasured_entries() -> None:
    from libsonare._cli_common import _measured_pairs, _strict_json_dumps

    pairs = _measured_pairs([float("nan"), 3.0, None, float("-inf"), 5.0], "f0", "f0_frame")
    assert json.loads(_strict_json_dumps(pairs)) == {
        "f0": [3.0, None, 5.0],
        "f0_frame": [1, 3, 4],
    }
