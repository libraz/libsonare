"""Minimal regression tests for ``import libsonare`` package surface.

These tests guard against the class of bug where ``libsonare/__init__.py``
re-exports a symbol from a submodule (e.g. ``.analyzer``) but the submodule
forgets to re-export it from its own backing module (``._effects``), causing
``import libsonare`` itself to fail with ``ImportError`` at load time.

Keep this file dependency-free (no audio synthesis, no native calls) so that
it is the very first thing to fail when the package surface regresses.
"""

from __future__ import annotations

import inspect


def test_import_libsonare() -> None:
    """``import libsonare`` must not raise at module load."""
    import libsonare  # noqa: F401  (import is the assertion)


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
