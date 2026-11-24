"""Project MIDI tests: SMF round-trips and import, MIDI helper tables and MIDI FX baking."""

from __future__ import annotations

import ctypes
import dataclasses
import json
import re

import numpy as np
import pytest

import libsonare as ls
from libsonare import (
    AutomationPoint,
    ErrorCode,
    MidiCcBinding,
    Project,
    ProjectMidiEvent,
    SonareError,
    SonareValueError,
)
from libsonare._ffi_types_mastering_project import SonareMidiEventPod
from libsonare._project_model import _midi_event_from_c

from ._helpers import raises_coded
from ._project_helpers import _build_project


def _make_sysex_smf() -> bytes:
    payload = bytes([0x7E, 0x7F, 0x09, 0x01, 0xF7])
    body = bytearray()
    body.extend([0x00, 0xF0, len(payload)])
    body.extend(payload)
    body.extend([0x00, 0x90, 0x3C, 0x40, 0x83, 0x60, 0x80, 0x3C, 0x00])
    body.extend([0x00, 0xFF, 0x2F, 0x00])
    smf = bytearray()
    smf.extend(b"MThd")
    smf.extend((6).to_bytes(4, "big"))
    smf.extend((0).to_bytes(2, "big"))
    smf.extend((1).to_bytes(2, "big"))
    smf.extend((480).to_bytes(2, "big"))
    smf.extend(b"MTrk")
    smf.extend(len(body).to_bytes(4, "big"))
    smf.extend(body)
    return bytes(smf)


def _make_truncated_smf() -> bytes:
    body = bytes(
        [
            0x00,
            0x90,
            0x3C,
            0x64,  # valid note-on
            0x00,
            0xFF,
            0x01,
            0x7F,  # text meta claims 127 missing payload bytes
        ]
    )
    return (
        b"MThd"
        + (6).to_bytes(4, "big")
        + (0).to_bytes(2, "big")
        + (1).to_bytes(2, "big")
        + (480).to_bytes(2, "big")
        + b"MTrk"
        + len(body).to_bytes(4, "big")
        + body
    )


def _make_key_signature_smf() -> bytes:
    """Build a one-track SMF carrying a key-signature meta event (FF 59 02 sf mi).

    ``sf = 1`` (one sharp) and ``mi = 0`` (major) describe G major; a short note
    follows so the track is non-empty before End-of-Track.
    """
    body = bytearray()
    body.extend([0x00, 0xFF, 0x59, 0x02, 0x01, 0x00])
    body.extend([0x00, 0x90, 0x3C, 0x40, 0x83, 0x60, 0x80, 0x3C, 0x00])
    body.extend([0x00, 0xFF, 0x2F, 0x00])
    smf = bytearray()
    smf.extend(b"MThd")
    smf.extend((6).to_bytes(4, "big"))
    smf.extend((0).to_bytes(2, "big"))
    smf.extend((1).to_bytes(2, "big"))
    smf.extend((480).to_bytes(2, "big"))
    smf.extend(b"MTrk")
    smf.extend(len(body).to_bytes(4, "big"))
    smf.extend(body)
    return bytes(smf)


# --- SMF round-trip --------------------------------------------------------


def test_export_smf_returns_bytes() -> None:
    project, *_ = _build_project()
    try:
        data = project.export_smf()
        assert isinstance(data, bytes)
        assert data  # tempo map + MIDI structure present
        assert data[:4] == b"MThd"
    finally:
        project.close()


def test_midi_loop_export_rejects_expansion_above_budget() -> None:
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_midi_events(clip_id, [ls.midi_note_on(0.0, 0, 0, 60, 100)])
        project.set_clip_loop(clip_id, "loop", 1e-9)
        with pytest.raises(SonareError):
            project.export_smf()
        with pytest.raises(SonareError):
            project.export_clip_file()
    finally:
        project.close()


def test_clip_file_round_trips_midi_2_losslessly() -> None:
    project, _audio_clip, _midi_track, midi_clip = _build_project()
    try:
        # A MIDI 2.0 note-on (message type 0x4) whose 16-bit velocity (0xBEEF)
        # would be truncated through MIDI 1.0 SMF.
        note_on = ProjectMidiEvent(0.0, 0x40903C00, 0xBEEF0000)
        note_off = ProjectMidiEvent(1.0, 0x40803C00, 0x00000000)
        project.set_midi_events(midi_clip, [note_on, note_off])

        data = project.export_clip_file()
        assert isinstance(data, bytes)
        assert data[:8] == b"SMF2CLIP"

        # Re-import through the binding into a fresh project.
        reimported = Project()
        try:
            first_clip = reimported.import_clip_file(data)
            assert first_clip != 0
            # Export -> import -> export is deterministic for the same content.
            assert reimported.export_clip_file()[:8] == b"SMF2CLIP"
        finally:
            reimported.close()
    finally:
        project.close()


def test_midi_helpers_program_midi_fx_and_sysex_smf_round_trip() -> None:
    project, _audio_clip, _midi_track, midi_clip = _build_project()
    try:
        project.set_midi_events(
            midi_clip,
            [
                ls.midi_note_on(0.1, 0, 0, 60, 100),
                ls.midi_poly_pressure(0.2, 0, 0, 60, 70),
                ls.midi_channel_pressure(0.3, 0, 0, 80),
                ls.midi_pitch_bend(0.4, 0, 0, 8192),
                ls.midi_note_off(1.1, 0, 0, 60, 0),
            ],
        )
        project.bake_midi_fx(
            midi_clip,
            {
                "transposeSemitones": 12,
                "quantizePpq": 0.25,
                "quantizeStrength": 1.0,
                "velocityScale": 0.5,
            },
        )
        project.set_program(midi_clip, program=42)
        project.set_program(midi_clip, 24, bank_msb=2, bank_lsb=35, group=0, channel=3)
        exported = project.export_smf()
        assert exported[:4] == b"MThd"
        assert bytes([0xC0, 42]) in exported
        assert bytes([0xC3, 24]) in exported
        # Pressure and bend survive the export. Poly pressure carries the
        # transposed note (60 + 12), because aftertouch left on the source note
        # would address a note the shifted voice never played.
        assert bytes([0xA0, 72, 70]) in exported
        assert bytes([0xA0, 60, 70]) not in exported
        assert bytes([0xD0, 80]) in exported
        assert bytes([0xE0, 0x00, 0x40]) in exported

        sysex_project = Project()
        try:
            first_clip = sysex_project.import_smf(_make_sysex_smf())
            assert first_clip != 0
            json_before = sysex_project.to_json()
            assert "__sysexPayloads" in json_before
            restored = Project.from_json(json_before)
            try:
                assert bytes([0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7]) in restored.export_smf()
            finally:
                restored.close()
        finally:
            sysex_project.close()
    finally:
        project.close()


def test_validate_midi_notes_flags_hanging_note_on() -> None:
    """A well-paired clip validates ``ok``; a lone note-on is flagged hanging."""
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 4.0).clip_id

        # Well-paired note-on / note-off: nothing hanging.
        project.set_midi_events(
            clip_id,
            [
                ls.midi_note_on(0.0, 0, 0, 60, 100),
                ls.midi_note_off(2.0, 0, 0, 60, 0),
            ],
        )
        paired = project.validate_midi_notes(clip_id)
        assert paired.ok is True
        assert paired.unmatched_note_ons == 0
        assert paired.unmatched_note_offs == 0

        # A single hanging note-on (no matching note-off).
        project.set_midi_events(
            clip_id,
            [ls.midi_note_on(0.0, 0, 0, 60, 100)],
        )
        hanging = project.validate_midi_notes(clip_id)
        assert hanging.ok is False
        assert hanging.unmatched_note_ons == 1
        assert hanging.unmatched_note_offs == 0

        # The clip window is half-open [source_offset, source_offset + length), so
        # an off exactly at length_ppq lies outside it; export closes the note at
        # the clip end, so it validates as a matched pair.
        boundary_clip = project.add_midi_clip(0.0, 1.0).clip_id
        project.set_midi_events(
            boundary_clip,
            [
                ls.midi_note_on(0.0, 0, 0, 61, 100),
                ls.midi_note_off(1.0, 0, 0, 61, 0),
            ],
        )
        boundary = project.validate_midi_notes(boundary_clip)
        assert boundary.ok is True
        assert boundary.unmatched_note_ons == 0
        assert boundary.unmatched_note_offs == 0
    finally:
        project.close()


def test_set_midi_events_validates_event_shape_and_words() -> None:
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 1.0).clip_id
        with pytest.raises(ValueError, match="ppq"):
            project.set_midi_events(clip_id, [ProjectMidiEvent(float("nan"), 0)])
        with pytest.raises(ValueError, match=r"events\[0\]\.word0"):
            project.set_midi_events(clip_id, [ProjectMidiEvent(0.0, -1)])
        with pytest.raises(ValueError, match=r"events\[1\]\.word1"):
            project.set_midi_events(
                clip_id, np.array([[0.0, 0x20903C40, 0], [1.0, 0x20803C00, -1]])
            )
        with pytest.raises(ValueError, match=r"\(n, 3\)"):
            project.set_midi_events(clip_id, np.zeros((2, 2)))
    finally:
        project.close()


def test_a_midi_event_is_taken_only_as_a_project_midi_event() -> None:
    on = (0.0, 0x20903C40, 0)
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 1.0).clip_id
        with pytest.raises(TypeError, match="ProjectMidiEvent"):
            project.set_midi_events(clip_id, [on])  # type: ignore[list-item]
        with pytest.raises(TypeError, match="ProjectMidiEvent"):
            project.set_midi_events(clip_id, [{"ppq": 0.0, "word0": 0x20903C40}])  # type: ignore[list-item]
    finally:
        project.close()
    with pytest.raises(TypeError, match="ProjectMidiEvent"):
        ls.midi_route_events([on])  # type: ignore[list-item]
    with pytest.raises(TypeError, match="ProjectMidiEvent"):
        ls.midi_cc_learn([on], 1)  # type: ignore[list-item]
    with pytest.raises(TypeError, match="ProjectMidiEvent"):
        ls.midi_cc_to_breakpoint([], on)  # type: ignore[arg-type]


def test_midi_events_come_back_as_project_midi_event_records() -> None:
    on = ls.midi_note_on(1.5, 0, 0, 60, 100)
    assert on == ProjectMidiEvent(1.5, on.word0, on.word1)
    assert (on.word0 >> 20) & 0xF == 0x9  # note-on status in the first UMP word
    with pytest.raises(TypeError):
        iter(on)  # type: ignore[call-overload]
    with pytest.raises(dataclasses.FrozenInstanceError):
        on.ppq = 0.0  # type: ignore[misc]
    assert ProjectMidiEvent(0.0, 1).word1 == 0
    routed = ls.midi_route_events([on]).events
    assert routed == [on]
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 4.0).clip_id
        events = [on, ls.midi_note_off(2.0, 0, 0, 60, 0)]
        project.set_midi_events(clip_id, events)
        assert project.midi_events(clip_id) == events
        as_array = np.array([[e.ppq, e.word0, e.word1] for e in events])
        project.set_midi_events(clip_id, as_array)
    finally:
        project.close()


def test_project_midi_events_round_trip_owned_sysex_in_stable_order() -> None:
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 4.0).clip_id
        payload = b"\xf0\x7e\x00\xff\xf7"
        events = [
            ProjectMidiEvent(1.0, 0x20903C60),
            ProjectMidiEvent(0.5, 0x30000000, 0, payload),
            ProjectMidiEvent(0.5, 0x20904060),
        ]
        project.set_midi_events(clip_id, events)
        observed = project.midi_events(clip_id)
        assert observed == [events[1], events[2], events[0]]
        assert observed[0].sysex == payload
        project.set_midi_events(clip_id, observed)
        assert project.midi_events(clip_id) == observed
    finally:
        project.close()
    assert observed[0].sysex == payload


def test_project_midi_events_refuses_empty_sysex_and_wrong_payload_type() -> None:
    project = Project()
    try:
        clip_id = project.add_midi_clip(0.0, 1.0).clip_id
        with pytest.raises(SonareValueError, match=r"events\[0\]\.sysex"):
            project.set_midi_events(clip_id, [ProjectMidiEvent(0.0, 0x30000000, sysex=b"")])
        with pytest.raises(TypeError, match=r"events\[0\]\.sysex"):
            project.set_midi_events(
                clip_id,
                [ProjectMidiEvent(0.0, 0x30000000, sysex=bytearray(b"\xf0"))],  # type: ignore[arg-type]
            )
    finally:
        project.close()


def test_project_midi_events_refuses_non_midi_and_missing_clips() -> None:
    project, audio_clip, _midi_track, _midi_clip = _build_project()
    try:
        with pytest.raises(SonareValueError, match="clip_id"):
            project.midi_events(audio_clip)
        with pytest.raises(SonareValueError, match="clip_id"):
            project.midi_events(999_999)
    finally:
        project.close()


def test_midi_event_reader_rejects_empty_pointed_sysex_as_malformed_output() -> None:
    payload = (ctypes.c_uint8 * 1)(0xF0)
    event = SonareMidiEventPod()
    event.word0 = 0x30000000
    event.sysex = ctypes.cast(payload, ctypes.POINTER(ctypes.c_uint8))
    event.sysex_size = 0
    with pytest.raises(SonareError) as exc:
        _midi_event_from_c(event)
    assert exc.value.code == ErrorCode.INVALID_FORMAT


def test_midi_builders_are_module_functions() -> None:
    builders = {
        "midi_note_on": (0.0, 0, 0, 60, 100),
        "midi_note_off": (0.0, 0, 0, 60, 0),
        "midi_cc": (0.0, 0, 0, 1, 64),
        "midi_poly_pressure": (0.0, 0, 0, 60, 64),
        "midi_program": (0.0, 0, 0, 5),
        "midi_channel_pressure": (0.0, 0, 0, 64),
        "midi_pitch_bend": (0.0, 0, 0, 8192),
        "midi2_note_on": (0.0, 0, 0, 60, 0x8000),
        "midi2_note_off": (0.0, 0, 0, 60),
        "midi2_cc": (0.0, 0, 0, 1, 0x80000000),
        "midi2_poly_pressure": (0.0, 0, 0, 60, 0x80000000),
        "midi2_channel_pressure": (0.0, 0, 0, 0x80000000),
        "midi2_pitch_bend": (0.0, 0, 0, 0x80000000),
        "midi2_program": (0.0, 0, 0, 5),
        "midi2_registered_controller": (0.0, 0, 0, 0, 0, 0),
        "midi2_assignable_controller": (0.0, 0, 0, 0, 0, 0),
        "midi2_relative_registered_controller": (0.0, 0, 0, 0, 0, 0),
        "midi2_relative_assignable_controller": (0.0, 0, 0, 0, 0, 0),
        "midi2_registered_per_note_controller": (0.0, 0, 0, 60, 0, 0),
        "midi2_assignable_per_note_controller": (0.0, 0, 0, 60, 0, 0),
        "midi2_per_note_pitch_bend": (0.0, 0, 0, 60, 0),
        "midi2_per_note_management": (0.0, 0, 0, 60),
    }
    for name, args in builders.items():
        builder = getattr(ls, name)
        assert name in ls.__all__
        assert not hasattr(Project, name)
        assert isinstance(builder(*args), ProjectMidiEvent)

    bank_events = ls.midi_bank_program(0.0, 0, 0, 0, 0, 5)
    assert "midi_bank_program" in ls.__all__
    assert not hasattr(Project, "midi_bank_program")
    assert bank_events
    assert all(isinstance(event, ProjectMidiEvent) for event in bank_events)


def test_midi_catalog_and_conversion_helpers_are_module_functions() -> None:
    names = (
        "gm_instrument_name",
        "gm_program_for_name",
        "gm_family_name",
        "gm_family_first_program",
        "gm2_instrument_name",
        "gm_drum_name",
        "gm_drum_note_for_name",
        "gm2_drum_set_name",
        "gm2_drum_name",
        "midi_cc_name",
        "midi_cc_index_for_name",
        "per_note_controller_name",
        "midi_route_events",
        "midi_cc_learn",
        "midi_cc_to_breakpoint",
        "midi_param_to_cc",
    )
    for name in names:
        assert name in ls.__all__
        assert callable(getattr(ls, name))
        assert not hasattr(Project, name)


# ---------------------------------------------------------------------------
# Parity surface added for Node/WASM cross-binding coverage: MIDI helper
# tables (ProgramMap / MidiRouter / CcMap) and the bake / last-bounce-compile-result
# accessors.
# ---------------------------------------------------------------------------


def test_gm_program_and_cc_name_tables_round_trip() -> None:
    assert ls.gm_instrument_name(0) == "Acoustic Grand Piano"
    assert ls.gm_program_for_name("Acoustic Grand Piano") == 0
    assert isinstance(ls.gm_family_name(0), str)
    assert ls.gm_family_first_program(0) >= 0
    assert isinstance(ls.gm2_instrument_name(0, 0), (str, type(None)))
    assert isinstance(ls.gm_drum_name(35), (str, type(None)))
    assert isinstance(ls.gm2_drum_set_name(0), (str, type(None)))
    assert isinstance(ls.gm2_drum_name(0, 35), (str, type(None)))
    assert ls.midi_cc_name(7) is not None
    assert ls.midi_cc_index_for_name(ls.midi_cc_name(7)) == 7
    assert isinstance(ls.per_note_controller_name(0), (str, type(None)))


@pytest.mark.parametrize(
    ("lookup", "args", "key", "high"),
    [
        (ls.gm_instrument_name, (128,), "program", 127),
        (ls.gm_family_name, (16,), "family", 15),
        (ls.gm_family_first_program, (-1,), "family", 15),
        (ls.gm2_instrument_name, (128, 0), "bank_lsb", 127),
        (ls.gm2_instrument_name, (0, -1), "program", 127),
        (ls.gm_drum_name, (128,), "note", 127),
        (ls.gm2_drum_set_name, (-1,), "bank_lsb", 127),
        (ls.gm2_drum_name, (0, 128), "note", 127),
        (ls.midi_cc_name, (128,), "controller", 127),
        (ls.per_note_controller_name, (256,), "index", 255),
    ],
)
def test_a_gm_lookup_refuses_a_number_outside_its_domain(lookup, args, key, high) -> None:
    with pytest.raises(SonareValueError, match=rf": {key} must be an integer in \[0, {high}\]$"):
        lookup(*args)


def test_a_gm_lookup_answers_none_for_an_in_domain_number_that_names_nothing() -> None:
    assert ls.gm_drum_name(34) is None
    assert ls.gm_drum_name(127) is None
    assert ls.per_note_controller_name(200) is None
    assert all(isinstance(ls.gm_instrument_name(p), str) for p in range(128))
    assert all(isinstance(ls.gm_family_name(f), str) for f in range(16))


def test_a_gm_lookup_refuses_a_non_integer_by_type() -> None:
    with pytest.raises(TypeError, match=r": note must be an integer$"):
        ls.gm_drum_name("36")  # type: ignore[arg-type]
    with pytest.raises(SonareValueError, match=r": note must be an integer in \[0, 127\]$"):
        ls.gm_drum_name(36.5)  # type: ignore[arg-type]


@pytest.mark.parametrize(
    ("lookup", "argument"),
    [
        (ls.gm_program_for_name, "not a real instrument"),
        (ls.gm_drum_note_for_name, "not a drum"),
        (ls.midi_cc_index_for_name, "not a cc"),
        (ls.gm_family_first_program, 16),
        (ls.gm_family_first_program, -1),
    ],
)
def test_a_reverse_lookup_that_finds_nothing_is_refused(lookup, argument) -> None:
    with pytest.raises(SonareValueError, match=lookup.__name__):
        lookup(argument)


def test_midi_bank_program_lowers_to_events() -> None:
    events = ls.midi_bank_program(0.0, 0, 0, 0, 0, 5)
    assert isinstance(events, list)
    assert 1 <= len(events) <= 3
    assert all(isinstance(ev, ProjectMidiEvent) and ev.ppq == 0.0 for ev in events)


def test_midi_route_events_filters_and_reports_overflow() -> None:
    events = [ls.midi_cc(0.0, 0, 2, 1, 64), ls.midi_cc(0.1, 0, 5, 1, 20)]
    result = ls.midi_route_events(events, {"filter_channel": 2})
    assert result.overflowed is False
    assert isinstance(result.overflow_count, int)
    # Only the channel-2 event survives the filter.
    assert len(result.events) == 1


def test_midi_route_absent_filters_mean_any_and_a_negative_is_refused() -> None:
    events = [ls.midi_cc(0.0, 0, 2, 1, 64), ls.midi_cc(0.1, 0, 5, 1, 20)]
    unfiltered = ls.midi_route_events(events, {"filter_channel": None, "remap_channel": None})
    assert len(unfiltered.events) == 2
    for key in ("filter_group", "filter_channel", "remap_channel"):
        with pytest.raises(SonareValueError, match=key):
            ls.midi_route_events(events, {key: -1})
        with pytest.raises(SonareValueError, match=key):
            ls.midi_route_events(events, {key: 16})


def test_midi_route_preserves_sysex_and_absent_payloads() -> None:
    payload = bytes([0xF0, 0x00, 0x7D, 0xF7])
    events = [
        ProjectMidiEvent(0.0, 0x33000000, sysex=payload),
        ls.midi_note_on(1.0, 3, 0, 60, 100),
    ]
    result = ls.midi_route_events(events, {"filter_group": 3})
    assert result.events == events
    assert result.events[1].sysex is None
    assert not result.overflowed
    assert result.overflow_count == 0


def test_midi_cc_to_breakpoint_reports_valid_sysex_as_not_supported() -> None:
    binding = MidiCcBinding(
        cc_number=1,
        channel=None,
        kind="controlChange7",
        cc_lsb_number=0,
        selector_msb=0,
        selector_lsb=0,
        param_id=5,
        min_value=0.0,
        max_value=1.0,
    )
    event = ProjectMidiEvent(
        0.0,
        0x33000000,
        sysex=bytes([0xF0, 0x00, 0x7D, 0xF7]),
    )

    with pytest.raises(SonareError) as caught:
        ls.midi_cc_to_breakpoint([binding], event)
    assert caught.value.code == ErrorCode.NOT_SUPPORTED
    assert str(caught.value) == (
        "[6] midi_cc_to_breakpoint: MIDI CC conversion does not support SysEx events"
    )

    wrong_shape = dataclasses.replace(
        event,
        sysex=[0xF0, 0x00, 0xF7],  # type: ignore[arg-type]
    )
    invalid_binding = dataclasses.replace(binding, cc_lsb_number=128)
    with pytest.raises(
        TypeError, match=r"^midi_cc_to_breakpoint: event\.sysex must be bytes or None$"
    ):
        ls.midi_cc_to_breakpoint([invalid_binding], wrong_shape)


def test_cc_refuses_wrong_sysex_shape_before_a_domain_error() -> None:
    event = ProjectMidiEvent(
        0.0,
        0x30000000,
        sysex=[0xF0, 0x00, 0xF7],  # type: ignore[arg-type]
    )
    with pytest.raises(
        TypeError, match=r"^midi_cc_learn: events\[0\]\.sysex must be bytes or None$"
    ):
        ls.midi_cc_learn([event], param_id=1, min_movement=128)
    first = ProjectMidiEvent(0.0, -1)
    with pytest.raises(
        TypeError, match=r"^midi_cc_learn: events\[1\]\.sysex must be bytes or None$"
    ):
        ls.midi_cc_learn([first, event], param_id=1)


def test_a_cc_binding_channel_is_absent_for_any_and_0xff_is_refused() -> None:
    event = ls.midi_cc(0.0, 0, 9, 1, 100)
    any_channel = MidiCcBinding(
        cc_number=1,
        channel=None,
        kind="controlChange7",
        cc_lsb_number=0,
        selector_msb=0,
        selector_lsb=0,
        param_id=5,
        min_value=0.0,
        max_value=1.0,
    )
    assert ls.midi_cc_to_breakpoint([any_channel], event) is not None
    with pytest.raises(SonareValueError, match="channel"):
        ls.midi_cc_to_breakpoint([dataclasses.replace(any_channel, channel=0xFF)], event)


def test_a_cc_binding_is_taken_only_as_a_record_with_a_kind_token() -> None:
    event = ls.midi_cc(0.0, 0, 9, 1, 100)
    binding = MidiCcBinding(
        cc_number=1,
        channel=None,
        kind="controlChange7",
        cc_lsb_number=0,
        selector_msb=0,
        selector_lsb=0,
        param_id=5,
        min_value=0.0,
        max_value=1.0,
    )
    with pytest.raises(TypeError, match=r"MidiCcBinding"):
        ls.midi_cc_to_breakpoint([{"cc_number": 1, "param_id": 5}], event)  # type: ignore[list-item]
    with pytest.raises(TypeError, match=r"MidiCcBinding"):
        ls.midi_param_to_cc([{"cc_number": 1, "param_id": 5}], 5, 0.5, 0)  # type: ignore[list-item]
    with pytest.raises(TypeError, match="kind"):
        ls.midi_param_to_cc([dataclasses.replace(binding, kind=0)], 5, 0.5, 0)  # type: ignore[arg-type]
    with pytest.raises(SonareValueError, match="kind"):
        ls.midi_param_to_cc([dataclasses.replace(binding, kind="cc7")], 5, 0.5, 0)  # type: ignore[arg-type]


def test_cc_readers_check_all_shapes_before_any_domain() -> None:
    domain_event = ProjectMidiEvent(0.0, -1)
    malformed_event = ProjectMidiEvent(0.0, object())  # type: ignore[arg-type]
    with pytest.raises(TypeError) as refused:
        ls.midi_cc_learn([domain_event, malformed_event], param_id=1)
    assert str(refused.value) == (
        "midi_cc_learn: events[1].word0 must be an integer in [0, 4294967295]"
    )

    with Project() as project:
        clip_id = project.add_midi_clip(0.0, 1.0).clip_id
        with pytest.raises(TypeError) as refused:
            project.set_midi_events(clip_id, [domain_event, malformed_event])
        assert str(refused.value) == (
            "Project.set_midi_events: events[1].word0 must be an integer in [0, 4294967295]"
        )

    event = ls.midi_cc(0.0, 0, 0, 1, 64)
    invalid_binding = MidiCcBinding(cc_number=128, param_id=4)
    malformed_binding = dataclasses.replace(
        MidiCcBinding(cc_number=1, param_id=5),
        min_value=object(),  # type: ignore[arg-type]
    )
    with pytest.raises(TypeError) as refused:
        ls.midi_cc_to_breakpoint([invalid_binding, malformed_binding], event)
    assert str(refused.value) == ("midi_cc_to_breakpoint: bindings[1].min_value must be a number")
    with pytest.raises(TypeError) as refused:
        ls.midi_param_to_cc([invalid_binding, malformed_binding], 4, 0.5, 0)
    assert str(refused.value) == ("midi_param_to_cc: bindings[1].min_value must be a number")

    with pytest.raises(TypeError) as refused:
        ls.midi_route_events([domain_event], {"filter_group": object()})
    assert str(refused.value) == "midi_route_events: filter_group must be an integer or None"


def test_cc_learn_refuses_a_later_sysex_after_earlier_learnable_ccs() -> None:
    events = [
        ls.midi_cc(0.0, 0, 0, 7, 0),
        ls.midi_cc(1.0, 0, 0, 7, 64),
        ProjectMidiEvent(2.0, 0x20903C40, sysex=b"\xf0\xf7"),
    ]
    with pytest.raises(SonareValueError) as refused:
        ls.midi_cc_learn(events, param_id=9)
    assert str(refused.value) == (
        "midi_cc_learn: events[2].sysex must be NULL and empty for a channel-voice event"
    )
    assert refused.value.code == ErrorCode.INVALID_PARAMETER


def test_midi_cc_learn_and_conversion_helpers() -> None:
    # A 14-bit CC pair (MSB controller 1 + LSB controller 33) learns kind 1.
    events = [ls.midi_cc(0.0, 0, 2, 1, 64), ls.midi_cc(0.1, 0, 2, 33, 12)]
    binding = ls.midi_cc_learn(events, 77, min_value=-1.0, max_value=1.0)
    assert binding is not None
    assert binding.kind == "controlChange14"
    assert binding.param_id == 77
    assert binding.min_value == -1.0
    assert binding.consume is False

    # RPN selector assembly learns kind 2.
    rpn = [
        ls.midi_cc(0.0, 0, 3, 101, 0),
        ls.midi_cc(0.1, 0, 3, 100, 1),
        ls.midi_cc(0.2, 0, 3, 6, 64),
    ]
    rpn_binding = ls.midi_cc_learn(rpn, 78)
    assert rpn_binding is not None
    assert rpn_binding.kind == "rpn"
    assert rpn_binding.consume is False

    # No learnable CC stream -> None sentinel (INVALID_STATE), not an exception.
    assert ls.midi_cc_learn([], 99) is None

    # cc_to_breakpoint converts a matching CC to an automation point.
    cc = ls.midi_cc(1.0, 0, 2, 1, 100)
    point = ls.midi_cc_to_breakpoint([binding], cc)
    assert isinstance(point, AutomationPoint)
    assert point.ppq == 1.0
    assert point.curve_to_next in ("linear", "exponential", "hold", "sCurve")
    # param_to_cc converts a parameter value back to a CC event; an unbound id is None.
    back = ls.midi_param_to_cc([binding], 77, 0.5, 0, ppq=2.0)
    assert isinstance(back, ProjectMidiEvent)
    assert back.ppq == 2.0
    consuming = dataclasses.replace(binding, consume=True)
    assert ls.midi_cc_to_breakpoint([consuming], cc) == point
    assert ls.midi_param_to_cc([consuming], 77, 0.5, 0, ppq=2.0) == back
    assert ls.midi_param_to_cc([binding], 12345, 0.5, 0) is None


def test_project_import_smf_surfaces_key_signature_marker() -> None:
    project = Project()
    try:
        project.import_smf(_make_key_signature_smf())
        markers = project.markers()
        assert "keySignature" in [marker.kind for marker in markers]
        key = next(marker for marker in markers if marker.kind == "keySignature")
        assert key.key_fifths == 1
        assert key.key_minor is False
    finally:
        project.close()


def test_project_import_smf_replaces_invalid_utf8_before_json_serialization() -> None:
    """Imported legacy text metadata remains valid JSON through the Python facade."""
    body = bytes(
        [
            0x00,
            0xFF,
            0x03,
            0x03,
            ord("A"),
            0xFF,
            ord("B"),  # Invalid UTF-8 track name byte.
            0x00,
            0xFF,
            0x06,
            0x03,
            ord("M"),
            0xC3,
            ord("N"),  # Unterminated UTF-8 marker byte.
            0x00,
            0x90,
            0x3C,
            0x64,
            0x83,
            0x60,
            0x80,
            0x3C,
            0x00,
            0x00,
            0xFF,
            0x2F,
            0x00,
        ]
    )
    smf = b"MThd\x00\x00\x00\x06\x00\x00\x00\x01\x01\xe0MTrk" + len(body).to_bytes(4, "big") + body
    project = Project()
    try:
        project.import_smf(smf)
        payload = project.to_json()
        assert "\ufffd" in payload
        assert json.loads(payload)["markers"][0]["name"] == "M\ufffdN"
    finally:
        project.close()


def test_bake_midi_fx_and_last_bounce_compile_result() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_midi_events(
            clip,
            [ls.midi_note_on(0.0, 0, 0, 60, 100), ls.midi_note_off(2.0, 0, 0, 60, 0)],
        )
        # A MIDI-only bounce with no instrument compiles to silence but records
        # the no-instrument diagnostic, retrievable after the bounce.
        project.bounce(total_frames=48000, num_channels=2)
        result = project.last_bounce_compile_result()
        assert result is not None
        assert [d.code for d in result.diagnostics] == ["midiClipNoInstrument"]
    finally:
        project.close()


def test_bake_midi_fx_preserves_more_than_512_events() -> None:
    project = Project()
    try:
        clip = project.add_midi_clip(0.0, 8.0).clip_id
        events = [ls.midi_cc(i / 80, 0, 0, i % 128, (i * 3) % 128) for i in range(600)]
        project.set_midi_events(clip, events)
        project.bake_midi_fx(clip, {})
        payload = json.loads(project.to_json())
        baked = payload["midiContent"][str(clip)]
        assert baked == [{"word0": e.word0, "word1": e.word1, "ppq": e.ppq} for e in events]

        roundtrip = Project()
        try:
            round_clip = roundtrip.import_smf(project.export_smf())
            roundtrip.bake_midi_fx(round_clip, {})
            round_payload = json.loads(roundtrip.to_json())
            assert len(round_payload["midiContent"][str(round_clip)]) == 600
        finally:
            roundtrip.close()
    finally:
        project.close()


def test_bake_midi_fx_reports_source_index() -> None:
    def seed(project: Project, clip: int) -> None:
        project.set_midi_events(
            clip,
            [
                ls.midi_note_on(0.0, 0, 0, 60, 100),
                ls.midi_note_off(1.0, 0, 0, 60, 0),
                ls.midi_note_on(2.0, 0, 0, 64, 100),
                ls.midi_note_off(3.0, 0, 0, 64, 0),
            ],
        )

    config = {"chordIntervals": [0, 4, 7]}
    tracked = Project()
    plain = Project()
    try:
        tracked_clip = tracked.add_midi_clip(0.0, 4.0).clip_id
        seed(tracked, tracked_clip)
        assert tracked.preview_midi_fx_count(tracked_clip, config) == 12
        # The preview must leave the clip alone.
        assert len(json.loads(tracked.to_json())["midiContent"][str(tracked_clip)]) == 4

        source_index = tracked.bake_midi_fx(tracked_clip, config, with_source_index=True)
        assert isinstance(source_index, np.ndarray)
        assert source_index.dtype == np.int32
        assert len(source_index) == 12
        per_input = [0, 0, 0, 0]
        for source in source_index:
            assert 0 <= source < 4
            per_input[source] += 1
        assert per_input == [3, 3, 3, 3]

        # Without the flag the call keeps returning None and produces the same
        # bytes, so the provenance path cannot drift from the plain one.
        plain_clip = plain.add_midi_clip(0.0, 4.0).clip_id
        seed(plain, plain_clip)
        assert plain.bake_midi_fx(plain_clip, config) is None
        assert (
            json.loads(tracked.to_json())["midiContent"][str(tracked_clip)]
            == json.loads(plain.to_json())["midiContent"][str(plain_clip)]
        )

        # The config is a mapping; JSON text is the caller's to parse.
        with pytest.raises(TypeError, match="json.loads"):
            tracked.bake_midi_fx(tracked_clip, '{"chordIntervals":[0,4,7]}', with_source_index=True)  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="json.loads"):
            tracked.preview_midi_fx_count(tracked_clip, '{"chordIntervals":[0,4,7]}')  # type: ignore[arg-type]
    finally:
        tracked.close()
        plain.close()


def test_project_import_smf_preserves_salvaged_truncation() -> None:
    project = Project()
    try:
        assert project.import_smf(_make_truncated_smf()) > 0
    finally:
        project.close()


@pytest.mark.parametrize(
    ("config", "message"),
    [
        (
            {"transpose_semitones": 12},
            "unknown key config.transpose_semitones (expected ",
        ),
        ({"transposeSemitone": 12}, "unknown key config.transposeSemitone"),
        ({"velocityScale": float("nan")}, None),
        ({"velocityGamma": 0.0}, "config.velocityGamma must be > 0"),
        (
            {"quantizePpq": 240, "quantizeStrength": 2.0},
            "config.quantizeStrength must be in [0, 1]",
        ),
        ({"humanizeVelocity": 200}, "config.humanizeVelocity must be an integer in [0, 127]"),
    ],
    ids=[
        "earlier_spelling",
        "unknown_key",
        "non_finite_value",
        "velocity_gamma",
        "quantize_strength",
        "humanize_velocity",
    ],
)
def test_a_midi_fx_config_refusal_is_an_argument_refusal_naming_the_key(
    config: dict[str, object], message: str | None
) -> None:
    """The MIDI-FX config is a call's argument; a refusal names the key it read under ``config``.

    A value out of its domain is refused naming the key. A non-finite value fails the
    JSON parse, so only its code is pinned.
    """
    project = Project()
    try:
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        with raises_coded(
            ErrorCode.INVALID_PARAMETER, match=None if message is None else re.escape(message)
        ):
            project.preview_midi_fx_count(clip, config)
        with raises_coded(
            ErrorCode.INVALID_PARAMETER, match=None if message is None else re.escape(message)
        ):
            project.bake_midi_fx(clip, config)
    finally:
        project.close()


@pytest.mark.parametrize(
    ("packer", "args", "key", "domain"),
    [
        (ls.midi_note_on, (0.0, 16, 0, 60, 100), "group", "[0, 15]"),
        (ls.midi_note_on, (0.0, 0, 16, 60, 100), "channel", "[0, 15]"),
        (ls.midi_note_on, (0.0, 0, 0, 60, 128), "velocity", "[0, 127]"),
        (ls.midi_cc, (0.0, 0, 0, 7, 128), "value", "[0, 127]"),
        (ls.midi_pitch_bend, (0.0, 0, 0, 16384), "bend", "[0, 16383]"),
        (ls.midi2_registered_controller, (0.0, 0, 0, 128, 0, 0), "bank", "[0, 127]"),
        (ls.midi2_registered_controller, (0.0, 0, 0, 0, 128, 0), "index", "[0, 127]"),
        (ls.midi_bank_program, (0.0, 0, 0, 128, 0, 0), "bank_msb", "[0, 127]"),
    ],
)
def test_a_midi_packer_refuses_a_field_outside_its_width_by_name(packer, args, key, domain) -> None:
    with pytest.raises(
        SonareValueError, match=rf": {key} must be an integer in {re.escape(domain)}$"
    ):
        packer(*args)


def test_a_per_note_controller_index_takes_all_eight_bits() -> None:
    assert ls.midi2_registered_per_note_controller(0.0, 0, 0, 60, 255, 0)


def test_program_and_bank_are_refused_outside_their_widths() -> None:
    project = Project()
    try:
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_program(clip, 0, bank_msb=127, bank_lsb=127)
        for name, other in (("bank_msb", "bank_lsb"), ("bank_lsb", "bank_msb")):
            for value in (-1, 128):
                with pytest.raises(SonareValueError, match=rf"{name}.*\[0, 127\]"):
                    project.set_program(clip, 0, **{name: value, other: 0})
        with pytest.raises(SonareValueError, match=r": program must be an integer in \[0, 127\]$"):
            project.set_program(clip, 128)
        with pytest.raises(SonareValueError, match=r": program must be an integer in \[0, 127\]$"):
            project.set_program(clip, 128, group=0, channel=3)
    finally:
        project.close()


@pytest.mark.parametrize("given", [{"bank_msb": 1}, {"bank_lsb": 1}, {"bank_msb": 0}])
def test_set_program_bank_half_is_refused(given: dict[str, int]) -> None:
    project = Project()
    try:
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        with pytest.raises(SonareValueError, match=r"(?s)(?=.*bank_msb)(?=.*bank_lsb)"):
            project.set_program(clip, 5, **given)
        # The refused call left nothing behind: the clip still exports no program.
        assert bytes([0xC0, 5]) not in project.export_smf()
    finally:
        project.close()


def test_set_program_without_a_bank_emits_no_bank_select_and_with_one_emits_it() -> None:
    project = Project()
    try:
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_program(clip, 5)
        project.set_program(clip, 6, group=0, channel=2)
        plain = project.export_smf()
        assert bytes([0xC0, 5]) in plain
        assert bytes([0xC2, 6]) in plain
        assert bytes([0xB0, 0, 0]) not in plain
        assert bytes([0xB2, 0, 1]) not in plain

        project.set_program(clip, 7, bank_msb=1, bank_lsb=5, group=0, channel=2)
        banked = project.export_smf()
        assert bytes([0xC2, 7]) in banked
        assert bytes([0xB2, 0, 1]) in banked
        assert bytes([0xB2, 32, 5]) in banked
    finally:
        project.close()
