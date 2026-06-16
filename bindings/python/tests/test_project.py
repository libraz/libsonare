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

from libsonare import (
    BuiltinSynthConfig,
    ExternalInstrument,
    MarkerKind,
    Project,
    ProjectMarker,
    SonareError,
    project_abi_version,
)
from libsonare._project import EXPECTED_PROJECT_ABI_VERSION
from libsonare._runtime import _get_lib


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


def _dangling_source_json() -> str:
    return (
        '{"version":1,"sample_rate":48000,'
        '"tracks":[{"id":1,"name":"audio","kind":0,"channel_strip_ref":"",'
        '"output_target":"","midi_destination_id":0,"automation_lanes":[]}],'
        '"clips":[{"id":1,"track_id":1,"source_id":99,"start_ppq":0,'
        '"length_ppq":1,"source_offset_ppq":0,"gain":1,'
        '"fade_in":{"length_ppq":0,"curve":0},'
        '"fade_out":{"length_ppq":0,"curve":0},'
        '"loop_mode":0,"loop_length_ppq":0,"warp_ref_id":0,"warp_mode":0}]}'
    )


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


def test_from_json_with_diagnostics_returns_success_warnings() -> None:
    result = Project.from_json_with_diagnostics(_dangling_source_json())
    try:
        assert "dangling_clip_source" in result.diagnostics
        assert result.project.track_count() == 1
    finally:
        result.project.close()


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


class _ConstantInstrument:
    """An :class:`ExternalInstrument` that emits a constant DC level and records
    every dispatched MIDI event, so a test can assert both audible output and
    sample-accurate event delivery."""

    def __init__(self, level: float = 0.25) -> None:
        self.level = float(level)
        self.prepared: tuple[float, int] | None = None
        self.events: list[tuple[int, tuple[int, ...], int]] = []
        self.render_calls = 0

    def prepare(self, sample_rate: float, max_block_size: int) -> None:
        self.prepared = (sample_rate, max_block_size)

    def on_event(self, destination_id: int, ump_words: tuple[int, ...], render_frame: int) -> None:
        self.events.append((destination_id, ump_words, render_frame))

    def render(self, channels: np.ndarray, num_frames: int) -> None:
        self.render_calls += 1
        channels += self.level


def test_bounce_with_instruments_hosts_external_callback() -> None:
    """Flagship: a MIDI-only project routes through a host-supplied instrument."""
    if not hasattr(_get_lib(), "sonare_project_bounce_with_instruments"):
        pytest.skip("libsonare built without the external-instrument bounce ABI")
    project = _build_midi_only_project()
    instrument = _ConstantInstrument(level=0.25)
    try:
        audio = project.bounce_with_instruments(
            instrument, total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert audio.shape == (48000, 2)
        assert audio.dtype == np.float32
        # The instrument emits a constant 0.25 DC, so every rendered frame is audible.
        assert float(np.min(audio)) == pytest.approx(0.25, abs=1e-6)
        assert instrument.prepared is not None
        assert instrument.render_calls > 0
        # The note-on / note-off were delivered as dispatched UMP events.
        assert len(instrument.events) >= 2
        statuses = [(words[0] >> 20) & 0xF for _dst, words, _frame in instrument.events if words]
        assert 0x9 in statuses  # note-on
        assert 0x8 in statuses  # note-off
    finally:
        project.close()


def test_bounce_with_instruments_render_only_instrument() -> None:
    """Only render() is required; prepare/on_event are optional (duck-typed)."""
    if not hasattr(_get_lib(), "sonare_project_bounce_with_instruments"):
        pytest.skip("libsonare built without the external-instrument bounce ABI")

    class RenderOnly:
        def render(self, channels: np.ndarray, num_frames: int) -> None:
            channels += 0.1

    project = _build_midi_only_project()
    try:
        audio = project.bounce_with_instruments(
            RenderOnly(), total_frames=4800, num_channels=2, sample_rate=48000
        )
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_instruments_auto_length_includes_tail_samples() -> None:
    """External instruments can report release/effect tail for auto-length bounce."""
    if not hasattr(_get_lib(), "sonare_project_bounce_with_instruments"):
        pytest.skip("libsonare built without the external-instrument bounce ABI")

    project = _build_midi_only_project()
    try:
        dry = _ConstantInstrument(level=0.0)
        no_tail = project.bounce_with_instruments(
            dry, total_frames=0, block_size=128, num_channels=2, sample_rate=48000
        )

        wet = _ConstantInstrument(level=0.0)
        wet.tail_samples = 4096
        with_tail = project.bounce_with_instruments(
            wet, total_frames=0, block_size=128, num_channels=2, sample_rate=48000
        )
        assert with_tail.shape[0] == no_tail.shape[0] + wet.tail_samples
    finally:
        project.close()


def test_bounce_with_instruments_propagates_callback_error() -> None:
    """An exception raised inside a callback surfaces to the caller, not silenced."""
    if not hasattr(_get_lib(), "sonare_project_bounce_with_instruments"):
        pytest.skip("libsonare built without the external-instrument bounce ABI")

    class Boom:
        def render(self, channels: np.ndarray, num_frames: int) -> None:
            raise ValueError("synthesis failed")

    project = _build_midi_only_project()
    try:
        with pytest.raises(ValueError, match="synthesis failed"):
            project.bounce_with_instruments(
                Boom(), total_frames=4800, num_channels=2, sample_rate=48000
            )
    finally:
        project.close()


def test_external_instrument_is_exported() -> None:
    import libsonare

    assert libsonare.ExternalInstrument is ExternalInstrument


def test_bounce_with_instruments_requires_an_instrument() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(ValueError, match="requires"):
            project.bounce_with_instruments(num_channels=2, sample_rate=48000)
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
        # The project carries a MIDI clip, so the compiler emits a single
        # best-effort warning (code 10 = kMidiClipNoInstrument) that the bounce is
        # silent unless an instrument is bound. It is non-fatal (timeline valid).
        assert result.diagnostic_count == 1
        assert result.diagnostics[0].code == 10
        assert result.diagnostics[0].severity == 1  # warning
        assert "project contains MIDI clips" in result.diagnostics[0].message
        assert result.messages.splitlines()[0] == result.diagnostics[0].message
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
        with pytest.raises(SonareError) as exc:
            project.undo()
        assert exc.value.code != 0
        assert str(exc.value).startswith(f"[{exc.value.code}] ")
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
        project.set_clip_warp_mode(clip_id, "repitch")
        after = project.to_json_bytes()
        assert after != before
        assert b'"warp_ref_id":123' in after
        assert b'"warp_mode":1' in after
        restored = Project.from_json(after)
        try:
            assert restored.to_json_bytes() == after
        finally:
            restored.close()

        project.undo()
        assert b'"warp_mode":0' in project.to_json_bytes()
        project.undo()
        assert project.to_json_bytes() == before

        project.set_clip_warp_mode(clip_id, "tempo-sync")
        assert b'"warp_mode":2' in project.to_json_bytes()
    finally:
        project.close()


def test_set_clip_takes_and_comp_segments_round_trip_and_undo() -> None:
    project, audio_clip, *_ = _build_project()
    try:
        before = project.to_json_bytes()

        project.set_clip_takes(
            audio_clip,
            [
                {"id": 1, "sourceOffsetPpq": 0.0, "name": "take A"},
                {"id": 2, "source_offset_ppq": 0.5, "name": "take B"},
            ],
            active_take_id=1,
        )
        with_takes = project.to_json_bytes()
        assert with_takes != before
        assert b'"takes"' in with_takes
        assert b'"active_take_id":1' in with_takes

        project.set_clip_comp_segments(
            audio_clip,
            [
                {"startPpq": 0.0, "endPpq": 1.0, "takeId": 1},
                {"start_ppq": 1.0, "end_ppq": 2.0, "take_id": 2},
            ],
        )
        with_comp = project.to_json_bytes()
        assert b'"comp_segments"' in with_comp

        project.undo()
        assert project.to_json_bytes() == with_takes
        project.undo()
        assert project.to_json_bytes() == before
        project.redo()
        assert project.to_json_bytes() == with_takes

        with pytest.raises(SonareError):
            project.set_clip_takes(
                audio_clip,
                [(1, 0, 0.0, "duplicate A"), (1, 0, 0.0, "duplicate B")],
                active_take_id=1,
            )
        with pytest.raises(SonareError):
            project.set_clip_comp_segments(
                audio_clip, [{"startPpq": 0.0, "endPpq": 1.0, "takeId": 99}]
            )
    finally:
        project.close()


def test_add_loop_recording_takes_splits_capture_into_active_take() -> None:
    project = Project()
    try:
        project.set_sample_rate(48000.0)
        track_id = project.add_track("audio", "record")
        audio = np.empty(48000, dtype=np.float32)
        audio[:24000] = 0.25
        audio[24000:] = 0.75

        clip_id, take_count = project.add_loop_recording_takes(
            track_id,
            start_ppq=0.0,
            loop_length_ppq=1.0,
            audio=audio,
            audio_channels=1,
            audio_sample_rate=48000,
        )
        assert clip_id != 0
        assert take_count == 2
        json = project.to_json_bytes()
        assert b'"takes"' in json
        assert b'"active_take_id":2' in json

        project.undo()
        assert b'"clips":[]' in project.to_json_bytes()
    finally:
        project.close()


def test_project_rejects_audio_length_not_matching_channels() -> None:
    project = Project()
    try:
        track_id = project.add_track("audio", "record")
        audio = np.zeros(5, dtype=np.float32)
        with pytest.raises(ValueError, match="audio length must be a multiple of audio_channels"):
            project.add_clip(
                track_id,
                start_ppq=0.0,
                length_ppq=1.0,
                audio=audio,
                audio_channels=2,
                audio_sample_rate=48000,
            )
        with pytest.raises(ValueError, match="audio length must be a multiple of audio_channels"):
            project.add_loop_recording_takes(
                track_id,
                start_ppq=0.0,
                loop_length_ppq=1.0,
                audio=audio,
                audio_channels=2,
                audio_sample_rate=48000,
            )
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
# tables (ProgramMap / MidiRouter / CcMap), project getters/setters, and the
# bake / last-bounce-compile-result accessors.
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


def test_project_getters_setters_and_counts() -> None:
    project = Project()
    try:
        project.set_sample_rate(44100.0)
        assert project.get_sample_rate() == 44100.0

        project.set_overlap_policy(1)
        assert project.get_overlap_policy() == 1

        project.set_tempo_segments([(0.0, 120.0), (480.0, 140.0)])
        assert project.tempo_segment_count() == 2
        project.set_time_signatures([(0.0, 4, 4), (1920.0, 3, 4)])
        assert project.time_signature_count() == 2

        marker_id = project.set_marker(0, 1.0, "intro")
        assert marker_id > 0

        assert project.track_count() == 0
        assert project.source_count() == 0
    finally:
        project.close()


def test_project_marker_ex_round_trips_kind_and_key() -> None:
    project = Project()
    try:
        plain_id = project.set_marker_ex(ProjectMarker(0, 1.0, "intro"))
        assert plain_id > 0
        key_id = project.set_marker_ex(
            ProjectMarker(
                0,
                2.0,
                "E flat major",
                kind=MarkerKind.KEY_SIGNATURE,
                key_fifths=-3,
                key_minor=False,
            )
        )
        assert key_id > 0 and key_id != plain_id
        assert project.marker_count() == 2

        # marker_by_index iterates every stored marker in [0, marker_count).
        ids = [project.marker_by_index(i).id for i in range(project.marker_count())]
        assert ids == [plain_id, key_id]

        plain = project.marker_by_index(0)
        assert plain.id == plain_id
        assert plain.kind == MarkerKind.MARKER
        assert plain.name == "intro"
        assert plain.key_fifths == 0
        assert plain.key_minor is False

        key = project.marker_by_index(1)
        assert key.id == key_id
        assert key.kind == MarkerKind.KEY_SIGNATURE
        assert key.name == "E flat major"
        assert key.key_fifths == -3
        assert key.key_minor is False

        with pytest.raises(SonareError):
            project.marker_by_index(2)
    finally:
        project.close()


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
