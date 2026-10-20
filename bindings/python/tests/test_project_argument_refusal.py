"""Project argument refusals are SonareValueError; state refusals stay a coded SonareError."""

from __future__ import annotations

from collections.abc import Callable

import pytest

from libsonare import Project, SonareError, SonareValueError
from libsonare._errors import ErrorCode

_UNKNOWN_ID = 987654


def _with_audio_track(p: Project) -> int:
    return p.add_track("audio", "gtr")


_VALUE_REFUSALS: list[tuple[str, Callable[[Project], object]]] = [
    ("set_track_pan_out_of_range", lambda p: p.set_track_pan(_with_audio_track(p), 2.0)),
    ("set_track_pan_unknown_track", lambda p: p.set_track_pan(_UNKNOWN_ID, 0.0)),
    ("remove_track_unknown", lambda p: p.remove_track(_UNKNOWN_ID)),
    ("set_clip_gain_unknown_clip", lambda p: p.set_clip_gain(_UNKNOWN_ID, 1.0)),
    ("track_by_index_past_end", lambda p: p.track_by_index(5)),
]


@pytest.mark.parametrize(("name", "call"), _VALUE_REFUSALS, ids=[n for n, _ in _VALUE_REFUSALS])
def test_value_refusal_is_sonare_value_error(name: str, call: Callable[[Project], object]) -> None:
    p = Project()
    try:
        with pytest.raises(SonareValueError) as exc:
            call(p)
        assert exc.value.code == int(ErrorCode.INVALID_PARAMETER)
        assert isinstance(exc.value, SonareError)
    finally:
        p.close()


def test_undo_on_empty_history_is_coded_invalid_state() -> None:
    p = Project()
    try:
        with pytest.raises(SonareError) as exc:
            p.undo()
        assert not isinstance(exc.value, SonareValueError)
        assert exc.value.code == int(ErrorCode.INVALID_STATE)
    finally:
        p.close()


def test_call_on_closed_project_is_coded_invalid_state() -> None:
    p = Project()
    p.close()
    with pytest.raises(SonareError) as exc:
        p.remove_track(1)
    assert not isinstance(exc.value, SonareValueError)
    assert exc.value.code == int(ErrorCode.INVALID_STATE)
