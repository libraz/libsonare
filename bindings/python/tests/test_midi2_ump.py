"""MIDI 2.0 event builders and raw UMP pushes."""

from __future__ import annotations

import pytest

from libsonare import ErrorCode, Project, RealtimeEngine, SonareError, SonareValueError

# (builder, keyword arguments, expected (word0, word1)); every case is computed
# by hand from the MT 0x4 channel-voice layout.
BUILDER_CASES = [
    (
        Project.midi2_note_on,
        dict(
            group=1,
            channel=2,
            note=0x3C,
            velocity16=0xFFFF,
            attribute_type=3,
            attribute_data=0x1234,
        ),
        (0x41923C03, 0xFFFF1234),
    ),
    (
        Project.midi2_note_on,
        dict(group=0, channel=0, note=0x40, velocity16=0),
        (0x40904000, 0x00000000),
    ),
    (
        Project.midi2_note_off,
        dict(group=0, channel=0, note=0x40, velocity16=0x8000),
        (0x40804000, 0x80000000),
    ),
    (
        Project.midi2_cc,
        dict(group=0, channel=3, controller=7, value32=0xDEADBEEF),
        (0x40B30700, 0xDEADBEEF),
    ),
    (
        Project.midi2_poly_pressure,
        dict(group=2, channel=1, note=0x45, pressure32=0x11223344),
        (0x42A14500, 0x11223344),
    ),
    (
        Project.midi2_channel_pressure,
        dict(group=0, channel=5, pressure32=0xCAFEBABE),
        (0x40D50000, 0xCAFEBABE),
    ),
    (
        Project.midi2_pitch_bend,
        dict(group=0, channel=9, bend32=0x80000000),
        (0x40E90000, 0x80000000),
    ),
    (
        Project.midi2_program,
        dict(
            group=0,
            channel=4,
            program=5,
            bank_valid=True,
            bank_msb=0x10,
            bank_lsb=0x20,
        ),
        (0x40C40001, 0x05001020),
    ),
    (
        Project.midi2_program,
        dict(group=0, channel=4, program=5),
        (0x40C40000, 0x05000000),
    ),
    (
        Project.midi2_registered_controller,
        dict(group=0, channel=1, bank=0x12, index=0x34, value32=0x01020304),
        (0x40211234, 0x01020304),
    ),
    (
        Project.midi2_assignable_controller,
        dict(group=0, channel=1, bank=0x12, index=0x34, value32=0x01020304),
        (0x40311234, 0x01020304),
    ),
    (
        Project.midi2_relative_registered_controller,
        dict(group=0, channel=1, bank=0x12, index=0x34, delta32=-1),
        (0x40411234, 0xFFFFFFFF),
    ),
    (
        Project.midi2_relative_assignable_controller,
        dict(group=0, channel=1, bank=0x12, index=0x34, delta32=16),
        (0x40511234, 0x00000010),
    ),
    (
        Project.midi2_registered_per_note_controller,
        dict(group=0, channel=1, note=0x3C, index=0x03, value32=0xA5A5A5A5),
        (0x40013C03, 0xA5A5A5A5),
    ),
    (
        Project.midi2_assignable_per_note_controller,
        dict(group=0, channel=1, note=0x3C, index=0xFF, value32=0x5A5A5A5A),
        (0x40113CFF, 0x5A5A5A5A),
    ),
    (
        Project.midi2_per_note_pitch_bend,
        dict(group=0, channel=1, note=0x3C, bend32=0x80000000),
        (0x40613C00, 0x80000000),
    ),
    (
        Project.midi2_per_note_management,
        dict(group=0, channel=1, note=0x3C, detach=True),
        (0x40F13C02, 0x00000000),
    ),
    (
        Project.midi2_per_note_management,
        dict(group=0, channel=1, note=0x3C, detach=True, reset=True),
        (0x40F13C03, 0x00000000),
    ),
]


@pytest.mark.parametrize(("builder", "kwargs", "words"), BUILDER_CASES)
def test_midi2_builder_packs_expected_words(builder, kwargs, words) -> None:
    ppq, data0, data1 = builder(ppq=1.5, **kwargs)
    assert ppq == 1.5
    assert (data0, data1) == words


@pytest.mark.parametrize(
    "call",
    [
        lambda: Project.midi2_cc(0.0, 16, 0, 7, 0),
        lambda: Project.midi2_cc(0.0, 0, 16, 7, 0),
        lambda: Project.midi2_cc(0.0, 0, 0, 128, 0),
        lambda: Project.midi2_cc(0.0, 0, 0, 7, 1 << 32),
        lambda: Project.midi2_cc(0.0, 0, 0, 7, -1),
        lambda: Project.midi2_cc(float("nan"), 0, 0, 7, 0),
        lambda: Project.midi2_note_on(0.0, 0, 0, 60, 1 << 16),
        lambda: Project.midi2_relative_registered_controller(0.0, 0, 0, 0, 0, 1 << 31),
        lambda: Project.midi2_registered_per_note_controller(0.0, 0, 0, 60, 256, 0),
    ],
)
def test_midi2_builder_rejects_out_of_range(call) -> None:
    with pytest.raises(SonareError):
        call()


def test_push_midi_ump_accepts_two_word_channel_voice() -> None:
    _, w0, w1 = Project.midi2_note_on(0.0, 0, 0, 60, 0xC000)
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(destination_id=0)
        engine.push_midi_ump(0, [w0, w1])
        engine.push_midi_ump(0, (w0, w1), render_frame=-1)
        engine.push_midi_ump(0, [0x20903C64])


@pytest.mark.parametrize(
    "words",
    [
        [0x30160000, 0x00000000],  # MT 0x3 SysEx7
        [0x50000000, 0x00000000, 0x00000000, 0x00000000],  # MT 0x5
        [0x40903C00],  # MT 0x4 needs two words
        [0x20903C64, 0x00000000],  # MT 0x2 needs one word
        [],
        [0x40903C00, 0, 0, 0, 0],
    ],
)
def test_push_midi_ump_rejects_bad_messages(words) -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(destination_id=0)
        with pytest.raises(SonareError) as excinfo:
            engine.push_midi_ump(0, words)
        assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_push_midi_ump_rejects_word_out_of_range() -> None:
    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine,
        pytest.raises(SonareValueError),
    ):
        engine.push_midi_ump(0, [1 << 32])


def test_push_midi_input_ump_queues_messages() -> None:
    _, w0, w1 = Project.midi2_cc(0.0, 0, 0, 1, 0x80000000)
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(destination_id=0)
        with pytest.raises(SonareError):
            engine.push_midi_input_ump([w0, w1], 0)
        engine.set_midi_input_source(0)
        engine.push_midi_input_ump([w0, w1], 0)
        engine.push_midi_input_ump([0x20903C64], port_time_samples=16)
        assert engine.midi_input_pending_count() == 2
        with pytest.raises(SonareError):
            engine.push_midi_input_ump([0x30160000, 0], 0)
        with pytest.raises(SonareError):
            engine.push_midi_input_ump([w0], 0)
