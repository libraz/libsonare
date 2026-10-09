"""Shared helpers for mastering evaluation metrics."""

from __future__ import annotations

import numpy as np
import numpy.typing as npt


def rms_matched(
    reference: npt.NDArray[np.float64], other: npt.NDArray[np.float64]
) -> npt.NDArray[np.float64]:
    """Scale `other` by one global factor so its RMS equals the reference's."""
    rms = float(np.sqrt(np.mean(other**2)))
    if rms <= 0.0:
        return other
    return other * (float(np.sqrt(np.mean(reference**2))) / rms)
