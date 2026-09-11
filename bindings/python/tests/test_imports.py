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


# ``load_library`` builds a fresh CDLL and runs the signature-configuration
# passes with no caching of its own, so the binding caches it in exactly one
# place. A module that grows a second ``_lib``/``_get_lib`` pair pays that cost
# again and gives ``SONARE_LIB_PATH`` a second resolution point, which can load
# a different file when the environment changes between the two first uses.
_LOADER_OWNERS = {"_ffi.py", "_runtime.py"}


def test_only_one_module_calls_the_library_loader() -> None:
    """Derived from the source: a second cached loader fails here."""
    root = Path(__file__).parents[1] / "src" / "libsonare"
    callers = set()
    for path in sorted(root.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and node.func.id == "load_library"
            ):
                callers.add(path.name)
    # Non-vacuity: the owner has to still be calling it.
    assert "_runtime.py" in callers
    assert callers <= _LOADER_OWNERS, f"a second library loader appeared in {sorted(callers)}"


def test_every_module_shares_one_configured_library(monkeypatch: pytest.MonkeyPatch) -> None:
    """Reaching the CDLL through Audio first must not load it a second time."""
    from libsonare import _runtime
    from libsonare import audio as audio_module

    loads = []
    real_load = _runtime.load_library

    def counting_load(*args, **kwargs):
        loads.append(args)
        return real_load(*args, **kwargs)

    monkeypatch.setattr(_runtime, "load_library", counting_load)
    monkeypatch.setattr(_runtime, "_lib", None)

    through_audio = audio_module._get_lib()
    through_runtime = _runtime._get_lib()

    assert through_audio is through_runtime
    assert len(loads) == 1, f"the library was loaded {len(loads)} times"
    # No module may keep a second cache of it.
    assert not hasattr(audio_module, "_lib")


def _abi_version_mirrors() -> list[tuple[str, str]]:
    """The (file, literal name) pairs check_abi_versions.py models for Python."""
    import importlib.util

    tool = Path(__file__).resolve().parents[3] / "tools" / "abi" / "check_abi_versions.py"
    spec = importlib.util.spec_from_file_location("_sonare_check_abi_versions", tool)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return [
        (path, re.match(r"(\w+)", pattern).group(1))
        for path, pattern, _key in module.MIRRORS
        if path.startswith("bindings/python/")
    ]


def test_every_abi_version_literal_lives_only_where_the_checker_reads_it() -> None:
    """An unmirrored duplicate stays stale through a bump while the check is green.

    check_abi_versions.py compares one file per literal against the C source of
    truth. A second copy elsewhere in the binding is simply unchecked, so it
    keeps claiming the old version after a bump with nothing to catch it.
    """
    root = Path(__file__).parents[1] / "src" / "libsonare"
    mirrors = _abi_version_mirrors()
    # Non-vacuity: the checker has to still model the Python binding.
    assert len(mirrors) >= 3, mirrors

    for modelled_path, literal in mirrors:
        modelled = Path(modelled_path).name
        declaring = set()
        for path in sorted(root.glob("*.py")):
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                targets = (
                    node.targets
                    if isinstance(node, ast.Assign)
                    else [node.target]
                    if isinstance(node, ast.AnnAssign)
                    else []
                )
                if any(isinstance(t, ast.Name) and t.id == literal for t in targets):
                    declaring.add(path.name)
        assert declaring == {modelled}, (
            f"{literal} must be declared only in {modelled}, the file "
            f"check_abi_versions.py reads; found it in {sorted(declaring)}"
        )


# Every handle-owning class assigns its handle sentinel before anything that can
# raise, so a failed construction cannot make __del__ read a missing attribute.
# The convention is documented on StreamAnalyzer; applying it is a per-class
# edit, so the set is derived from the source rather than listed.
def _handle_owning_inits() -> list[tuple[str, str, ast.FunctionDef]]:
    """Every class with a close()/__del__ whose __init__ assigns self._handle."""
    root = Path(__file__).parents[1] / "src" / "libsonare"
    found: list[tuple[str, str, ast.FunctionDef]] = []
    for path in sorted(root.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for cls in (n for n in ast.walk(tree) if isinstance(n, ast.ClassDef)):
            methods = {m.name: m for m in cls.body if isinstance(m, ast.FunctionDef)}
            init = methods.get("__init__")
            if init is None or not ({"close", "__del__"} & set(methods)):
                continue
            if any(
                isinstance(t, ast.Attribute)
                and isinstance(t.value, ast.Name)
                and t.value.id == "self"
                and t.attr == "_handle"
                for node in ast.walk(init)
                for t in (
                    node.targets
                    if isinstance(node, ast.Assign)
                    else [node.target]
                    if isinstance(node, ast.AnnAssign)
                    else []
                )
            ):
                found.append((path.name, cls.name, init))
    return found


def test_every_handle_class_sets_its_handle_before_anything_can_raise() -> None:
    """A raise before the assignment leaves __del__ reading a missing attribute."""
    owners = _handle_owning_inits()
    # Non-vacuity: the walk has to be finding the handle classes at all.
    assert len(owners) >= 8, owners

    late = []
    for filename, class_name, init in owners:
        body = init.body
        if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant):
            body = body[1:]  # a docstring cannot raise
        first = body[0] if body else None
        targets = (
            first.targets
            if isinstance(first, ast.Assign)
            else [first.target]
            if isinstance(first, ast.AnnAssign)
            else []
        )
        if not any(
            isinstance(t, ast.Attribute)
            and isinstance(t.value, ast.Name)
            and t.value.id == "self"
            and t.attr == "_handle"
            for t in targets
        ):
            late.append(f"{filename}::{class_name}")
    assert late == [], (
        "these constructors can raise before self._handle exists, so __del__ "
        f"would report an AttributeError over the real error: {late}"
    )


@pytest.mark.parametrize("subject", ["engine", "project"])
def test_a_failed_handle_construction_reports_only_the_real_error(
    monkeypatch: pytest.MonkeyPatch, subject: str
) -> None:
    """No second exception escapes __del__ to obscure the construction failure.

    Both classes have a bare ``__del__`` that calls ``close()``, so before the
    sentinel an AttributeError reached the unraisable hook and printed an
    "Exception ignored in" traceback pointing at the wrapper rather than at the
    incompatible library.
    """
    import gc
    import sys
    from types import SimpleNamespace

    import libsonare
    from libsonare import _project
    from libsonare import engine as engine_module

    if subject == "engine":
        stub = SimpleNamespace(
            sonare_engine_abi_version=lambda: engine_module.EXPECTED_ENGINE_ABI_VERSION + 1
        )
        monkeypatch.setattr(engine_module, "_get_lib", lambda: stub)
        construct = libsonare.RealtimeEngine
        expected = "engine ABI mismatch"
    else:

        def reject(_lib: object) -> None:
            raise RuntimeError("libsonare project ABI mismatch: stubbed")

        monkeypatch.setattr(_project, "_check_project_abi", reject)
        construct = libsonare.Project
        expected = "project ABI mismatch"

    unraisable: list[object] = []
    monkeypatch.setattr(sys, "unraisablehook", unraisable.append)

    message = ""
    try:
        construct()
    except RuntimeError as exc:
        message = str(exc)
    # The `except ... as` name is already gone, so nothing pins the traceback
    # that would otherwise keep the half-built instance alive past this point.
    gc.collect()

    assert expected in message
    assert unraisable == [], f"__del__ raised {unraisable}"
