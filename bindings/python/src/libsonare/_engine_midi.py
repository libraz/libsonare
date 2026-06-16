"""Live-MIDI and realtime-instrument methods for the realtime engine.

Split out of ``engine.py`` as a mixin to keep ``RealtimeEngine`` to a
manageable size. The methods depend only on ``_require_handle`` (provided by
the concrete :class:`RealtimeEngine`); this is not a public class on its own.
"""

from __future__ import annotations

import ctypes
from typing import TYPE_CHECKING

from ._project import BuiltinSynthConfig, Sf2InstrumentConfig, SynthPatch, _synth_patch_arg
from ._runtime import _check, _get_lib


class _EngineMidiMixin:
    """Live-MIDI / instrument-binding methods mixed into ``RealtimeEngine``."""

    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

    def push_midi_cc(
        self,
        destination_id: int,
        group: int,
        channel: int,
        controller: int,
        value: int,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate (live) MIDI control change to a MIDI destination.

        Values are 7-bit (``controller`` / ``value`` in 0..127); ``channel`` and
        ``group`` in 0..15. ``render_frame`` is the render-frame time to apply,
        or ``-1`` for immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_cc(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(controller),
                int(value),
                int(render_frame),
            )
        )

    def push_midi_panic(self, render_frame: int = -1) -> None:
        """Queue a MIDI panic (all-notes-off) releasing every sounding note.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_panic"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_push_midi_panic(self._require_handle(), int(render_frame)))

    # -- live MIDI instruments / CC bindings / input source -----------------

    def set_builtin_instrument(
        self, config: BuiltinSynthConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind the built-in polyphonic synth to ``destination_id`` (default 0).

        ``config`` is a :class:`BuiltinSynthConfig` patch; ``None`` installs the
        default sine patch. After binding, MIDI events routed to that
        destination render through the built-in synth.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_builtin_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        cfg = (config if config is not None else BuiltinSynthConfig())._to_c()
        _check(
            lib.sonare_engine_set_builtin_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(cfg)
            )
        )

    def set_synth_instrument(
        self, patch: SynthPatch | str | None = None, destination_id: int = 0
    ) -> None:
        """Bind the patch-driven NativeSynth to ``destination_id`` (default 0).

        ``patch`` is a :class:`SynthPatch`, a preset name string
        (``"saw-lead"`` or ``"va:saw-lead"``; see :func:`synth_preset_names`),
        or ``None`` for the default subtractive patch. The patch resolves
        exactly like :meth:`Project.bounce_with_synth_instrument`. After
        binding, live MIDI input and scheduled MIDI clips routed to that
        destination render through the synth. Raises :class:`SonareError` for
        an unknown preset name.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_synth_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        c_patch = _synth_patch_arg(patch)._to_c()
        _check(
            lib.sonare_engine_set_synth_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(c_patch)
            )
        )

    def load_soundfont(self, data: bytes | bytearray | memoryview) -> None:
        """Load (parse) SoundFont 2 bytes into the engine.

        Replaces any previously loaded SoundFont (already-bound SF2 instruments
        keep the SoundFont they were created with); the input buffer is not
        referenced after the call. Raises :class:`SonareError` on malformed
        input.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_load_soundfont"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        buf = bytes(data)
        if not buf:
            raise ValueError("SoundFont data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_engine_load_soundfont(
                self._require_handle(), c_data, ctypes.c_size_t(len(buf))
            )
        )

    def set_sf2_instrument(
        self, config: Sf2InstrumentConfig | None = None, destination_id: int = 0
    ) -> None:
        """Bind a GS-compatible SoundFont player to ``destination_id`` (default 0).

        ``config`` is an :class:`Sf2InstrumentConfig` patch; ``None`` installs
        the defaults. After binding, live MIDI input and scheduled MIDI clips
        routed to that destination render through the player (16 MIDI
        channels, channel 10 drums, GS NRPN part edits, GS/GM SysEx resets).
        Without a loaded SoundFont (:meth:`load_soundfont`) — or for programs
        the SoundFont does not cover — notes play through the built-in
        synthesizer GM fallback bank (the data-free floor).
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_sf2_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        cfg = (config if config is not None else Sf2InstrumentConfig())._to_c()
        _check(
            lib.sonare_engine_set_sf2_instrument(
                self._require_handle(), int(destination_id), ctypes.byref(cfg)
            )
        )

    def clear_midi_instrument(self, destination_id: int = 0) -> None:
        """Clear any realtime instrument bound to ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_instrument(self._require_handle(), int(destination_id)))

    def midi_instrument_count(self) -> int:
        """Return the number of bound realtime MIDI instruments."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_instrument_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(lib.sonare_engine_midi_instrument_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def bind_midi_cc(
        self,
        channel: int,
        controller: int,
        param_id: int,
        min_value: float = 0.0,
        max_value: float = 1.0,
    ) -> None:
        """Bind a live MIDI CC to an engine automation parameter.

        Incoming CC values on ``channel`` / ``controller`` are mapped onto
        ``[min_value, max_value]`` and applied to ``param_id``.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_bind_midi_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_bind_midi_cc(
                self._require_handle(),
                int(channel),
                int(controller),
                int(param_id),
                float(min_value),
                float(max_value),
            )
        )

    def clear_midi_cc_bindings(self) -> None:
        """Clear all live MIDI CC to parameter bindings."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_cc_bindings"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_cc_bindings(self._require_handle()))

    def midi_cc_binding_count(self) -> int:
        """Return the number of live MIDI CC bindings."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_cc_binding_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(lib.sonare_engine_midi_cc_binding_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def clear_midi_fx(self, destination_id: int = 0) -> None:
        """Clear the live MIDI-FX insert for ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_fx"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_fx(self._require_handle(), int(destination_id)))

    def set_midi_input_source(self, destination_id: int = 0) -> None:
        """Enable the engine-owned live MIDI input source for ``destination_id``.

        Hosts can then push timestamped events with
        :meth:`push_midi_input_note_on` / ``_note_off`` / ``_cc``; the engine
        drains them at block boundaries.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_input_source"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_set_midi_input_source(self._require_handle(), int(destination_id)))

    def clear_midi_input_source(self) -> None:
        """Clear the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_input_source"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(lib.sonare_engine_clear_midi_input_source(self._require_handle()))

    def midi_input_pending_count(self) -> int:
        """Number of queued events in the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_input_pending_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(
            lib.sonare_engine_midi_input_pending_count(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def push_midi_input_note_on(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a note-on into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_note_on"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_note_on(
                self._require_handle(),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(port_time_samples),
            )
        )

    def push_midi_input_note_off(
        self,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a note-off into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_note_off"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_note_off(
                self._require_handle(),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(port_time_samples),
            )
        )

    def push_midi_input_cc(
        self,
        group: int,
        channel: int,
        controller: int,
        value: int,
        port_time_samples: int = 0,
    ) -> None:
        """Queue a control change into the engine-owned live MIDI input source."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_input_cc"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_input_cc(
                self._require_handle(),
                int(group),
                int(channel),
                int(controller),
                int(value),
                int(port_time_samples),
            )
        )

    def push_midi_note_on(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate live MIDI note-on to a MIDI destination.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_note_on"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_note_on(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(render_frame),
            )
        )

    def push_midi_note_off(
        self,
        destination_id: int,
        group: int,
        channel: int,
        note: int,
        velocity: int = 0,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate live MIDI note-off to a MIDI destination.

        ``render_frame`` is the render-frame time to apply, or ``-1`` for
        immediate.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_note_off"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_push_midi_note_off(
                self._require_handle(),
                int(destination_id),
                int(group),
                int(channel),
                int(note),
                int(velocity),
                int(render_frame),
            )
        )
