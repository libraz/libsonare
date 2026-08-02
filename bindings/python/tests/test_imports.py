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
