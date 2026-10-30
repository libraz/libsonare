"""Shared ctypes callback state for cancellable offline operations.

A user callback must never raise into C. Each trampoline records the first
exception it sees, asks the native call to stop at its next cancellation point,
and the facade re-raises that exception once the call has returned.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from ._ffi import SONARE_OK, SonareCancelCallback


class CancellationState:
    """Keep Python cancellation callbacks alive and safe across a C call."""

    def __init__(self, cancel: Callable[[], bool] | None) -> None:
        self._cancel = cancel
        self.errors: list[BaseException] = []

    def requested(self) -> int:
        """Return the C callback's nonzero cancellation signal."""
        if self.errors:
            return 1
        if self._cancel is None:
            return 0
        try:
            return int(bool(self._cancel()))
        except BaseException as exc:  # noqa: BLE001 -- recorded here, re-raised by the facade
            self.errors.append(exc)
            return 1


def raise_callback_error(state: CancellationState, rc: int, release: Callable[[], object]) -> None:
    """Raise the first exception a user callback raised during the native call.

    ``release`` frees the result a call that still completed (``rc`` OK) produced,
    since the caller never reaches its own cleanup once this raises.
    """
    if not state.errors:
        return
    if rc == SONARE_OK:
        release()
    raise state.errors[0]


def make_cancel_trampoline(state: CancellationState) -> Any:
    """Create a C callback that keeps ``state``'s cancellation policy alive."""

    def _trampoline(_user_data: int) -> int:
        return state.requested()

    return SonareCancelCallback(_trampoline)


def make_progress_trampoline(
    callback_type: Callable[[Any], Any],
    on_progress: Callable[[float, str], None],
    state: CancellationState,
) -> Any:
    """Wrap ``on_progress`` as a C progress callback of ``callback_type``.

    A raising callback is recorded on ``state``, which turns the next
    cancellation check into a stop. The returned object MUST be kept alive across
    the C call or the ctypes closure is collected.
    """

    def _trampoline(progress: float, stage_cstr: bytes | None, _user_data: int) -> None:
        if state.errors:
            return
        try:
            stage = stage_cstr.decode("utf-8") if stage_cstr else ""
            on_progress(float(progress), stage)
        except BaseException as exc:  # noqa: BLE001 -- recorded here, re-raised by the facade
            state.errors.append(exc)

    return callback_type(_trampoline)
