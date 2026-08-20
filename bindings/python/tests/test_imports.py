"""Minimal regression tests for ``import libsonare`` package surface.

These tests guard against the class of bug where ``libsonare/__init__.py``
re-exports a symbol from a submodule (e.g. ``.analyzer``) but the submodule
forgets to re-export it from its own backing module (``._effects``), causing
``import libsonare`` itself to fail with ``ImportError`` at load time.

Keep this file dependency-free (no audio synthesis, no native calls) so that
it is the very first thing to fail when the package surface regresses.
"""

from __future__ import annotations

import ast
import inspect
import re
import types
from pathlib import Path

import pytest


def test_import_libsonare() -> None:
    """``import libsonare`` must not raise at module load."""
    import libsonare  # noqa: F401  (import is the assertion)


def test_track_monitor_mode_is_public_enum() -> None:
    from enum import IntEnum

    import libsonare

    assert issubclass(libsonare.EngineTrackMonitorMode, IntEnum)
    assert libsonare.EngineTrackMonitorMode.OFF.value == 0
    assert libsonare.EngineTrackMonitorMode.PFL.value == 1
    assert libsonare.EngineTrackMonitorMode.AFL.value == 2
    assert "EngineTrackMonitorMode" in libsonare.__all__


def test_error_codes_are_public_and_named() -> None:
    """Python errors expose the same branchable code contract as JS bindings."""
    import libsonare

    error = libsonare.SonareError(libsonare.ErrorCode.INVALID_PARAMETER, "bad input")
    assert error.code == libsonare.ErrorCode.INVALID_PARAMETER
    assert error.code_name == "InvalidParameter"
    assert libsonare.SonareError(12345, "unknown").code_name == "Unknown"


def test_library_abi_is_checked_before_configuring_symbols(monkeypatch: pytest.MonkeyPatch) -> None:
    """An old dylib produces the ABI error before any newer symbol is looked up."""
    import libsonare._ffi as ffi

    fake_library = types.SimpleNamespace(sonare_abi_version=lambda: 0)
    monkeypatch.setattr(ffi.ctypes, "CDLL", lambda _path: fake_library)

    def unexpected_configuration(_lib: object) -> None:
        pytest.fail("configured function signatures before checking the ABI")

    monkeypatch.setattr(ffi, "configure_core_signatures", unexpected_configuration)
    with pytest.raises(RuntimeError, match="ABI mismatch"):
        ffi.load_library("/tmp/old-libsonare.dylib")


def test_realtime_voice_changer_symbols_exposed() -> None:
    """All 5 realtime voice-changer symbols must be reachable on the package."""
    import libsonare

    expected_callables = (
        "voice_change_realtime",
        "realtime_voice_changer_preset_json",
        "realtime_voice_changer_preset_names",
        "validate_realtime_voice_changer_preset_json",
    )
    for name in expected_callables:
        assert hasattr(libsonare, name), f"libsonare.{name} is missing"
        obj = getattr(libsonare, name)
        assert callable(obj), f"libsonare.{name} is not callable (got {type(obj).__name__})"

    # The class export must resolve to an actual class object.
    assert hasattr(libsonare, "RealtimeVoiceChanger"), "libsonare.RealtimeVoiceChanger is missing"
    assert inspect.isclass(libsonare.RealtimeVoiceChanger), (
        f"libsonare.RealtimeVoiceChanger is not a class "
        f"(got {type(libsonare.RealtimeVoiceChanger).__name__})"
    )


def test_realtime_voice_changer_symbols_in_all() -> None:
    """The realtime voice-changer symbols must be advertised via ``__all__``."""
    import libsonare

    expected = {
        "RealtimeVoiceChanger",
        "voice_change_realtime",
        "realtime_voice_changer_preset_json",
        "realtime_voice_changer_preset_names",
        "validate_realtime_voice_changer_preset_json",
    }
    missing = expected - set(libsonare.__all__)
    assert not missing, f"libsonare.__all__ is missing: {sorted(missing)}"


def test_public_api_stub_reexports_all() -> None:
    """Every name in ``__all__`` must be re-exported by ``__init__.pyi``.

    ``py.typed`` ships the stub, so under PEP 561 ``__init__.pyi`` shadows the
    runtime package for type checkers. A name present in ``__all__`` but absent
    from the stub as an explicit ``X as X`` re-export (or a direct annotation
    such as ``__version__: str``) is seen as non-exported, so consumers get a
    spurious "no attribute" from mypy/pyright. This guard makes that drift a red
    test instead of a silent typing regression.
    """
    pkg = Path(__file__).resolve().parent.parent / "src" / "libsonare"
    init_py = pkg / "__init__.py"
    init_pyi = pkg / "__init__.pyi"

    all_names: list[str] = []
    for node in ast.walk(ast.parse(init_py.read_text())):
        if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "__all__" for t in node.targets
        ):
            all_names = [e.value for e in node.value.elts if isinstance(e, ast.Constant)]

    assert all_names, "__all__ could not be parsed from __init__.py"

    stub = init_pyi.read_text()
    missing = [
        name
        for name in all_names
        if not re.search(rf"\b{re.escape(name)} as {re.escape(name)}\b", stub)
        and not re.search(rf"^{re.escape(name)}\s*:", stub, re.MULTILINE)
    ]
    assert not missing, (
        "__init__.pyi does not re-export these public names (add "
        f"`from .<module> import X as X`): {sorted(missing)}"
    )


def test_loaded_library_exports_every_guarded_symbol() -> None:
    """The loaded dylib must export every symbol the binding probes for.

    ``_ffi_signatures_*.py`` wraps most C symbols in ``if hasattr(lib, ...)`` so
    an older library keeps loading, and the tests that exercise those code paths
    skip themselves the same way. That combination makes a build with a C-ABI
    translation unit missing from the source list look completely green: the
    facade raises "libsonare was built without the ... ABI" at runtime while
    every test that would have caught it opted out.

    This asserts the inventory instead. The expected set is derived from the
    guards themselves, so it cannot fall behind the binding, and a missing
    symbol is a failure rather than a skip. Symbol lookup only -- nothing here
    calls into the library.
    """
    from libsonare._runtime import _get_lib

    signature_dir = Path(__file__).resolve().parent.parent / "src" / "libsonare"
    guarded: set[str] = set()
    for path in sorted(signature_dir.glob("_ffi_signatures_*.py")):
        guarded |= set(re.findall(r"""hasattr\(lib,\s*["'](sonare_\w+)["']\)""", path.read_text()))

    # The derivation must not collapse silently: an empty or tiny set would make
    # this pass vacuously, which is the failure mode it exists to remove.
    assert len(guarded) >= 150, (
        f"only {len(guarded)} guarded symbols found in _ffi_signatures_*.py; "
        "the scan broke rather than the binding shrinking"
    )

    lib = _get_lib()
    missing = sorted(name for name in guarded if not hasattr(lib, name))
    assert not missing, (
        f"the loaded libsonare is missing {len(missing)} symbol(s) the Python facade "
        f"declares and ships as public API: {missing}. This usually means a C-ABI "
        "translation unit was dropped from a source list in src/CMakeLists.txt, or "
        "the loader picked up a stale dylib -- set SONARE_LIB_PATH explicitly to "
        "check. Do not silence this by skipping: the wheel would ship those calls "
        "as documented APIs that raise at runtime."
    )
