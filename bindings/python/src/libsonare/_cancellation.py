"""Shared ctypes cancellation state for cancellable offline operations."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from ._ffi import SonareCancelCallback


class CancellationState:
    """Keep Python cancellation callbacks alive and safe across a C call."""

    def __init__(self, cancel: Callable[[], bool] | None) -> None:
        self._cancel = cancel

    def requested(self) -> int:
        """Return the C callback's nonzero cancellation signal."""
        if self._cancel is None:
            return 0
        try:
            return int(bool(self._cancel()))
        except Exception:  # noqa: BLE001 — never propagate Python exceptions into C
            return 1


def make_cancel_trampoline(state: CancellationState) -> Any:
    """Create a C callback that keeps ``state``'s cancellation policy alive."""

    def _trampoline(_user_data: int) -> int:
        return state.requested()

    return SonareCancelCallback(_trampoline)
