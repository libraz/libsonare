# ruff: noqa: F405
"""Project MIDI editing, routing, and catalog methods."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Literal, cast

import numpy as np

from ._effects_note_model import (
    _transcribe_config,
    _transcribe_sample_rate,
)
from ._errors import _entry, _refuse

# The transcription config is built once for the whole binding: this method and
# the free `transcribe` take the same keyword options, and two builders would be
# two chances for their domains to part company.
from ._narrowing import _narrow_double
from ._project_model import *  # noqa: F403
from ._project_model import (
    _absent_or_in_range,
    _cc_binding_from_c,
    _cc_binding_to_c,
    _midi_event_from_c,
    _midi_event_packed,
    _midi_event_to_c,
    _midi_events_to_c,
    _validate_midi_event_ppq,
    _validate_midi_event_word,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    _AUTOMATION_CURVES,
    AutomationCurve,
    SonareAutomationPoint,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    SonareMidiRouteConfig,
    SonareNotePairValidation,
    _check,
    _from_c_int_array,
    _get_lib,
    _guard_buffer,
    _midi_fx_config_json,
    _narrow_int,
    _resolve_optional_non_negative,
    _to_c_double,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_size_t,
    _to_c_uint8,
    _to_c_uint16,
    _to_c_uint32,
    _utf8_arg,
)
from ._types_engine import AutomationPoint, MidiFxConfig


def _found_index(value: int, message: str) -> int:
    """A reverse lookup's answer; the C ABI's -1 (nothing found) is refused."""
    if value < 0:
        raise _refuse(message)
    return int(value)


class _ProjectMidiMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    @_entry("Project.set_midi_events")
    def set_midi_events(
        self,
        clip_id: int,
        events: Sequence[ProjectMidiEvent] | np.ndarray,
    ) -> None:
        """Replace a MIDI clip's entire event list.

        Each event is a :class:`ProjectMidiEvent` (a PPQ position and the first
        two UMP-1.0 words of a channel-voice message, stored opaquely), or the
        whole list is one ``(n, 3)`` array whose columns are ``ppq``, ``word0``
        and ``word1``. Pass an empty sequence to clear.

        Drops the clip's SysEx, which :meth:`import_smf` and :meth:`export_smf`
        both keep. The payloads sit beside the event list and are reached by a
        handle this triple does not carry, so replacing the list leaves nothing
        referring to them: a GS setup block that survives an import and an
        export byte for byte is gone after one call here. Nothing reads the
        handles back either, so a caller that must keep the setup edits the
        exported file rather than the event list.

        Only events inside the clip's half-open window ``[0, length_ppq)`` (after
        the source offset) are kept for export; a note still sounding at the clip
        end, including one whose note-off sits at or past ``length_ppq``, is
        closed with a note-off at the clip end.
        """
        if isinstance(events, np.ndarray):
            if events.ndim != 2 or events.shape[1] != 3:
                raise _refuse("events must be an (n, 3) array of ppq, word0, word1 columns")
            count = int(events.shape[0])
            c_events = (SonareMidiEventPod * count)()
            for i, (ppq, word0, word1) in enumerate(events.tolist()):
                c_events[i].ppq = _validate_midi_event_ppq(ppq, f"events[{i}].ppq")
                c_events[i].word0 = _validate_midi_event_word(word0, f"events[{i}].word0")
                c_events[i].word1 = _validate_midi_event_word(word1, f"events[{i}].word1")
        else:
            c_events, count = _midi_events_to_c(events, "events")
        _check(
            _get_lib().sonare_project_set_midi_events(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                c_events if count else None,
                _to_c_size_t(count, "count"),
            )
        )

    @_entry("Project.transcribe_to_clip")
    @_guard_buffer("samples")
    def transcribe_to_clip(
        self,
        clip_id: int,
        samples: Sequence[float] | np.ndarray,
        sample_rate: int,
        *,
        polyphonic: bool = False,
        reference_hz: float | str | None = None,
        f0_min_hz: float | None = None,
        f0_max_hz: float | None = None,
        min_note_ms: float | None = None,
        segmentation_threshold_cents: float | None = None,
        velocity_floor_db: float | None = None,
        fixed_velocity: int | None = None,
        group: int = 0,
        channel: int = 0,
        max_polyphony: int | None = None,
        min_frame_peak_ratio: float | None = None,
        min_ridge_peak_ratio: float | None = None,
        reattack_ratio: float | None = None,
        min_note_division: int | None = None,
        monophonic_model: Literal["segments", "singing"] | None = None,
    ) -> int:
        """Transcribe mono audio straight into a MIDI clip's event list.

        The PPQ grid is the PROJECT's tempo map, so a project whose tempo was
        installed by :meth:`auto_tempo` transcribes onto that map rather than
        onto a second, separately detected tempo -- which is why this takes no
        tempo argument, where :func:`transcribe` does. It REPLACES the clip's
        entire event list, exactly as :meth:`set_midi_events` does.

        The keyword options, and what this deliberately leaves to
        :meth:`bake_midi_fx` and :meth:`auto_tempo`, are :func:`transcribe`'s.

        Args:
            clip_id: MIDI clip to write; its previous events are discarded.
            samples: Mono source audio (any sequence convertible to float32).
            sample_rate: Sample rate of ``samples`` in Hz. Need not match the
                project's: the conversion goes through seconds, so a 44.1 kHz
                take lands correctly on a 48 kHz project's grid.
            polyphonic: Read the multi-F0 chain instead of pYIN cut into notes.
                Working memory grows with the input: about 0.75 MB per second
                of audio at any rate, plus about 3 MB, plus about 32 bytes per
                sample of the longest note while it is measured.
            reference_hz: Tuning reference in Hz; ``"auto"`` measures it from the
                audio; ``None`` keeps the default (440 Hz).
            f0_min_hz: Low end of the F0 tracker's range; ``None`` uses 65 Hz for the
                monophonic path or 55 Hz for the polyphonic path. Both paths use
                this bound. The polyphonic path applies it to the salience
                estimator's F0 axis; its cent-spectrum bounds remain independent.
            f0_max_hz: High end of the F0 tracker's range; ``None`` uses 2093 Hz for
                the monophonic path or 1760 Hz for the polyphonic path. Both
                paths use this bound, with the same polyphonic salience-axis
                behavior as ``f0_min_hz``.
            min_note_ms: Shortest span kept as a note; ``None`` keeps 30 ms
                (monophonic segments), 45 ms (singing) or 60 ms (polyphonic),
                whatever the tempo. Not
                given together with ``min_note_division``.
            min_note_division: Shortest note kept, as a note value: ``n`` is a
                1/n note at the project's tempo at the clip start (32 is a
                thirty-second note), 1..128. Applies to both paths.
            segmentation_threshold_cents: Monophonic only, in cents.
                ``"segments"`` ends one note and starts the next where the pitch
                moves this far; ``"singing"`` reads it as the scale of the pitch
                movement kept within one note. ``None`` keeps 50 cents.
            monophonic_model: Monophonic only. ``"segments"`` (the default, also
                ``None``) cuts the pitch track where it moves and splits brief
                level dips -- accurate on instrument lines, short notes included.
                ``"singing"`` decodes notes jointly over pitch bins and holds
                vibrato and intonation drift within a note -- suited to sung
                takes, and can merge very short notes at fast tempos. Refused
                with ``polyphonic=True``.
            velocity_floor_db: Level mapped to velocity 1; a note under it gets
                velocity 1. A note more than 36 dB under the take's loudest
                notes, or under -80 dBFS, is dropped whatever this is. Must be
                negative. ``None`` keeps the default (-48 dB).
            fixed_velocity: 1..127 gives every note that velocity instead of the
                mapped one (the level gate still applies); ``None`` maps the
                level.
            group: UMP group the events are emitted on, 0..15.
            channel: MIDI channel the events are emitted on, 0..15.
            max_polyphony: Polyphonic only. Voices one frame may hold, 1..64;
                ``None`` keeps the default (10).
            min_frame_peak_ratio: Polyphonic only. A frame's search stops below
                this share of its first peak, 0..1; ``None`` keeps 0.20 and 0
                means a real 0.
            min_ridge_peak_ratio: Polyphonic only. A ridge ends below this share
                of its own running peak, 0..1; ``None`` keeps 0.10 and 0 means a
                real 0.
            reattack_ratio: Polyphonic only. Splits a ridge where its salience
                climbs past this multiple of the level just before; above 1.
                ``None`` means no split.

        Returns:
            The number of notes written; the clip holds twice that many events.

        Raises:
            SonareValueError: If ``samples`` is empty or holds a NaN or Inf
                sample, if a config argument is outside its domain, or if both
                ``f0_min_hz`` and ``f0_max_hz`` are given and ``f0_min_hz`` is
                not below ``f0_max_hz``.
            SonareValueError: When only one of ``f0_min_hz``/``f0_max_hz``
                is given and it does not lie on the right side of the selected
                source's default for the other bound; also if the clip id is
                unknown.
            SonareError: ``NOT_SUPPORTED`` when the library was built without
                the pitch editor or the arrangement subsystem.

        Example:
            >>> project.auto_tempo(samples, sample_rate=sr)
            >>> clip_id = project.add_midi_clip(0.0, 8.0).clip_id
            >>> project.transcribe_to_clip(clip_id, samples, sr)
            4
        """
        rate = _transcribe_sample_rate(sample_rate)
        config = _transcribe_config(
            polyphonic=polyphonic,
            reference_hz=reference_hz,
            f0_min_hz=f0_min_hz,
            f0_max_hz=f0_max_hz,
            min_note_ms=min_note_ms,
            segmentation_threshold_cents=segmentation_threshold_cents,
            velocity_floor_db=velocity_floor_db,
            fixed_velocity=fixed_velocity,
            group=group,
            channel=channel,
            max_polyphony=max_polyphony,
            min_frame_peak_ratio=min_frame_peak_ratio,
            min_ridge_peak_ratio=min_ridge_peak_ratio,
            reattack_ratio=reattack_ratio,
            min_note_division=min_note_division,
            monophonic_model=monophonic_model,
        )
        c_array, length = _to_c_float_array(samples)
        note_count = ctypes.c_size_t()
        _check(
            _get_lib().sonare_project_transcribe_to_clip(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(rate, "sample_rate"),
                ctypes.byref(config),
                ctypes.byref(note_count),
            )
        )
        return int(note_count.value)

    @_entry("Project.import_smf")
    def import_smf(self, data: bytes) -> int:
        """Import an in-memory SMF buffer; return the first added clip id.

        Raises :class:`SonareError` (``INVALID_FORMAT``) on malformed or partially truncated input
        instead of installing a silently shortened clip.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_smf(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    @_entry("Project.export_smf")
    def export_smf(self) -> bytes:
        """Export the project's tempo map + MIDI clips to an SMF byte buffer."""
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_smf(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out or out_len.value == 0:
                return b""
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_bytes(out)

    @_entry("Project.import_clip_file")
    def import_clip_file(self, data: bytes) -> int:
        """Import an in-memory MIDI 2.0 Clip File (``SMF2CLIP``); return the
        first added clip id.

        Unlike :meth:`import_smf`, the UMP-based container preserves MIDI 2.0
        channel-voice messages (16-bit velocity, 32-bit CC, per-note /
        registered controllers, bank-valid Program Change) without loss. Raises
        ``ValueError`` on malformed input, never crashing.
        """
        lib = _get_lib()
        raw = bytes(data)
        buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_project_import_clip_file(
                self._require_handle(),
                buf,
                ctypes.c_size_t(len(raw)),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

    @_entry("Project.export_clip_file")
    def export_clip_file(self) -> bytes:
        """Export the project's tempo map + MIDI clips to a MIDI 2.0 Clip File
        (``SMF2CLIP``) byte buffer.

        MIDI 2.0-only events are written WITHOUT loss — prefer this over
        :meth:`export_smf` when MIDI 2.0 fidelity matters.
        """
        lib = _get_lib()
        out = ctypes.POINTER(ctypes.c_uint8)()
        out_len = ctypes.c_size_t()
        _check(
            lib.sonare_project_export_clip_file(
                self._require_handle(), ctypes.byref(out), ctypes.byref(out_len)
            )
        )
        try:
            if not out or out_len.value == 0:
                return b""
            return ctypes.string_at(out, out_len.value)
        finally:
            if out:
                lib.sonare_free_bytes(out)

    @_entry("Project.set_program")
    def set_program(self, clip_id: int, program: int, bank: int | None = None) -> None:
        """Set a MIDI clip's channel-0 program / bank at source PPQ 0.

        ``bank`` defaults to ``None`` (no Bank Select emitted), matching
        :meth:`set_program_on_channel`; pass ``>= 0`` to emit a Bank Select. A
        negative ``bank`` is refused.
        """
        _check(
            _get_lib().sonare_project_set_program(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_int(program, "program"),
                _to_c_int(_resolve_optional_non_negative(bank, "bank"), "bank"),
            )
        )

    @_entry("Project.set_program_on_channel")
    def set_program_on_channel(
        self, clip_id: int, group: int, channel: int, program: int, bank: int | None = None
    ) -> None:
        """Set a MIDI clip's program / bank for one UMP group and channel."""
        _check(
            _get_lib().sonare_project_set_program_on_channel(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_int(program, "program"),
                _to_c_int(_resolve_optional_non_negative(bank, "bank"), "bank"),
            )
        )

    @_entry("Project.bake_midi_fx")
    def bake_midi_fx(
        self,
        clip_id: int,
        config: MidiFxConfig,
        *,
        with_source_index: bool = False,
    ) -> np.ndarray | None:
        """Destructively bake a clip's MIDI-FX chain from a :data:`MidiFxConfig`.

        Canonical name matching the Node / WASM ``bakeMidiFx`` surface. Large
        clips are drained without truncation; failure leaves the original clip
        unchanged.

        With ``with_source_index`` set, returns an ``int32`` array with one
        entry per transformed event in canonical order: the index of the input
        event it derives from, or ``-1`` for an event with no originating input.
        Chord and arpeggiator fan-out makes several outputs share one source
        index, so a caller that treats the first output per index as the same
        event and the rest as newly generated can carry a selection across the
        bake. Returns ``None`` otherwise.
        """
        handle = self._require_handle()
        config_json = _midi_fx_config_json(config)
        if not with_source_index:
            _check(
                _get_lib().sonare_project_bake_midi_fx(
                    handle, _to_c_uint32(clip_id, "clip_id"), config_json
                )
            )
            return None
        expected = self.preview_midi_fx_count(clip_id, config)
        buffer = (ctypes.c_int32 * expected)() if expected else None
        written = ctypes.c_size_t(0)
        _check(
            _get_lib().sonare_project_bake_midi_fx_ex(
                handle,
                _to_c_uint32(clip_id, "clip_id"),
                config_json,
                buffer,
                _to_c_size_t(expected, "expected"),
                ctypes.byref(written),
            )
        )
        return (
            _from_c_int_array(buffer, min(written.value, expected))
            if buffer
            else _from_c_int_array(None, 0)
        )

    @_entry("Project.preview_midi_fx_count")
    def preview_midi_fx_count(self, clip_id: int, config: MidiFxConfig) -> int:
        """Count the events :meth:`bake_midi_fx` would produce, without baking."""
        count = ctypes.c_size_t(0)
        _check(
            _get_lib().sonare_project_preview_midi_fx_count(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                _midi_fx_config_json(config),
                ctypes.byref(count),
            )
        )
        return count.value

    @_entry("Project.validate_midi_notes")
    def validate_midi_notes(self, clip_id: int) -> NotePairValidation:
        """Check the clip's exported playback window for unmatched notes.

        The window is half-open, ``[0, length_ppq)``. A note still sounding at the
        clip end (its note-off at or past ``length_ppq``) is closed there by
        export, so it validates as paired.
        """
        result = SonareNotePairValidation()
        _check(
            _get_lib().sonare_project_validate_midi_notes(
                self._require_handle(),
                _to_c_uint32(clip_id, "clip_id"),
                ctypes.byref(result),
            )
        )
        return NotePairValidation(
            ok=bool(result.ok),
            unmatched_note_ons=int(result.unmatched_note_ons),
            unmatched_note_offs=int(result.unmatched_note_offs),
        )

    @staticmethod
    @_entry("Project.midi_note_on")
    def midi_note_on(
        ppq: float, group: int, channel: int, note: int, velocity: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 note-on event accepted by :meth:`set_midi_events`."""
        return _midi_event_packed(
            "sonare_midi_note_on",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint8(velocity, "velocity"),
        )

    @staticmethod
    @_entry("Project.midi_note_off")
    def midi_note_off(
        ppq: float, group: int, channel: int, note: int, velocity: int = 0
    ) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 note-off event accepted by :meth:`set_midi_events`."""
        return _midi_event_packed(
            "sonare_midi_note_off",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint8(velocity, "velocity"),
        )

    @staticmethod
    @_entry("Project.midi_cc")
    def midi_cc(
        ppq: float, group: int, channel: int, controller: int, value: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 control-change event tuple."""
        return _midi_event_packed(
            "sonare_midi_cc",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(controller, "controller"),
            _to_c_uint8(value, "value"),
        )

    @staticmethod
    @_entry("Project.midi_poly_pressure")
    def midi_poly_pressure(
        ppq: float, group: int, channel: int, note: int, pressure: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 poly-pressure event tuple."""
        return _midi_event_packed(
            "sonare_midi_poly_pressure",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint8(pressure, "pressure"),
        )

    @staticmethod
    @_entry("Project.midi_program")
    def midi_program(ppq: float, group: int, channel: int, program: int) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 program-change event tuple."""
        return _midi_event_packed(
            "sonare_midi_program",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(program, "program"),
        )

    @staticmethod
    @_entry("Project.midi_channel_pressure")
    def midi_channel_pressure(
        ppq: float, group: int, channel: int, pressure: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 channel-pressure event tuple."""
        return _midi_event_packed(
            "sonare_midi_channel_pressure",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(pressure, "pressure"),
        )

    @staticmethod
    @_entry("Project.midi_pitch_bend")
    def midi_pitch_bend(ppq: float, group: int, channel: int, bend: int) -> ProjectMidiEvent:
        """Pack a MIDI 1.0 pitch-bend event tuple (`bend` is unsigned 14-bit)."""
        # The one uint16 in the family; a uint8 here would refuse most of the range.
        return _midi_event_packed(
            "sonare_midi_pitch_bend",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint16(bend, "bend"),
        )

    @staticmethod
    @_entry("Project.midi2_note_on")
    def midi2_note_on(
        ppq: float,
        group: int,
        channel: int,
        note: int,
        velocity16: int,
        attribute_type: int = 0,
        attribute_data: int = 0,
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 note-on event tuple (16-bit velocity; 0 still sounds)."""
        return _midi_event_packed(
            "sonare_midi2_note_on",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint16(velocity16, "velocity16"),
            _to_c_uint8(attribute_type, "attribute_type"),
            _to_c_uint16(attribute_data, "attribute_data"),
        )

    @staticmethod
    @_entry("Project.midi2_note_off")
    def midi2_note_off(
        ppq: float, group: int, channel: int, note: int, velocity16: int = 0
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 note-off event tuple (16-bit release velocity)."""
        return _midi_event_packed(
            "sonare_midi2_note_off",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint16(velocity16, "velocity16"),
        )

    @staticmethod
    @_entry("Project.midi2_cc")
    def midi2_cc(
        ppq: float, group: int, channel: int, controller: int, value32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 control-change event tuple (32-bit value)."""
        return _midi_event_packed(
            "sonare_midi2_cc",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(controller, "controller"),
            _to_c_uint32(value32, "value32"),
        )

    @staticmethod
    @_entry("Project.midi2_poly_pressure")
    def midi2_poly_pressure(
        ppq: float, group: int, channel: int, note: int, pressure32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 poly-pressure event tuple (32-bit pressure)."""
        return _midi_event_packed(
            "sonare_midi2_poly_pressure",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint32(pressure32, "pressure32"),
        )

    @staticmethod
    @_entry("Project.midi2_channel_pressure")
    def midi2_channel_pressure(
        ppq: float, group: int, channel: int, pressure32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 channel-pressure event tuple (32-bit pressure)."""
        return _midi_event_packed(
            "sonare_midi2_channel_pressure",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint32(pressure32, "pressure32"),
        )

    @staticmethod
    @_entry("Project.midi2_pitch_bend")
    def midi2_pitch_bend(ppq: float, group: int, channel: int, bend32: int) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 pitch-bend event tuple (unsigned 32-bit)."""
        return _midi_event_packed(
            "sonare_midi2_pitch_bend",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint32(bend32, "bend32"),
        )

    @staticmethod
    @_entry("Project.midi2_program")
    def midi2_program(
        ppq: float,
        group: int,
        channel: int,
        program: int,
        bank_valid: bool = False,
        bank_msb: int = 0,
        bank_lsb: int = 0,
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 program-change event tuple.

        The bank travels in the same message and is applied only when
        ``bank_valid`` is true. A non-zero ``bank_msb`` or ``bank_lsb`` with
        ``bank_valid`` false is refused with :class:`SonareValueError`: a bank the receiver would
        ignore is not packed."""
        return _midi_event_packed(
            "sonare_midi2_program",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(program, "program"),
            _to_c_int(1 if bank_valid else 0, "bank_valid"),
            _to_c_uint8(bank_msb, "bank_msb"),
            _to_c_uint8(bank_lsb, "bank_lsb"),
        )

    @staticmethod
    @_entry("Project.midi2_registered_controller")
    def midi2_registered_controller(
        ppq: float, group: int, channel: int, bank: int, index: int, value32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 registered controller (RPN) event tuple (32-bit value)."""
        return _midi_event_packed(
            "sonare_midi2_registered_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(bank, "bank"),
            _to_c_uint8(index, "index"),
            _to_c_uint32(value32, "value32"),
        )

    @staticmethod
    @_entry("Project.midi2_assignable_controller")
    def midi2_assignable_controller(
        ppq: float, group: int, channel: int, bank: int, index: int, value32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 assignable controller (NRPN) event tuple (32-bit value)."""
        return _midi_event_packed(
            "sonare_midi2_assignable_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(bank, "bank"),
            _to_c_uint8(index, "index"),
            _to_c_uint32(value32, "value32"),
        )

    @staticmethod
    @_entry("Project.midi2_relative_registered_controller")
    def midi2_relative_registered_controller(
        ppq: float, group: int, channel: int, bank: int, index: int, delta32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 relative registered controller event tuple (signed 32-bit delta)."""
        return _midi_event_packed(
            "sonare_midi2_relative_registered_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(bank, "bank"),
            _to_c_uint8(index, "index"),
            _to_c_int32(delta32, "delta32"),
        )

    @staticmethod
    @_entry("Project.midi2_relative_assignable_controller")
    def midi2_relative_assignable_controller(
        ppq: float, group: int, channel: int, bank: int, index: int, delta32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 relative assignable controller event tuple (signed 32-bit delta)."""
        return _midi_event_packed(
            "sonare_midi2_relative_assignable_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(bank, "bank"),
            _to_c_uint8(index, "index"),
            _to_c_int32(delta32, "delta32"),
        )

    @staticmethod
    @_entry("Project.midi2_registered_per_note_controller")
    def midi2_registered_per_note_controller(
        ppq: float, group: int, channel: int, note: int, index: int, value32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 registered per-note controller event tuple (32-bit value)."""
        return _midi_event_packed(
            "sonare_midi2_registered_per_note_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint8(index, "index"),
            _to_c_uint32(value32, "value32"),
        )

    @staticmethod
    @_entry("Project.midi2_assignable_per_note_controller")
    def midi2_assignable_per_note_controller(
        ppq: float, group: int, channel: int, note: int, index: int, value32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 assignable per-note controller event tuple (32-bit value)."""
        return _midi_event_packed(
            "sonare_midi2_assignable_per_note_controller",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint8(index, "index"),
            _to_c_uint32(value32, "value32"),
        )

    @staticmethod
    @_entry("Project.midi2_per_note_pitch_bend")
    def midi2_per_note_pitch_bend(
        ppq: float, group: int, channel: int, note: int, bend32: int
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 per-note pitch-bend event tuple (unsigned 32-bit)."""
        return _midi_event_packed(
            "sonare_midi2_per_note_pitch_bend",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_uint32(bend32, "bend32"),
        )

    @staticmethod
    @_entry("Project.midi2_per_note_management")
    def midi2_per_note_management(
        ppq: float, group: int, channel: int, note: int, detach: bool = False, reset: bool = False
    ) -> ProjectMidiEvent:
        """Pack a MIDI 2.0 per-note management event tuple.

        ``detach`` detaches per-note controllers from voices already sounding
        on ``note``; ``reset`` resets the note's per-note controllers."""
        return _midi_event_packed(
            "sonare_midi2_per_note_management",
            _validate_midi_event_ppq(ppq, "ppq"),
            _to_c_uint8(group, "group"),
            _to_c_uint8(channel, "channel"),
            _to_c_uint8(note, "note"),
            _to_c_int(1 if detach else 0, "detach"),
            _to_c_int(1 if reset else 0, "reset"),
        )

    # -- MIDI naming / GM tables (static-lifetime lookups) ------------------

    @staticmethod
    @_entry("Project.gm_instrument_name")
    def gm_instrument_name(program: int) -> str | None:
        """GM Level 1 instrument name for ``program`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_gm_instrument_name(_to_c_int(program, "program"))
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.gm_program_for_name")
    def gm_program_for_name(name: str) -> int:
        """Reverse GM instrument lookup.

        Raises:
            SonareValueError: If ``name`` is not a GM instrument name.
        """
        return _found_index(
            _get_lib().sonare_midi_gm_program_for_name(_utf8_arg(name, "name")),
            f"unknown instrument name {name!r}",
        )

    @staticmethod
    @_entry("Project.gm_family_name")
    def gm_family_name(family: int) -> str | None:
        """GM family name for ``family`` [0,15], or ``None``."""
        r = _get_lib().sonare_midi_gm_family_name(_to_c_int(family, "family"))
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.gm_family_first_program")
    def gm_family_first_program(family: int) -> int:
        """First GM program in ``family`` [0,15].

        Raises:
            SonareValueError: If ``family`` is outside 0..15.
        """
        return _found_index(
            _get_lib().sonare_midi_gm_family_first_program(_to_c_int(family, "family")),
            "family must be an integer in [0, 15]",
        )

    @staticmethod
    @_entry("Project.gm2_instrument_name")
    def gm2_instrument_name(bank_lsb: int, program: int) -> str | None:
        """GM2 melodic instrument name for ``bank_lsb`` + ``program``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_instrument_name(
            _to_c_int(bank_lsb, "bank_lsb"), _to_c_int(program, "program")
        )
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.gm_drum_name")
    def gm_drum_name(note: int) -> str | None:
        """GM drum name for ``note`` [35,81], or ``None``."""
        r = _get_lib().sonare_midi_gm_drum_name(_to_c_int(note, "note"))
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.gm_drum_note_for_name")
    def gm_drum_note_for_name(name: str) -> int:
        """Reverse GM drum lookup.

        Raises:
            SonareValueError: If ``name`` is not a GM drum name.
        """
        return _found_index(
            _get_lib().sonare_midi_gm_drum_note_for_name(_utf8_arg(name, "name")),
            f"unknown drum name {name!r}",
        )

    @staticmethod
    @_entry("Project.gm2_drum_set_name")
    def gm2_drum_set_name(bank_lsb: int) -> str | None:
        """GM2 drum-set name for ``bank_lsb``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_set_name(_to_c_int(bank_lsb, "bank_lsb"))
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.gm2_drum_name")
    def gm2_drum_name(bank_lsb: int, note: int) -> str | None:
        """GM2 drum name for ``bank_lsb`` + ``note``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_name(
            _to_c_int(bank_lsb, "bank_lsb"), _to_c_int(note, "note")
        )
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.midi_cc_name")
    def midi_cc_name(controller: int) -> str | None:
        """Standard MIDI CC name for ``controller`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_cc_name(_to_c_int(controller, "controller"))
        return r.decode("utf-8") if r else None

    @staticmethod
    @_entry("Project.midi_cc_index_for_name")
    def midi_cc_index_for_name(name: str) -> int:
        """Reverse standard MIDI CC lookup.

        Raises:
            SonareValueError: If ``name`` is not a standard MIDI CC name.
        """
        return _found_index(
            _get_lib().sonare_midi_cc_index_for_name(_utf8_arg(name, "name")),
            f"unknown controller name {name!r}",
        )

    @staticmethod
    @_entry("Project.per_note_controller_name")
    def per_note_controller_name(index: int) -> str | None:
        """MIDI 2.0 registered per-note controller name for ``index``, or ``None``."""
        r = _get_lib().sonare_midi_per_note_controller_name(_to_c_int(index, "index"))
        return r.decode("utf-8") if r else None

    # -- MIDI pure conversion helpers ---------------------------------------

    @staticmethod
    @_entry("Project.midi_bank_program")
    def midi_bank_program(
        ppq: float,
        group: int,
        channel: int,
        bank_msb: int,
        bank_lsb: int,
        program: int,
    ) -> list[ProjectMidiEvent]:
        """Lower a bank/program selection to MIDI 1.0 bank-select + program events.

        Returns the emitted events (Bank MSB CC, Bank LSB CC, then Program
        Change) at ``ppq``.
        """
        lib = _get_lib()
        events = (SonareMidiEventPod * 3)()
        out_count = ctypes.c_size_t()
        _check(
            lib.sonare_midi_bank_program(
                _narrow_double(ppq, "ppq"),
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_int(bank_msb, "bank_msb"),
                _to_c_int(bank_lsb, "bank_lsb"),
                _to_c_int(program, "program"),
                events,
                ctypes.c_size_t(3),
                ctypes.byref(out_count),
            )
        )
        return [_midi_event_from_c(events[i]) for i in range(int(out_count.value))]

    @staticmethod
    @_entry("Project.midi_route_events")
    def midi_route_events(
        events: Sequence[ProjectMidiEvent],
        config: Mapping[str, int | None] | None = None,
    ) -> MidiRouteResult:
        """Route events through the RT MidiRouter filter / remap / thru logic.

        ``config`` is an optional mapping with ``filter_group`` (0..15) /
        ``filter_channel`` (0..15), absent or ``None`` for any;
        ``remap_channel`` (0..15), absent or ``None`` for no remap; and
        ``thru`` (1 = pass non-matching events through, default). A negative
        group or channel is refused rather than read as "any".
        """
        lib = _get_lib()
        c_in, n = _midi_events_to_c(events, "events")
        cfg_map: Mapping[str, int | None] = config or {}
        # The C ABI spells "any" / "no remap" as -1; only absence reaches it.
        cfg = SonareMidiRouteConfig(
            filter_group=_absent_or_in_range(cfg_map.get("filter_group"), "filter_group", 15, -1),
            filter_channel=_absent_or_in_range(
                cfg_map.get("filter_channel"), "filter_channel", 15, -1
            ),
            remap_channel=_absent_or_in_range(
                cfg_map.get("remap_channel"), "remap_channel", 15, -1
            ),
            thru=cfg_map.get("thru", 1),
        )
        out = (SonareMidiEventPod * n)()
        out_count = ctypes.c_size_t()
        overflowed = ctypes.c_int()
        overflow_count = ctypes.c_uint32()
        _check(
            lib.sonare_midi_route_events(
                c_in if n else None,
                _to_c_size_t(n, "n"),
                ctypes.byref(cfg),
                out,
                _to_c_size_t(n, "n"),
                ctypes.byref(out_count),
                ctypes.byref(overflowed),
                ctypes.byref(overflow_count),
            )
        )
        routed = [_midi_event_from_c(out[i]) for i in range(int(out_count.value))]
        return MidiRouteResult(
            events=routed,
            overflowed=bool(overflowed.value),
            overflow_count=int(overflow_count.value),
        )

    @staticmethod
    @_entry("Project.midi_cc_learn")
    def midi_cc_learn(
        events: Sequence[ProjectMidiEvent],
        param_id: int,
        min_value: float = 0.0,
        max_value: float = 1.0,
        min_movement: int = 0,
    ) -> MidiCcBinding | None:
        """Run MIDI learn over ``events`` and return the learned binding.

        ``min_movement`` is the activity threshold in 7-bit CC units (0-127).
        Returns ``None`` when no binding is learned (native
        ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        c_in, n = _midi_events_to_c(events, "events")
        out_binding = SonareMidiCcBinding()
        rc = lib.sonare_midi_cc_learn(
            c_in if n else None,
            _to_c_size_t(n, "n"),
            _to_c_uint32(param_id, "param_id"),
            _to_c_float(min_value, "min_value"),
            _to_c_float(max_value, "max_value"),
            ctypes.c_uint8(_narrow_int(min_movement, "min_movement", 0, 127)),
            ctypes.byref(out_binding),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return _cc_binding_from_c(out_binding)

    @staticmethod
    @_entry("Project.midi_cc_to_breakpoint")
    def midi_cc_to_breakpoint(
        bindings: Sequence[MidiCcBinding],
        event: ProjectMidiEvent,
    ) -> AutomationPoint | None:
        """Convert one CC ``event`` to an automation breakpoint via a binding table.

        Returns ``None`` when the event does not match any binding (native
        ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        ev = _midi_event_to_c(event, "event")
        pt = SonareAutomationPoint()
        rc = lib.sonare_midi_cc_to_breakpoint(
            c_bindings if m else None,
            _to_c_size_t(m, "m"),
            ctypes.byref(ev),
            ctypes.byref(pt),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        curve = cast(AutomationCurve, _AUTOMATION_CURVES[int(pt.curve_to_next)])
        return AutomationPoint(float(pt.ppq), float(pt.value), curve)

    @staticmethod
    @_entry("Project.midi_param_to_cc")
    def midi_param_to_cc(
        bindings: Sequence[MidiCcBinding],
        param_id: int,
        unit_value: float,
        group: int,
        ppq: float = 0.0,
    ) -> ProjectMidiEvent | None:
        """Convert an automation parameter value back to a CC event.

        Returns ``None`` when ``param_id`` is not bound (native ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        out_event = SonareMidiEventPod()
        rc = lib.sonare_midi_param_to_cc(
            c_bindings if m else None,
            _to_c_size_t(m, "m"),
            _to_c_uint32(param_id, "param_id"),
            _to_c_float(unit_value, "unit_value"),
            _to_c_uint8(group, "group"),
            _to_c_double(ppq, "ppq"),
            ctypes.byref(out_event),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return _midi_event_from_c(out_event)

    # -- MIR ----------------------------------------------------------------
