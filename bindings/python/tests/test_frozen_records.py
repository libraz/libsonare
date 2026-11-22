"""Records the library hands back are frozen; a changed copy comes from dataclasses.replace."""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

import libsonare

_RETURNED_NOTE = {
    "onset_sample": 0,
    "offset_sample": 1,
    "frame_start": 0,
    "frame_end": 1,
    "median_hz": None,
    "median_cents": 0.0,
    "f0_stability": 0.0,
    "amplitude": np.zeros(0, dtype=np.float32),
}
_RETURNED_EVENT = {
    "onset_sample": 0,
    "offset_sample": 1,
    "strength": 0.0,
    "peak_amplitude": 0.0,
    "percussive_ratio": 0.0,
}

_RECORDS = {
    "NoteObject": _RETURNED_NOTE,
    "NoteObjectInput": {"onset_sample": 0, "offset_sample": 1},
    "NoteSetEntry": {"frame_start": 0, "frame_end": 1},
    "NoteTarget": {"start_sec": 0.0, "end_sec": 1.0, "target_midi": 60.0},
    "PercussiveEvent": _RETURNED_EVENT,
    "PercussiveEventInput": {"onset_sample": 0, "offset_sample": 1},
    "PitchDecompositionResult": {},
}


@pytest.mark.parametrize("name", sorted(_RECORDS))
def test_records_refuse_assignment_and_copy_through_replace(name: str) -> None:
    cls = getattr(libsonare, name)
    assert cls.__dataclass_params__.frozen
    record = cls(**_RECORDS[name])
    field = dataclasses.fields(record)[0].name
    with pytest.raises(dataclasses.FrozenInstanceError):
        setattr(record, field, getattr(record, field))
    assert dataclasses.replace(record) is not record


def test_voice_changer_config_is_frozen() -> None:
    config = libsonare.RealtimeVoiceChangerConfig()
    with pytest.raises(dataclasses.FrozenInstanceError):
        config.wet_mix = 0.5  # type: ignore[misc]
    assert dataclasses.replace(config, wet_mix=0.5).wet_mix == 0.5


def test_the_pending_edit_stays_editable_in_place() -> None:
    note = libsonare.NoteObject(**_RETURNED_NOTE)
    note.edit.gain_db = -6.0
    assert note.edit.gain_db == -6.0
