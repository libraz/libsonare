"""Every row of the generated spelling table reaches a refusal that names the caller's keyword.

The rows come from ``_field_spelling.OVERRIDES``, so a row the generator adds is swept
without a pin. Each row's keyword is given values outside any domain until the library
refuses one; every refusal on the way must spell the subject as the caller wrote it.
"""

from __future__ import annotations

import inspect
import math
import re
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareError
from libsonare._field_spelling import OVERRIDES

_SR = 22050
_TONE = (0.5 * np.sin(2 * np.pi * 440 * np.arange(_SR // 2) / _SR)).astype(np.float32)

# A valid value for each required argument the swept entries take.
_REQUIRED: dict[str, Any] = {
    "samples": _TONE,
    "onset_envelope": np.abs(_TONE[:2048]),
    "mel": np.ones((128, 4), np.float32),
    "coefficients": np.ones((20, 4), np.float32),
    "values": np.ones((8, 4), np.float32),
    "tempogram": np.ones((384, 4), np.float32),
    "times": [0.1],
    "frames": 1,
    "time_sec": 0.1,
    "intervals": np.array([[0, 100]]),
    "data": b"MThd",
    "sample_rate": 48000,
}

_OUTSIDE: tuple[object, ...] = (0, -1, -1.0, math.nan, 1e30, "nope")

# Rows whose C subject no facade call can make the library refuse as a parameter.
_UNREACHABLE = {
    ("note_targets_from_smf", "bytes"): "unreadable SMF content is InvalidFormat, with no subject",
}


def _refusals(entry: str, spelling: str) -> list[str]:
    fn = getattr(libsonare, entry)
    params = inspect.signature(fn).parameters
    args = [
        _REQUIRED[name]
        for name, p in params.items()
        if p.default is p.empty and p.kind is p.POSITIONAL_OR_KEYWORD and name != spelling
    ]
    texts = []
    for value in _OUTSIDE:
        try:
            fn(*args, **{spelling: value})
        except (SonareError, TypeError, ValueError) as exc:
            texts.append(str(exc))
    return texts


def _names(text: str, word: str) -> bool:
    """Whether ``text`` names ``word`` where a refusal puts its subject, not inside its prose."""
    return re.search(rf"(^[a-z_.]+: |unknown key |, |\{{){re.escape(word)}\b", text) is not None


@pytest.mark.parametrize(("entry", "subject", "spelling"), [(*k, v) for k, v in OVERRIDES.items()])
def test_a_table_row_is_refused_under_the_callers_spelling(
    entry: str, subject: str, spelling: str
) -> None:
    assert spelling in inspect.signature(getattr(libsonare, entry)).parameters
    texts = _refusals(entry, spelling)
    leaked = [t for t in texts if _names(t, subject)]
    assert leaked == []
    reached = any(_names(t, spelling) for t in texts)
    assert reached is ((entry, subject) not in _UNREACHABLE), texts
