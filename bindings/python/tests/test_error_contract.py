"""Failures from the library or object state raise SonareError with a code."""

from __future__ import annotations

import re
from collections.abc import Callable
from typing import get_args

import pytest

from libsonare import (
    ErrorCode,
    ErrorCodeName,
    Mixer,
    SonareError,
    _mastering_offline,
    _runtime,
    cli,
)
from libsonare._cli_common import EXIT_ABI_MISMATCH, EXIT_STALE_RESULT

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")


def test_abi_mismatch_raises_coded_error(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare import _ffi

    monkeypatch.setattr(_ffi, "EXPECTED_ABI_VERSION", _ffi.EXPECTED_ABI_VERSION + 1)
    with pytest.raises(SonareError) as raised:
        _ffi.load_library()
    assert raised.value.code == ErrorCode.ABI_MISMATCH
    assert raised.value.code_name == "AbiMismatch"
    assert "ABI mismatch" in str(raised.value)


def test_missing_symbol_raises_coded_abi_mismatch(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare import _ffi

    def configure_with_missing_symbol(lib) -> None:
        lib.sonare_symbol_no_library_exports.restype = None

    monkeypatch.setattr(_ffi, "configure_core_signatures", configure_with_missing_symbol)
    with pytest.raises(SonareError) as raised:
        _ffi.load_library()
    assert raised.value.code == ErrorCode.ABI_MISMATCH
    assert "sonare_symbol_no_library_exports" in str(raised.value)
    assert _ffi._find_library() in str(raised.value)


def test_project_abi_mismatch_raises_coded_error(monkeypatch: pytest.MonkeyPatch) -> None:
    from libsonare import _project_model

    monkeypatch.setattr(
        _project_model,
        "_EXPECTED_PROJECT_ABI_VERSION",
        _project_model._EXPECTED_PROJECT_ABI_VERSION + 1,
    )
    with pytest.raises(SonareError) as raised:
        _project_model._check_project_abi(_project_model._get_lib())
    assert raised.value.code == ErrorCode.ABI_MISMATCH


def test_bad_scene_json_carries_oracle_code_and_detail() -> None:
    with pytest.raises(SonareError) as raised:
        Mixer.from_json("{ not json", sample_rate=48000)
    assert type(raised.value) is SonareError
    assert raised.value.code != ErrorCode.OK
    message = str(raised.value)
    assert message.startswith(f"[{raised.value.code}] Mixer.from_json: ")
    assert message.split("Mixer.from_json: ", 1)[1].strip()
    assert "failed to build mixer" not in message


def test_a_null_handle_refusal_is_the_library_detail_under_the_entry_prefix() -> None:
    with pytest.raises(SonareError) as scene:
        Mixer.from_json('{"strips": [{"id": "a", "faderDB": 0}]}', sample_rate=48000)
    assert str(scene.value) == (
        f"[{scene.value.code}] Mixer.from_json: unknown key strips[0].faderDB"
    )
    mixer = Mixer(sample_rate=48000)
    mixer.add_strip("a")
    with pytest.raises(SonareError) as strip:
        mixer.add_strip("a")
    assert str(strip.value).startswith("Mixer.add_strip: ")
    assert str(strip.value).endswith("id must not be the id of a strip already in this mixer")
    assert "failed to add strip" not in str(strip.value)


class _FailingLib:
    """A library whose list getters fail, leaving ``detail`` as the last error."""

    def __init__(self, detail: bytes | None) -> None:
        self._detail = detail

    def sonare_last_error_code(self) -> int:
        return int(ErrorCode.INVALID_PARAMETER)

    def sonare_last_error_message(self) -> bytes | None:
        return self._detail

    def sonare_last_refusal_subject_count(self) -> int:
        return 0

    def sonare_last_error_reason(self) -> None:
        return None

    def sonare_mastering_insert_names(self) -> None:
        return None

    def sonare_mastering_insert_param_names(self, insert: bytes) -> None:
        return None


@pytest.mark.parametrize(
    ("detail", "expected"), [(b"the library said why", "the library said why"), (None, None)]
)
@pytest.mark.parametrize(
    ("call", "fallback"),
    [
        (lambda: _mastering_offline.mastering_insert_names(), "failed to list mastering inserts"),
        (
            lambda: _mastering_offline.mastering_insert_param_names("eq"),
            "failed to list mastering insert params",
        ),
        (
            lambda: _runtime._joined_names(b"", "realtime voice changer preset names"),
            "realtime voice changer preset names",
        ),
    ],
)
def test_a_failed_getter_reports_the_detail_alone_and_its_own_text_only_without_one(
    monkeypatch: pytest.MonkeyPatch,
    call: Callable[[], object],
    fallback: str,
    detail: bytes | None,
    expected: str | None,
) -> None:
    lib = _FailingLib(detail)
    monkeypatch.setattr(_runtime, "_get_lib", lambda: lib)
    monkeypatch.setattr(_mastering_offline, "_get_lib", lambda: lib)
    with pytest.raises(SonareError) as raised:
        call()
    assert str(raised.value).endswith(expected or fallback)
    if expected is not None:
        assert fallback not in str(raised.value)


def test_closed_mixer_raises_invalid_state() -> None:
    mixer = Mixer.from_json('{"strips":[]}', sample_rate=48000)
    mixer.close()
    with pytest.raises(SonareError) as raised:
        mixer.add_bus("b")
    assert raised.value.code == ErrorCode.INVALID_STATE


def test_abi_mismatch_maps_to_python_only_exit_code() -> None:
    assert EXIT_ABI_MISMATCH == 13
    exc = SonareError(ErrorCode.ABI_MISMATCH, "libsonare ABI mismatch")
    assert cli._exit_code_for(exc) == EXIT_ABI_MISMATCH


def test_stale_result_code_and_exit_code() -> None:
    assert int(ErrorCode.STALE_RESULT) == 11
    exc = SonareError(ErrorCode.STALE_RESULT, "stale")
    assert exc.code_name == "StaleResult"
    assert EXIT_STALE_RESULT == 14
    assert cli._exit_code_for(exc) == EXIT_STALE_RESULT


def test_code_name_is_the_shared_spelling_of_every_member() -> None:
    names = set(get_args(ErrorCodeName))
    for member in ErrorCode:
        exc = SonareError(member, "x")
        assert exc.code is member
        assert exc.code_name in names
        assert re.sub(r"(?<!^)(?=[A-Z])", "_", exc.code_name).upper() == member.name
    assert {SonareError(m, "x").code_name for m in ErrorCode} == names


def test_unknown_code_reads_as_the_unknown_member() -> None:
    exc = SonareError(12345, "x")
    assert exc.code is ErrorCode.UNKNOWN
    assert exc.code_name == "Unknown"
    assert str(exc).startswith("[12345] ")
