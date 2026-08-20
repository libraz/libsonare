"""Handle-lifecycle contract tests for :class:`libsonare.Audio`.

Every public accessor on a handle-owning class either operates on a live
handle or raises. None of them may return a value indistinguishable from a
legitimate measurement once the handle is gone, because silent-empty audio
flows into downstream analysis unnoticed.
"""

from __future__ import annotations

import gc
import sys
from typing import Any

import pytest

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

# Members that need a positional argument; everything else on Audio is either
# a property or fully defaulted, so the table stays this short.
_REQUIRED_ARGS: dict[str, tuple[tuple[Any, ...], dict[str, Any]]] = {
    "resample": ((22050,), {}),
    "mastering_process": (("limiter",), {}),
}

# close() is the teardown entry point itself and must stay callable (and a
# no-op) on an already-closed handle, so it is not an accessor under test.
_NOT_ACCESSORS = frozenset({"close"})

# Accessors that dereference self._handle directly rather than going through
# the data / sample_rate properties. The parametrized sweep covers far more
# than these; they are pinned so a filter bug cannot quietly empty the sweep.
_DIRECT_HANDLE_MEMBERS = frozenset(
    {
        "data",
        "length",
        "sample_rate",
        "duration",
        "detect_bpm",
        "detect_beats",
        "detect_downbeats",
        "detect_onsets",
        "analyze",
    }
)


def _public_members() -> list[str]:
    """Return every public instance member of Audio that reads the handle.

    Classmethods are constructors, not accessors: they never touch an
    instance handle, so a closed instance says nothing about them.
    """
    from libsonare.audio import Audio

    return sorted(
        name
        for name, member in vars(Audio).items()
        if not name.startswith("_")
        and not isinstance(member, classmethod)
        and name not in _NOT_ACCESSORS
    )


def _make_audio() -> Any:
    from libsonare import Audio

    return Audio.from_buffer(sine(440.0, 0.25, sr=22050), sample_rate=22050)


def _invoke(audio: Any, name: str) -> Any:
    from libsonare.audio import Audio

    if isinstance(vars(Audio)[name], property):
        return getattr(audio, name)
    args, kwargs = _REQUIRED_ARGS.get(name, ((), {}))
    return getattr(audio, name)(*args, **kwargs)


def test_sweep_covers_the_direct_handle_members() -> None:
    """The parametrized sweep is derived from the class, so keep it honest."""
    assert set(_public_members()) >= _DIRECT_HANDLE_MEMBERS


@pytest.mark.parametrize("name", _public_members())
def test_member_raises_after_close(name: str) -> None:
    """Every public accessor raises once the handle is freed."""
    audio = _make_audio()
    audio.close()
    try:
        value = _invoke(audio, name)
    except RuntimeError as exc:
        # SonareError subclasses RuntimeError, so match the guard's own
        # message: a C-level failure on a NULL handle is not the contract.
        assert "Audio is closed" in str(exc), (
            f"Audio.{name} raised {exc!r} after close() instead of the closed-handle guard"
        )
        return
    pytest.fail(f"Audio.{name} returned {repr(value)[:200]} after close() instead of raising")


def test_close_is_idempotent() -> None:
    """Closing twice is a no-op, not an error."""
    audio = _make_audio()
    audio.close()
    audio.close()


def test_context_manager_closes_the_handle() -> None:
    """Leaving a with block frees the handle and arms the guard."""
    with _make_audio() as audio:
        assert audio.length > 0
    with pytest.raises(RuntimeError, match="Audio is closed"):
        _ = audio.duration


def test_del_after_close_is_silent() -> None:
    """Teardown of a closed instance raises nothing an unraisable hook sees."""
    recorded: list[Any] = []
    previous = sys.unraisablehook
    sys.unraisablehook = recorded.append
    try:
        audio = _make_audio()
        audio.close()
        del audio
        gc.collect()
    finally:
        sys.unraisablehook = previous
    assert recorded == []


def test_del_without_close_is_silent() -> None:
    """An open instance still frees cleanly when it is garbage collected."""
    recorded: list[Any] = []
    previous = sys.unraisablehook
    sys.unraisablehook = recorded.append
    try:
        audio = _make_audio()
        del audio
        gc.collect()
    finally:
        sys.unraisablehook = previous
    assert recorded == []
