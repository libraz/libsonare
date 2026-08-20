# ruff: noqa: F405
"""Project MIDI editing, routing, and catalog methods."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_model import (
    _cc_binding_from_c,
    _cc_binding_to_c,
    _midi_event_tuple,
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
    SonareAutomationPoint,
    SonareMidiCcBinding,
    SonareMidiEventPod,
    SonareMidiRouteConfig,
    SonareNotePairValidation,
    SonareValueError,
    _check,
    _get_lib,
)


class _ProjectMidiMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def set_midi_events(
        self,
        clip_id: int,
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]] | np.ndarray,
    ) -> None:
        """Replace a MIDI clip's entire event list.

        Each event is ``(ppq, data0, data1)`` (the first two UMP-1.0 words of a
        channel-voice message; stored opaquely). Pass an empty sequence to clear.
        """
        rows = list(events)
        count = len(rows)
        c_events = (SonareMidiEventPod * count)()
        for i, ev in enumerate(rows):
            if len(ev) < 3:
                raise SonareValueError(f"events[{i}] must contain (ppq, data0, data1)")
            ppq, data0, data1 = ev[0], ev[1], ev[2]
            c_events[i].ppq = _validate_midi_event_ppq(ppq, f"events[{i}].ppq")
            c_events[i].data0 = _validate_midi_event_word(data0, f"events[{i}].data0")
            c_events[i].data1 = _validate_midi_event_word(data1, f"events[{i}].data1")
        _check(
            _get_lib().sonare_project_set_midi_events(
                self._require_handle(),
                int(clip_id),
                c_events if count else None,
                ctypes.c_size_t(count),
            )
        )

    def import_smf(self, data: bytes) -> int:
        """Import an in-memory SMF buffer; return the first added clip id.

        Raises :class:`SonareError` on malformed or partially truncated input
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

    def set_program(self, clip_id: int, program: int, bank: int = -1) -> None:
        """Set a MIDI clip's channel-0 program / bank at source PPQ 0.

        ``bank`` defaults to ``-1`` (no Bank Select emitted), matching
        :meth:`set_program_on_channel`; pass ``>= 0`` to emit a Bank Select.
        """
        _check(
            _get_lib().sonare_project_set_program(
                self._require_handle(), int(clip_id), int(program), int(bank)
            )
        )

    def set_program_on_channel(
        self, clip_id: int, group: int, channel: int, program: int, bank: int = -1
    ) -> None:
        """Set a MIDI clip's program / bank for one UMP group and channel."""
        _check(
            _get_lib().sonare_project_set_program_on_channel(
                self._require_handle(),
                int(clip_id),
                int(group),
                int(channel),
                int(program),
                int(bank),
            )
        )

    def set_midi_fx(self, clip_id: int, config_json: str) -> None:
        """Backward alias that destructively bakes a clip's MIDI-FX chain."""
        _check(
            _get_lib().sonare_project_set_midi_fx(
                self._require_handle(),
                int(clip_id),
                config_json.encode("utf-8"),
            )
        )

    def bake_midi_fx(
        self,
        clip_id: int,
        config_json: str,
        *,
        with_source_index: bool = False,
    ) -> list[int] | None:
        """Destructively bake a clip's MIDI-FX chain from JSON.

        Canonical name matching the Node / WASM ``bakeMidiFx`` surface. Large
        clips are drained without truncation; failure leaves the original clip
        unchanged. :meth:`set_midi_fx` is a backward alias for this same
        destructive operation.

        With ``with_source_index`` set, returns one entry per transformed event
        in canonical order: the index of the input event it derives from, or
        ``-1`` for an event with no originating input. Chord and arpeggiator
        fan-out makes several outputs share one source index, so a caller that
        treats the first output per index as the same event and the rest as
        newly generated can carry a selection across the bake. Returns ``None``
        otherwise.
        """
        handle = self._require_handle()
        config = config_json.encode("utf-8")
        if not with_source_index:
            _check(_get_lib().sonare_project_bake_midi_fx(handle, int(clip_id), config))
            return None
        expected = self.preview_midi_fx_count(clip_id, config_json)
        buffer = (ctypes.c_int32 * expected)() if expected else None
        written = ctypes.c_size_t(0)
        _check(
            _get_lib().sonare_project_bake_midi_fx_ex(
                handle,
                int(clip_id),
                config,
                buffer,
                ctypes.c_size_t(expected),
                ctypes.byref(written),
            )
        )
        return list(buffer[: min(written.value, expected)]) if buffer else []

    def preview_midi_fx_count(self, clip_id: int, config_json: str) -> int:
        """Count the events :meth:`bake_midi_fx` would produce, without baking."""
        count = ctypes.c_size_t(0)
        _check(
            _get_lib().sonare_project_preview_midi_fx_count(
                self._require_handle(),
                int(clip_id),
                config_json.encode("utf-8"),
                ctypes.byref(count),
            )
        )
        return count.value

    def validate_midi_notes(self, clip_id: int) -> NotePairValidation:
        """Check the clip's exported playback window for unmatched notes."""
        result = SonareNotePairValidation()
        _check(
            _get_lib().sonare_project_validate_midi_notes(
                self._require_handle(),
                int(clip_id),
                ctypes.byref(result),
            )
        )
        return NotePairValidation(
            ok=bool(result.ok),
            unmatched_note_ons=int(result.unmatched_note_ons),
            unmatched_note_offs=int(result.unmatched_note_offs),
        )

    @staticmethod
    def midi_note_on(
        ppq: float, group: int, channel: int, note: int, velocity: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-on event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_on", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_note_off(
        ppq: float, group: int, channel: int, note: int, velocity: int = 0
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 note-off event tuple accepted by :meth:`set_midi_events`."""
        return _midi_event_tuple("sonare_midi_note_off", ppq, group, channel, note, velocity)

    @staticmethod
    def midi_cc(
        ppq: float, group: int, channel: int, controller: int, value: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 control-change event tuple."""
        return _midi_event_tuple("sonare_midi_cc", ppq, group, channel, controller, value)

    @staticmethod
    def midi_poly_pressure(
        ppq: float, group: int, channel: int, note: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 poly-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_poly_pressure", ppq, group, channel, note, pressure)

    @staticmethod
    def midi_program(ppq: float, group: int, channel: int, program: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 program-change event tuple."""
        return _midi_event_tuple("sonare_midi_program", ppq, group, channel, program)

    @staticmethod
    def midi_channel_pressure(
        ppq: float, group: int, channel: int, pressure: int
    ) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 channel-pressure event tuple."""
        return _midi_event_tuple("sonare_midi_channel_pressure", ppq, group, channel, pressure)

    @staticmethod
    def midi_pitch_bend(ppq: float, group: int, channel: int, bend: int) -> tuple[float, int, int]:
        """Pack a MIDI 1.0 pitch-bend event tuple (`bend` is unsigned 14-bit)."""
        return _midi_event_tuple("sonare_midi_pitch_bend", ppq, group, channel, bend)

    # -- MIDI naming / GM tables (static-lifetime lookups) ------------------

    @staticmethod
    def gm_instrument_name(program: int) -> str | None:
        """GM Level 1 instrument name for ``program`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_gm_instrument_name(int(program))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_program_for_name(name: str | None) -> int:
        """Reverse GM instrument lookup; ``-1`` when unknown / ``None``."""
        return int(
            _get_lib().sonare_midi_gm_program_for_name(name.encode("utf-8") if name else None)
        )

    @staticmethod
    def gm_family_name(family: int) -> str | None:
        """GM family name for ``family`` [0,15], or ``None``."""
        r = _get_lib().sonare_midi_gm_family_name(int(family))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_family_first_program(family: int) -> int:
        """First GM program in ``family`` [0,15], or ``-1``."""
        return int(_get_lib().sonare_midi_gm_family_first_program(int(family)))

    @staticmethod
    def gm2_instrument_name(bank_lsb: int, program: int) -> str | None:
        """GM2 melodic instrument name for ``bank_lsb`` + ``program``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_instrument_name(int(bank_lsb), int(program))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_drum_name(note: int) -> str | None:
        """GM drum name for ``note`` [35,81], or ``None``."""
        r = _get_lib().sonare_midi_gm_drum_name(int(note))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm_drum_note_for_name(name: str | None) -> int:
        """Reverse GM drum lookup; ``-1`` when unknown / ``None``."""
        return int(
            _get_lib().sonare_midi_gm_drum_note_for_name(name.encode("utf-8") if name else None)
        )

    @staticmethod
    def gm2_drum_set_name(bank_lsb: int) -> str | None:
        """GM2 drum-set name for ``bank_lsb``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_set_name(int(bank_lsb))
        return r.decode("utf-8") if r else None

    @staticmethod
    def gm2_drum_name(bank_lsb: int, note: int) -> str | None:
        """GM2 drum name for ``bank_lsb`` + ``note``, or ``None``."""
        r = _get_lib().sonare_midi_gm2_drum_name(int(bank_lsb), int(note))
        return r.decode("utf-8") if r else None

    @staticmethod
    def midi_cc_name(controller: int) -> str | None:
        """Standard MIDI CC name for ``controller`` [0,127], or ``None``."""
        r = _get_lib().sonare_midi_cc_name(int(controller))
        return r.decode("utf-8") if r else None

    @staticmethod
    def midi_cc_index_for_name(name: str | None) -> int:
        """Reverse standard MIDI CC lookup; ``-1`` when unknown / ``None``."""
        return int(_get_lib().sonare_midi_cc_index_for_name(name.encode("utf-8") if name else None))

    @staticmethod
    def per_note_controller_name(index: int) -> str | None:
        """MIDI 2.0 registered per-note controller name for ``index``, or ``None``."""
        r = _get_lib().sonare_midi_per_note_controller_name(int(index))
        return r.decode("utf-8") if r else None

    # -- MIDI pure conversion helpers ---------------------------------------

    @staticmethod
    def midi_bank_program(
        ppq: float,
        group: int,
        channel: int,
        bank_msb: int,
        bank_lsb: int,
        program: int,
    ) -> list[tuple[float, int, int]]:
        """Lower a bank/program selection to MIDI 1.0 bank-select + program events.

        Returns the emitted ``(ppq, data0, data1)`` events (Bank MSB CC, Bank LSB
        CC, then Program Change) at ``ppq``.
        """
        lib = _get_lib()
        events = (SonareMidiEventPod * 3)()
        out_count = ctypes.c_size_t()
        _check(
            lib.sonare_midi_bank_program(
                float(ppq),
                int(group),
                int(channel),
                int(bank_msb),
                int(bank_lsb),
                int(program),
                events,
                ctypes.c_size_t(3),
                ctypes.byref(out_count),
            )
        )
        return [
            (float(events[i].ppq), int(events[i].data0), int(events[i].data1))
            for i in range(int(out_count.value))
        ]

    @staticmethod
    def midi_route_events(
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]],
        config: Mapping[str, int] | None = None,
    ) -> MidiRouteResult:
        """Route events through the RT MidiRouter filter / remap / thru logic.

        ``config`` is an optional mapping with ``filter_group`` /
        ``filter_channel`` (``-1`` = any), ``remap_channel`` (``-1`` = no remap)
        and ``thru`` (1 = pass non-matching events through, default).
        """
        lib = _get_lib()
        rows = list(events)
        n = len(rows)
        c_in = (SonareMidiEventPod * n)()
        for i, ev in enumerate(rows):
            seq = tuple(ev)
            if len(seq) < 3:
                raise SonareValueError(f"events[{i}] must contain (ppq, data0, data1)")
            c_in[i].ppq = _validate_midi_event_ppq(seq[0], f"events[{i}].ppq")
            c_in[i].data0 = _validate_midi_event_word(seq[1], f"events[{i}].data0")
            c_in[i].data1 = _validate_midi_event_word(seq[2], f"events[{i}].data1")
        cfg_map: Mapping[str, int] = config or {}
        cfg = SonareMidiRouteConfig(
            filter_group=int(cfg_map.get("filter_group", -1)),
            filter_channel=int(cfg_map.get("filter_channel", -1)),
            remap_channel=int(cfg_map.get("remap_channel", -1)),
            thru=int(cfg_map.get("thru", 1)),
        )
        out = (SonareMidiEventPod * n)()
        out_count = ctypes.c_size_t()
        overflowed = ctypes.c_int()
        overflow_count = ctypes.c_uint32()
        _check(
            lib.sonare_midi_route_events(
                c_in if n else None,
                ctypes.c_size_t(n),
                ctypes.byref(cfg),
                out,
                ctypes.c_size_t(n),
                ctypes.byref(out_count),
                ctypes.byref(overflowed),
                ctypes.byref(overflow_count),
            )
        )
        routed = [
            (float(out[i].ppq), int(out[i].data0), int(out[i].data1))
            for i in range(int(out_count.value))
        ]
        return MidiRouteResult(
            events=routed,
            overflowed=bool(overflowed.value),
            overflow_count=int(overflow_count.value),
        )

    @staticmethod
    def midi_cc_learn(
        events: Sequence[tuple[float, int, int]] | Sequence[Sequence[float]],
        param_id: int,
        min_value: float = 0.0,
        max_value: float = 1.0,
        min_movement: int = 0,
    ) -> MidiCcBinding | None:
        """Run MIDI learn over ``events`` and return the learned binding.

        Returns ``None`` when no binding is learned (native
        ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(events)
        n = len(rows)
        c_in = (SonareMidiEventPod * n)()
        for i, ev in enumerate(rows):
            seq = tuple(ev)
            if len(seq) < 3:
                raise SonareValueError(f"events[{i}] must contain (ppq, data0, data1)")
            c_in[i].ppq = _validate_midi_event_ppq(seq[0], f"events[{i}].ppq")
            c_in[i].data0 = _validate_midi_event_word(seq[1], f"events[{i}].data0")
            c_in[i].data1 = _validate_midi_event_word(seq[2], f"events[{i}].data1")
        out_binding = SonareMidiCcBinding()
        rc = lib.sonare_midi_cc_learn(
            c_in if n else None,
            ctypes.c_size_t(n),
            ctypes.c_uint32(int(param_id) & 0xFFFFFFFF),
            ctypes.c_float(float(min_value)),
            ctypes.c_float(float(max_value)),
            ctypes.c_uint8(int(min_movement) & 0xFF),
            ctypes.byref(out_binding),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return _cc_binding_from_c(out_binding)

    @staticmethod
    def midi_cc_to_breakpoint(
        bindings: Sequence[MidiCcBinding | Mapping[str, object]],
        event: tuple[float, int, int] | Sequence[float],
    ) -> tuple[float, float, int] | None:
        """Convert one CC ``event`` to an automation breakpoint via a binding table.

        Returns ``(ppq, value, curve_to_next)`` or ``None`` when the event does
        not match any binding (native ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        seq = tuple(event)
        if len(seq) < 3:
            raise SonareValueError("event must contain (ppq, data0, data1)")
        ev = SonareMidiEventPod(
            ppq=_validate_midi_event_ppq(seq[0], "event.ppq"),
            data0=_validate_midi_event_word(seq[1], "event.data0"),
            data1=_validate_midi_event_word(seq[2], "event.data1"),
        )
        pt = SonareAutomationPoint()
        rc = lib.sonare_midi_cc_to_breakpoint(
            c_bindings if m else None,
            ctypes.c_size_t(m),
            ctypes.byref(ev),
            ctypes.byref(pt),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return (float(pt.ppq), float(pt.value), int(pt.curve_to_next))

    @staticmethod
    def midi_param_to_cc(
        bindings: Sequence[MidiCcBinding | Mapping[str, object]],
        param_id: int,
        unit_value: float,
        group: int,
        ppq: float = 0.0,
    ) -> tuple[float, int, int] | None:
        """Convert an automation parameter value back to a CC event.

        Returns ``(ppq, data0, data1)`` or ``None`` when ``param_id`` is not
        bound (native ``SONARE_ERROR_INVALID_STATE``).
        """
        lib = _get_lib()
        rows = list(bindings)
        m = len(rows)
        c_bindings = (SonareMidiCcBinding * m)(*[_cc_binding_to_c(b) for b in rows])
        out_event = SonareMidiEventPod()
        rc = lib.sonare_midi_param_to_cc(
            c_bindings if m else None,
            ctypes.c_size_t(m),
            ctypes.c_uint32(int(param_id) & 0xFFFFFFFF),
            ctypes.c_float(float(unit_value)),
            ctypes.c_uint8(int(group) & 0xFF),
            ctypes.c_double(float(ppq)),
            ctypes.byref(out_event),
        )
        if rc == SONARE_ERROR_INVALID_STATE:
            return None
        _check(rc)
        return (float(out_event.ppq), int(out_event.data0), int(out_event.data1))

    # -- MIR ----------------------------------------------------------------
