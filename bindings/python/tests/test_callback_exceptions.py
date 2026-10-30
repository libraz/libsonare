"""A raising progress or cancel callback is the exception the facade raises."""

from __future__ import annotations

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE, sine

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050


class _CallbackError(Exception):
    """Stands for the caller's own exception."""


def _material() -> np.ndarray:
    return sine(440.0, 0.5, sr=SR, amp=0.2)


def _repair(**callbacks):
    return libsonare.mastering_repair_apply(
        [_material()], SR, stages=[{"stage": "decrackle"}, {"stage": "declick"}], **callbacks
    )


def _chain(**callbacks):
    return libsonare.mastering_chain(
        _material(), SR, {"dynamics": {"compressor": {"thresholdDb": -24.0}}}, **callbacks
    )


def _analyze(**callbacks):
    if not hasattr(libsonare._runtime._get_lib(), "sonare_analyze_json_ex_with_progress"):
        pytest.skip("libsonare built without sonare_analyze_json_ex_with_progress")
    return libsonare.analyze_with_progress(sine(440.0, 1.0, sr=SR), SR, **callbacks)


ENTRY_POINTS = [_repair, _chain, _analyze]


@pytest.mark.parametrize("entry", ENTRY_POINTS)
def test_a_raising_progress_callback_is_raised_and_not_called_again(entry) -> None:
    calls: list[str] = []

    def on_progress(_progress: float, stage: str) -> None:
        calls.append(stage)
        raise _CallbackError(stage)

    with pytest.raises(_CallbackError):
        entry(on_progress=on_progress)
    assert len(calls) == 1


@pytest.mark.parametrize("entry", ENTRY_POINTS)
def test_a_raising_cancel_callback_is_raised_and_not_polled_again(entry) -> None:
    polls: list[int] = []

    def cancel() -> bool:
        polls.append(1)
        raise _CallbackError("cancel")

    with pytest.raises(_CallbackError):
        entry(cancel=cancel)
    assert len(polls) == 1
