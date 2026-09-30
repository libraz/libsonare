"""Project MIDI tests: SMF round-trips and import, MIDI helper tables and MIDI FX baking."""

from __future__ import annotations

import json

import pytest

from libsonare import (
    MarkerKind,
    Project,
    SonareError,
)

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
        _track_id, clip_id = project.add_midi_clip(0.0, 4.0)
        project.set_midi_events(clip_id, [Project.midi_note_on(0.0, 0, 0, 60, 100)])
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
        note_on = (0.0, 0x40903C00, 0xBEEF0000)
        note_off = (1.0, 0x40803C00, 0x00000000)
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
                Project.midi_note_on(0.1, 0, 0, 60, 100),
                Project.midi_poly_pressure(0.2, 0, 0, 60, 70),
                Project.midi_channel_pressure(0.3, 0, 0, 80),
                Project.midi_pitch_bend(0.4, 0, 0, 8192),
                Project.midi_note_off(1.1, 0, 0, 60, 0),
            ],
        )
        project.set_midi_fx(
            midi_clip,
            '{"transpose_semitones":12,"quantize_ppq":0.25,'
            '"quantize_strength":1.0,"velocity_scale":0.5}',
        )
        project.set_program(midi_clip, program=42)
        project.set_program_on_channel(midi_clip, group=0, channel=3, program=24, bank=0x0123)
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
            assert "__sysex_payloads" in json_before
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
        _track_id, clip_id = project.add_midi_clip(0.0, 4.0)

        # Well-paired note-on / note-off: nothing hanging.
        project.set_midi_events(
            clip_id,
            [
                Project.midi_note_on(0.0, 0, 0, 60, 100),
                Project.midi_note_off(2.0, 0, 0, 60, 0),
            ],
        )
        paired = project.validate_midi_notes(clip_id)
        assert paired.ok is True
        assert paired.unmatched_note_ons == 0
        assert paired.unmatched_note_offs == 0

        # A single hanging note-on (no matching note-off).
        project.set_midi_events(
            clip_id,
            [Project.midi_note_on(0.0, 0, 0, 60, 100)],
        )
        hanging = project.validate_midi_notes(clip_id)
        assert hanging.ok is False
        assert hanging.unmatched_note_ons == 1
        assert hanging.unmatched_note_offs == 0

        # Export uses a half-open [source_offset, source_offset + length)
        # interval, so an off exactly at length_ppq is excluded and must be
        # reported before export too.
        _boundary_track, boundary_clip = project.add_midi_clip(0.0, 1.0)
        project.set_midi_events(
            boundary_clip,
            [
                Project.midi_note_on(0.0, 0, 0, 61, 100),
                Project.midi_note_off(1.0, 0, 0, 61, 0),
            ],
        )
        boundary = project.validate_midi_notes(boundary_clip)
        assert boundary.ok is False
        assert boundary.unmatched_note_ons == 1
        assert boundary.unmatched_note_offs == 0
    finally:
        project.close()


def test_set_midi_events_validates_event_shape_and_words() -> None:
    project = Project()
    try:
        _track_id, clip_id = project.add_midi_clip(0.0, 1.0)
        with pytest.raises(ValueError, match="ppq"):
            project.set_midi_events(clip_id, [(float("nan"), 0, 0)])
        with pytest.raises(ValueError, match="data0"):
            project.set_midi_events(clip_id, [(0.0, -1, 0)])
        with pytest.raises(ValueError, match="ppq, data0, data1"):
            project.set_midi_events(clip_id, [(0.0, 0)])
    finally:
        project.close()


# ---------------------------------------------------------------------------
# Parity surface added for Node/WASM cross-binding coverage: MIDI helper
# tables (ProgramMap / MidiRouter / CcMap) and the bake / last-bounce-compile-result
# accessors.
# ---------------------------------------------------------------------------


def test_gm_program_and_cc_name_tables_round_trip() -> None:
    assert Project.gm_instrument_name(0) == "Acoustic Grand Piano"
    assert Project.gm_program_for_name("Acoustic Grand Piano") == 0
    assert Project.gm_instrument_name(9999) is None
    assert Project.gm_program_for_name("not a real instrument") == -1
    assert isinstance(Project.gm_family_name(0), str)
    assert Project.gm_family_first_program(0) >= 0
    assert isinstance(Project.gm2_instrument_name(0, 0), (str, type(None)))
    assert isinstance(Project.gm_drum_name(35), (str, type(None)))
    assert Project.gm_drum_note_for_name("not a drum") == -1
    assert isinstance(Project.gm2_drum_set_name(0), (str, type(None)))
    assert isinstance(Project.gm2_drum_name(0, 35), (str, type(None)))
    assert Project.midi_cc_name(7) is not None
    assert Project.midi_cc_index_for_name("not a cc") == -1
    assert isinstance(Project.per_note_controller_name(0), (str, type(None)))


def test_midi_bank_program_lowers_to_events() -> None:
    events = Project.midi_bank_program(0.0, 0, 0, 0, 0, 5)
    assert isinstance(events, list)
    assert 1 <= len(events) <= 3
    for ev in events:
        assert len(ev) == 3  # (ppq, data0, data1)


def test_midi_route_events_filters_and_reports_overflow() -> None:
    events = [Project.midi_cc(0.0, 0, 2, 1, 64), Project.midi_cc(0.1, 0, 5, 1, 20)]
    result = Project.midi_route_events(events, {"filter_channel": 2})
    assert result.overflowed is False
    assert isinstance(result.overflow_count, int)
    # Only the channel-2 event survives the filter.
    assert len(result.events) == 1


def test_midi_cc_learn_and_conversion_helpers() -> None:
    # A 14-bit CC pair (MSB controller 1 + LSB controller 33) learns kind 1.
    events = [Project.midi_cc(0.0, 0, 2, 1, 64), Project.midi_cc(0.1, 0, 2, 33, 12)]
    binding = Project.midi_cc_learn(events, 77, min_value=-1.0, max_value=1.0)
    assert binding is not None
    assert binding.kind == 1
    assert binding.param_id == 77
    assert binding.min_value == -1.0

    # RPN selector assembly learns kind 2.
    rpn = [
        Project.midi_cc(0.0, 0, 3, 101, 0),
        Project.midi_cc(0.1, 0, 3, 100, 1),
        Project.midi_cc(0.2, 0, 3, 6, 64),
    ]
    rpn_binding = Project.midi_cc_learn(rpn, 78)
    assert rpn_binding is not None
    assert rpn_binding.kind == 2

    # No learnable CC stream -> None sentinel (INVALID_STATE), not an exception.
    assert Project.midi_cc_learn([], 99) is None

    # cc_to_breakpoint converts a matching CC to an automation point.
    point = Project.midi_cc_to_breakpoint([binding], Project.midi_cc(0.0, 0, 2, 1, 100))
    assert point is None or len(point) == 3
    # param_to_cc converts a parameter value back to a CC event (or None if unbound).
    back = Project.midi_param_to_cc([binding], 77, 0.5, 0)
    assert back is None or len(back) == 3


def test_project_import_smf_surfaces_key_signature_marker() -> None:
    project = Project()
    try:
        project.import_smf(_make_key_signature_smf())
        kinds = [project.marker_by_index(i).kind for i in range(project.marker_count())]
        assert MarkerKind.KEY_SIGNATURE in kinds
        key = next(
            project.marker_by_index(i)
            for i in range(project.marker_count())
            if project.marker_by_index(i).kind == MarkerKind.KEY_SIGNATURE
        )
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
        project.set_sample_rate(48000.0)
        _track, clip = project.add_midi_clip(0.0, 4.0)
        project.set_midi_events(
            clip,
            [Project.midi_note_on(0.0, 0, 0, 60, 100), Project.midi_note_off(2.0, 0, 0, 60, 0)],
        )
        # A MIDI-only bounce with no instrument compiles to silence but records
        # the no-instrument diagnostic, retrievable after the bounce.
        project.bounce(total_frames=48000, num_channels=2, sample_rate=48000)
        result = project.last_bounce_compile_result()
        assert result.has_timeline is True
        assert isinstance(result.diagnostics, (list, tuple))
    finally:
        project.close()


def test_bake_midi_fx_preserves_more_than_512_events() -> None:
    project = Project()
    try:
        _track, clip = project.add_midi_clip(0.0, 8.0)
        events = [Project.midi_cc(i / 80, 0, 0, i % 128, (i * 3) % 128) for i in range(600)]
        project.set_midi_events(clip, events)
        project.bake_midi_fx(clip, "{}")
        payload = json.loads(project.to_json())
        baked = payload["midi_content"][str(clip)]
        assert baked == [
            {"data0": data0, "data1": data1, "ppq": ppq} for ppq, data0, data1 in events
        ]

        roundtrip = Project()
        try:
            round_clip = roundtrip.import_smf(project.export_smf())
            roundtrip.bake_midi_fx(round_clip, "{}")
            round_payload = json.loads(roundtrip.to_json())
            assert len(round_payload["midi_content"][str(round_clip)]) == 600
        finally:
            roundtrip.close()
    finally:
        project.close()


def test_bake_midi_fx_reports_source_index() -> None:
    def seed(project: Project, clip: int) -> None:
        project.set_midi_events(
            clip,
            [
                Project.midi_note_on(0.0, 0, 0, 60, 100),
                Project.midi_note_off(1.0, 0, 0, 60, 0),
                Project.midi_note_on(2.0, 0, 0, 64, 100),
                Project.midi_note_off(3.0, 0, 0, 64, 0),
            ],
        )

    config = '{"chord_intervals":[0,4,7]}'
    tracked = Project()
    plain = Project()
    try:
        _track, tracked_clip = tracked.add_midi_clip(0.0, 4.0)
        seed(tracked, tracked_clip)
        assert tracked.preview_midi_fx_count(tracked_clip, config) == 12
        # The preview must leave the clip alone.
        assert len(json.loads(tracked.to_json())["midi_content"][str(tracked_clip)]) == 4

        source_index = tracked.bake_midi_fx(tracked_clip, config, with_source_index=True)
        assert source_index is not None
        assert len(source_index) == 12
        per_input = [0, 0, 0, 0]
        for source in source_index:
            assert 0 <= source < 4
            per_input[source] += 1
        assert per_input == [3, 3, 3, 3]

        # Without the flag the call keeps returning None and produces the same
        # bytes, so the provenance path cannot drift from the plain one.
        _plain_track, plain_clip = plain.add_midi_clip(0.0, 4.0)
        seed(plain, plain_clip)
        assert plain.bake_midi_fx(plain_clip, config) is None
        assert (
            json.loads(tracked.to_json())["midi_content"][str(tracked_clip)]
            == json.loads(plain.to_json())["midi_content"][str(plain_clip)]
        )

        with pytest.raises(SonareError):
            tracked.bake_midi_fx(tracked_clip, "{bad json", with_source_index=True)
        with pytest.raises(SonareError):
            tracked.preview_midi_fx_count(tracked_clip, "{bad json")
    finally:
        tracked.close()
        plain.close()


def test_project_import_smf_preserves_salvaged_truncation() -> None:
    project = Project()
    try:
        assert project.import_smf(_make_truncated_smf()) > 0
    finally:
        project.close()
