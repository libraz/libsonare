"""A count the facade derives from an array is refused under that array's name.

The C ABI takes such a count beside its array, so the core names the count; the caller
never wrote it, and the refusal names the array's length instead. A count inside a
flattened C struct reaches the same spelling once its struct root is dropped.
"""

from __future__ import annotations

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError


def _refusal(call) -> str:
    with pytest.raises(SonareValueError) as raised:
        call()
    return str(raised.value)


def test_a_lane_list_past_the_ceiling_names_the_lanes_length() -> None:
    engine = ls.RealtimeEngine(48000, 256)
    message = _refusal(lambda: engine.set_track_lanes([{"track_id": i} for i in range(1, 34)]))
    assert message == ("RealtimeEngine.set_track_lanes: lanes length must be an integer in [0, 32]")


def test_an_oversized_sysex_names_the_data_length() -> None:
    engine = ls.RealtimeEngine(48000, 256)
    message = _refusal(lambda: engine.push_midi_sysex(0, bytes([0xF0] + [0] * 1024 + [0xF7])))
    assert message.startswith("RealtimeEngine.push_midi_sysex: data length must be ")
    assert "size" not in message


@pytest.mark.parametrize("field", ["band_absorption", "band_scattering"])
def test_a_band_list_past_the_ceiling_names_its_length_on_both_room_entries(field) -> None:
    bands = {field: [0.2] * 65}
    rir = _refusal(lambda: ls.synthesize_rir(sample_rate=16000, **bands))
    morph = _refusal(
        lambda: ls.room_morph(np.zeros(1600, np.float32), 16000, 7.0, 5.0, 3.0, **bands)
    )
    for entry, message in (("synthesize_rir", rir), ("room_morph", morph)):
        assert message.startswith(f"{entry}: {field} length must be ")
        assert "config." not in message


def test_a_flattened_struct_member_drops_its_c_root() -> None:
    message = _refusal(lambda: ls.synthesize_rir(sample_rate=16000, ism_order=-1))
    assert message == "synthesize_rir: ism_order must be an integer in [0, 2147483647]"
