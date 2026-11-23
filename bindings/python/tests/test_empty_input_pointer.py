"""An empty input reaches the C entry as a non-NULL pointer with length 0.

A NULL pointer would read as the argument being left out ("must not be None"),
which is a different refusal from the entry's own one for an empty input.
"""

from __future__ import annotations

from collections.abc import Callable

import numpy as np
import pytest

import libsonare
from libsonare import SonareError


def _project() -> libsonare.Project:
    return libsonare.Project.create()


_CASES: list[tuple[str, Callable[[], object], str]] = [
    (
        "Project.import_smf",
        lambda: _project().import_smf(b""),
        "Project.import_smf: data must not be empty",
    ),
    (
        "Project.import_clip_file",
        lambda: _project().import_clip_file(b""),
        "Project.import_clip_file: data must not be empty",
    ),
    (
        "HrtfSet.from_bytes",
        lambda: libsonare.HrtfSet.from_bytes(b""),
        "[2] HrtfSet.from_bytes: SHRF data is smaller than the 36-byte header: 0 bytes",
    ),
    (
        "note_targets_from_smf",
        lambda: libsonare.note_targets_from_smf(b""),
        "[2] note_targets_from_smf: unreadable SMF",
    ),
    (
        "normalize",
        lambda: libsonare.normalize(np.zeros(0, dtype=np.float32), 22050),
        "normalize: samples must not be empty",
    ),
]


@pytest.mark.parametrize(("name", "call", "message"), _CASES, ids=[c[0] for c in _CASES])
def test_an_empty_input_reads_the_entry_refusal_not_a_missing_argument(
    name: str, call: Callable[[], object], message: str
) -> None:
    with pytest.raises(SonareError) as caught:
        call()
    assert str(caught.value) == message
    assert "must not be None" not in str(caught.value)
