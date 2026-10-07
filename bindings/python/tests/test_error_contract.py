"""Failures from the library or object state raise SonareError with a code."""

from __future__ import annotations

import pytest

from libsonare import ErrorCode, Mixer, SonareError, cli
from libsonare._cli_common import EXIT_ABI_MISMATCH

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
        "EXPECTED_PROJECT_ABI_VERSION",
        _project_model.EXPECTED_PROJECT_ABI_VERSION + 1,
    )
    with pytest.raises(SonareError) as raised:
        _project_model._check_project_abi(_project_model._get_lib())
    assert raised.value.code == ErrorCode.ABI_MISMATCH


def test_bad_scene_json_carries_oracle_code_and_detail() -> None:
    with pytest.raises(SonareError) as raised:
        Mixer.from_scene_json("{ not json")
    assert type(raised.value) is SonareError
    assert raised.value.code != ErrorCode.OK
    detail = str(raised.value).split("failed to build mixer from scene JSON", 1)[1]
    assert detail.strip(": ").strip()


def test_closed_mixer_raises_invalid_state() -> None:
    mixer = Mixer.from_scene_json('{"sampleRate":48000,"strips":[]}')
    mixer.close()
    with pytest.raises(SonareError) as raised:
        mixer.add_bus("b")
    assert raised.value.code == ErrorCode.INVALID_STATE


def test_abi_mismatch_maps_to_python_only_exit_code() -> None:
    assert EXIT_ABI_MISMATCH == 13
    exc = SonareError(ErrorCode.ABI_MISMATCH, "libsonare ABI mismatch")
    assert cli._exit_code_for(exc) == EXIT_ABI_MISMATCH
