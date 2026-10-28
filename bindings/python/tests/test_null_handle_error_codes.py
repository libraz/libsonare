"""A null-returning C constructor reports the code the library recorded."""

from __future__ import annotations

import math

import pytest

from libsonare import (
    ErrorCode,
    SonareError,
    StreamingRetune,
    mastering_insert_param_info,
)

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare not available")


def test_streaming_retune_refusal_is_coded_from_the_library() -> None:
    with pytest.raises(SonareError) as caught:
        StreamingRetune(semitones=math.nan)
    assert caught.value.code == int(ErrorCode.INVALID_PARAMETER)
    assert "finite" in str(caught.value)


def test_insert_param_info_rate_refusal_carries_the_library_detail() -> None:
    with pytest.raises(SonareError) as caught:
        mastering_insert_param_info("compressor", sample_rate=7)
    assert caught.value.code == int(ErrorCode.INVALID_PARAMETER)
    assert "sample_rate" in str(caught.value)


class _FailedGetterLib:
    """Stands in for the library after a never-NULL name getter caught an exception."""

    def sonare_last_error_code(self) -> int:
        return int(ErrorCode.OUT_OF_MEMORY)

    def sonare_last_error_message(self) -> bytes:
        return b"allocation failed"


def test_joined_names_raises_the_error_an_empty_getter_result_recorded(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from libsonare import _runtime

    monkeypatch.setattr(_runtime, "_get_lib", lambda: _FailedGetterLib())
    with pytest.raises(SonareError) as caught:
        _runtime._joined_names(b"", "mixing scene preset names")
    assert caught.value.code == int(ErrorCode.OUT_OF_MEMORY)
    assert "allocation failed" in str(caught.value)


class _CleanGetterLib:
    """Stands in for the library right after a getter that succeeded."""

    def sonare_last_error_code(self) -> int:
        return int(ErrorCode.OK)


def test_joined_names_reads_an_empty_success_as_no_names(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare import _runtime

    monkeypatch.setattr(_runtime, "_get_lib", lambda: _CleanGetterLib())
    assert _runtime._joined_names(b"", "names") == []
    assert _runtime._joined_names(b"a\nb\n", "names") == ["a", "b"]
