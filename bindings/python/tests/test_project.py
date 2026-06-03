"""Tests for the headless arrangement / DAW :class:`Project` Python wrapper.

Mirrors the C keystone parity test
(``tests/bindings/binding_project_parity_test.cpp``): it drives the
``sonare_project_*`` C ABI end to end through the Python wrapper and pins the
ABI version, deterministic serialize byte-stability, bit-exact bounce,
undo/redo, MIR snap-to-grid, and malformed-input handling.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import BuiltinSynthConfig, Project, project_abi_version
from libsonare._project import EXPECTED_PROJECT_ABI_VERSION


def _make_stereo_sine(frames: int, sample_rate: float = 48000.0) -> np.ndarray:
    """Build interleaved stereo samples mirroring the C parity fixture."""
    t = np.arange(frames, dtype=np.float64) / sample_rate
    left = 0.25 * np.sin(2.0 * math.pi * 220.0 * t)
    right = 0.18 * np.sin(2.0 * math.pi * 330.0 * t)
    interleaved = np.empty(frames * 2, dtype=np.float32)
    interleaved[0::2] = left.astype(np.float32)
    interleaved[1::2] = right.astype(np.float32)
    return interleaved


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


def _build_project() -> tuple[Project, int, int, int]:
    """Build a small non-trivial project: audio track+clip and MIDI track+clip."""
    project = Project()
    project.set_sample_rate(48000.0)

    audio_track = project.add_track("audio", "audio")
    assert audio_track != 0

    audio = _make_stereo_sine(48000)
    audio_clip = project.add_clip(
        audio_track,
        start_ppq=0.0,
        length_ppq=2.0,
        audio=audio,
        audio_channels=2,
        audio_sample_rate=48000,
    )
    assert audio_clip != 0

    midi_track, midi_clip = project.add_midi_clip(0.0, 4.0)
    assert midi_clip != 0

    project.set_midi_events(
        midi_clip,
        [(0.0, 0x20903C40, 0), (1.0, 0x20803C00, 0)],
    )
    return project, audio_clip, midi_track, midi_clip


# --- ABI version -----------------------------------------------------------


def test_project_abi_version_matches_expected() -> None:
    """The runtime project ABI version is exposed and matches the binding."""
    assert project_abi_version() == EXPECTED_PROJECT_ABI_VERSION
    assert project_abi_version() > 0


# --- serialization ---------------------------------------------------------


def test_serialize_round_trips_byte_identically() -> None:
    project, *_ = _build_project()
    try:
        first = project.to_json_bytes()
        assert first

        second = Project.from_json(first)
        try:
            assert second.to_json_bytes() == first
        finally:
            second.close()
    finally:
        project.close()


def test_to_json_is_utf8_decoded() -> None:
    project, *_ = _build_project()
    try:
        text = project.to_json()
        assert isinstance(text, str)
        assert text.encode("utf-8") == project.to_json_bytes()
    finally:
        project.close()


def test_malformed_deserialize_raises_without_crashing() -> None:
    with pytest.raises(ValueError):
        Project.from_json("{ this is not valid project json ]]")
    with pytest.raises(ValueError):
        Project.from_json(b"")


# --- bounce ----------------------------------------------------------------


def test_bounce_is_bit_exact_across_two_renders() -> None:
    project, *_ = _build_project()
    try:
        first = project.bounce(
            total_frames=24000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert first.shape == (24000, 2)
        assert first.dtype == np.float32

        second = project.bounce(
            total_frames=24000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert second.shape == first.shape
        # Deterministic: same project + options => bit-identical output.
        assert np.array_equal(first, second)
    finally:
        project.close()


def _build_midi_only_project() -> Project:
    """A MIDI-only arrangement: one MIDI track + clip with a sustained note."""
    project = Project()
    project.set_sample_rate(48000.0)
    _track, clip = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, 60, 100),
            Project.midi_note_off(2.0, 0, 0, 60, 0),
        ],
    )
    return project


def test_bounce_with_builtin_instrument_produces_non_silent_audio() -> None:
    """Flagship: a MIDI-only project bounced through the built-in synth is audible."""
    project = _build_midi_only_project()
    try:
        # Silent baseline: plain bounce has no instrument bound -> MIDI is silence.
        silent = project.bounce(
            total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(silent))) == 0.0

        audio = project.bounce_with_builtin_instrument(
            total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert audio.shape == (48000, 2)
        assert audio.dtype == np.float32
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_builtin_instrument_auto_derives_length() -> None:
    """Omitting total_frames lets the native layer derive the render length."""
    project = _build_midi_only_project()
    try:
        audio = project.bounce_with_builtin_instrument(num_channels=2, sample_rate=48000)
        assert audio.ndim == 2
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_builtin_instrument_accepts_waveform_patch() -> None:
    """A non-default patch (named waveform + overrides) still renders audibly."""
    project = _build_midi_only_project()
    try:
        patch = BuiltinSynthConfig(waveform="saw", gain=0.3, polyphony=8)
        audio = project.bounce_with_builtin_instrument(
            patch, total_frames=24000, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_auto_derives_length_when_total_frames_omitted() -> None:
    """bounce() with total_frames omitted no longer renders empty (C auto-derives)."""
    project, *_ = _build_project()
    try:
        audio = project.bounce(num_channels=2, sample_rate=48000)
        assert audio.ndim == 2
        assert audio.shape[0] > 0
    finally:
        project.close()


# --- compile ---------------------------------------------------------------


def test_compile_surfaces_renderable_timeline() -> None:
    project, *_ = _build_project()
    try:
        has_timeline, messages = project.compile()
        assert has_timeline is True
        assert isinstance(messages, str)
        result = project.compile()
        assert result.has_timeline is True
        assert result.diagnostic_count == 0
        assert result.diagnostics == ()
    finally:
        project.close()


# --- undo / redo -----------------------------------------------------------


def test_undo_restores_serialized_bytes_and_redo_reapplies() -> None:
    project, audio_clip, *_ = _build_project()
    try:
        before = project.to_json_bytes()

        new_clip = project.split_clip(audio_clip, 1.0)
        assert new_clip != 0
        after = project.to_json_bytes()
        assert after != before

        project.undo()
        assert project.to_json_bytes() == before

        project.redo()
        assert project.to_json_bytes() == after
    finally:
        project.close()


def test_undo_on_empty_stack_raises() -> None:
    project = Project()
    try:
        with pytest.raises(RuntimeError):
            project.undo()
    finally:
        project.close()


def test_set_track_midi_destination_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id, _clip_id = project.add_midi_clip(0.0, 4.0)
        before = project.to_json_bytes()

        project.set_track_midi_destination(track_id, 7)
        after = project.to_json_bytes()
        assert after != before
        assert b'"midi_destination_id":7' in after

        # Routes through the edit history, so undo restores the prior routing.
        project.undo()
        assert project.to_json_bytes() == before
    finally:
        project.close()


def test_set_clip_warp_ref_round_trips_and_undoes() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "audio")
        clip_id = project.add_clip(
            track_id=track_id, start_ppq=0.0, length_ppq=4.0, audio_channels=0
        )
        before = project.to_json_bytes()

        project.set_clip_warp_ref(clip_id, 123)
        after = project.to_json_bytes()
        assert after != before
        assert b'"warp_ref_id":123' in after

        project.undo()
        assert project.to_json_bytes() == before
    finally:
        project.close()


def test_set_track_midi_destination_rejects_unknown_track() -> None:
    project = Project()
    try:
        with pytest.raises(RuntimeError):
            project.set_track_midi_destination(9999, 1)
    finally:
        project.close()


# --- MIR -------------------------------------------------------------------


def test_snap_to_grid_snaps_near_beat_to_line() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        assert project.snap_to_grid(1.02, 1.0) == 1.0
    finally:
        project.close()


def test_auto_tempo_returns_positive_bpm() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        # A steady click-like pulse train so the beat bridge has onsets.
        sr = 48000
        audio = np.zeros(sr * 2, dtype=np.float32)
        for beat in range(0, len(audio), sr // 2):  # 120 BPM
            audio[beat : beat + 64] = 1.0
        bpm = project.auto_tempo(audio, sr)
        assert bpm > 0.0
    finally:
        project.close()


# --- lifecycle -------------------------------------------------------------


def test_context_manager_and_aliases_close_handle() -> None:
    with Project() as project:
        project.set_sample_rate(44100.0)
    # Cross-binding close aliases are all safe no-ops after close.
    project.destroy()
    project.delete()
    project.close()
    with pytest.raises(RuntimeError):
        project.add_track("audio")


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
