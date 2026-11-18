"""Failures from the library or object state raise SonareError with a code."""

from __future__ import annotations

import re
from typing import get_args

import pytest

from libsonare import ErrorCode, ErrorCodeName, Mixer, SonareError, cli
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
    detail = str(raised.value).split("failed to build mixer from scene JSON", 1)[1]
    assert detail.strip(": ").strip()


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
