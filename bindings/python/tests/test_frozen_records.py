"""Records the library hands back are frozen; a changed copy comes from dataclasses.replace."""

from __future__ import annotations

import dataclasses

import pytest

import libsonare

_RECORDS = {
    "NoteObject": {"onset_sample": 0, "offset_sample": 1},
    "NoteTarget": {"start_sec": 0.0, "end_sec": 1.0, "target_midi": 60.0},
    "PercussiveEvent": {"onset_sample": 0, "offset_sample": 1},
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
    note = libsonare.NoteObject(onset_sample=0, offset_sample=1)
    note.edit.gain_db = -6.0
    assert note.edit.gain_db == -6.0
