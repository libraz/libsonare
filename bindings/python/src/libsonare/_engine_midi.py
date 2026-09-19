"""Live-MIDI and realtime-instrument methods for the realtime engine.

Split out of ``engine.py`` as a mixin to keep ``RealtimeEngine`` to a
manageable size. The methods depend only on ``_require_handle`` (provided by
the concrete :class:`RealtimeEngine`); this is not a public class on its own.
"""

from __future__ import annotations

import ctypes
from typing import TYPE_CHECKING

from ._ffi_types_mastering_project import SonareControllerBinding
from ._project import (
    BuiltinSynthConfig,
    MidiCcBinding,
    SampleBank,
    Sf2InstrumentConfig,
    SynthPatch,
    _cc_binding_to_c,
    _synth_patch_arg,
)
from ._project_synth import (
    _articulation_name,
    _articulation_value,
    _controller_axis_value,
    _controller_input_value,
)
from ._runtime import (
    SonareValueError,
    _check,
    _get_lib,
    _to_c_float,
    _to_c_int,
    _to_c_int64,
    _to_c_uint8,
    _to_c_uint32,
)


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
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(controller, "controller"),
                _to_c_uint8(value, "value"),
                _to_c_int64(render_frame, "render_frame"),
            )
        )

    def push_midi_sysex(
        self,
        destination_id: int,
        data: bytes | bytearray | memoryview,
        render_frame: int = -1,
    ) -> None:
        """Queue an immediate (live) MIDI SysEx message to a MIDI destination.

        ``data`` is the full SysEx frame including the leading ``0xF0`` and
        trailing ``0xF7`` (1..512 bytes). ``render_frame`` is the render-frame
        time to apply, or ``-1`` for immediate. Raises :class:`SonareError`
        if the payload exceeds the accepted size.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_push_midi_sysex"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        buf = bytes(data)
        if not buf:
            raise SonareValueError("SysEx data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_engine_push_midi_sysex(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                c_data,
                ctypes.c_size_t(len(buf)),
                _to_c_int64(render_frame, "render_frame"),
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
        _check(
            lib.sonare_engine_push_midi_panic(
                self._require_handle(), _to_c_int64(render_frame, "render_frame")
            )
        )

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
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(cfg),
            )
        )

    def set_synth_instrument(
        self,
        patch: SynthPatch | str | None = None,
        destination_id: int = 0,
        *,
        sample_bank: SampleBank | None = None,
    ) -> None:
        """Bind the patch-driven NativeSynth to ``destination_id`` (default 0).

        ``patch`` is a :class:`SynthPatch`, a preset name string
        (``"saw-lead"`` or ``"va:saw-lead"``; see :func:`synth_preset_names`),
        or ``None`` for the default subtractive patch. The patch resolves
        exactly like :meth:`Project.bounce_with_synth_instrument`. After
        binding, live MIDI input and scheduled MIDI clips routed to that
        destination render through the synth. Raises :class:`SonareError` for
        an unknown preset name.

        The :class:`SampleBank` a ``"sample"`` engine patch reads normally
        travels on :attr:`SynthPatch.sample_bank`, so the field means the same
        thing here as it does in a bounce. The ``sample_bank`` argument is for
        the case that cannot carry one -- a bare preset-name string -- and wins
        over the patch's own field when both are given. Unlike the bounce, the
        engine takes a SHARE of the bank, so the caller may close its own handle
        straight afterwards. A sample patch bound without a bank is accepted and
        renders silence, the same way one naming a keymap set the bank lacks
        does.

        Control-thread only: this is a structural mutation, so do not call it
        concurrently with :meth:`process`.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_synth_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        resolved = _synth_patch_arg(patch)
        bank = sample_bank if sample_bank is not None else resolved.sample_bank
        c_patch = resolved._to_c()
        if bank is None:
            _check(
                lib.sonare_engine_set_synth_instrument(
                    self._require_handle(),
                    _to_c_uint32(destination_id, "destination_id"),
                    ctypes.byref(c_patch),
                )
            )
            return
        if not hasattr(lib, "sonare_engine_set_synth_instrument_with_bank"):
            raise RuntimeError("libsonare was built without the sample-bank ABI")
        _check(
            lib.sonare_engine_set_synth_instrument_with_bank(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(c_patch),
                bank._require_handle(),
            )
        )

    def resolve_instrument_automation_id(self, param_name: str, destination_id: int = 0) -> int:
        """Resolve a hosted instrument's continuous parameter to its automation id.

        The returned id drives :meth:`set_automation_lane`,
        :meth:`set_parameter`, or :meth:`set_parameter_smoothed` exactly like a
        fader/pan or strip-insert id, so an instrument parameter follows a
        breakpoint lane at audio-block precision, live and offline alike.

        For the NativeSynth (:meth:`set_synth_instrument`) ``param_name`` is one
        of the continuous :class:`SynthPatch` fields, spelled as the JSON key:
        ``gain``, ``busDrive``, ``cutoffHz``, ``resonanceQ``, ``drive``,
        ``keyTrack``, ``envToCutoffCents``, ``velToCutoffCents``,
        ``ampAttackMs``, ``ampDecayMs``, ``ampSustain``, ``ampReleaseMs``,
        ``filterAttackMs``, ``filterDecayMs``, ``filterSustain``,
        ``filterReleaseMs``, ``lfoRateHz``, ``lfoToPitchCents``, ``lfo2RateHz``,
        ``glideMs``, ``bodyMix``, ``stereoSpread``, ``detuneCents``,
        ``driftCents``, ``pitchOffsetCents``.

        Structural fields (preset, engine mode, waveform, filter model, unison,
        polyphony, body type, mod routings) are not automatable: they resize
        voice pools or swap DSP topology, which is not audio-thread safe. Rebind
        the instrument with a new patch instead.

        ``gain``, ``busDrive``, ``cutoffHz``, ``resonanceQ``,
        ``envToCutoffCents``, ``lfoToPitchCents`` and ``pitchOffsetCents`` reach
        voices that are already sounding from the next block; the rest are
        cached into per-voice state at note-on and take effect from the next
        note.

        Raises :class:`SonareError` when the destination has no bound
        instrument, the instrument exposes no automatable parameters, or the
        name is unknown.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_resolve_instrument_automation_id"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out_id = ctypes.c_uint32()
        _check(
            lib.sonare_engine_resolve_instrument_automation_id(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                param_name.encode("utf-8"),
                ctypes.byref(out_id),
            )
        )
        return int(out_id.value)

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
            raise SonareValueError("SoundFont data must not be empty")
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
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(cfg),
            )
        )

    def clear_midi_instrument(self, destination_id: int = 0) -> None:
        """Clear any realtime instrument bound to ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_instrument"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_clear_midi_instrument(
                self._require_handle(), _to_c_uint32(destination_id, "destination_id")
            )
        )

    def midi_instrument_count(self) -> int:
        """Return the number of bound realtime MIDI instruments."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_midi_instrument_count"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        out = ctypes.c_size_t()
        _check(lib.sonare_engine_midi_instrument_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    # -- controller profiles -------------------------------------------------

    def set_controller_profile(self, destination_id: int, preset_name: str) -> None:
        """Replace the instrument's controller profile with a named preset.

        ``preset_name`` is one of :func:`controller_profile_names` (``"gm"``,
        ``"breath"``, ``"breath-aftertouch"``, ``"mpe"``). The preset replaces
        the binding table wholesale and drops every channel's accumulated axis
        value, since the new bindings say nothing about what the old ones had
        reached. Raises :class:`SonareError` for an unknown name, for a
        destination nothing is bound to, and for an instrument that has nowhere
        to put a profile.

        Control-thread only: do not call concurrently with :meth:`process`.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_controller_profile"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        _check(
            lib.sonare_engine_set_controller_profile(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                preset_name.encode("utf-8"),
            )
        )

    def bind_controller(
        self,
        destination_id: int,
        *,
        input: str | int,
        index: int = 0,
        axis: str | int,
        lo: float = 0.0,
        hi: float = 1.0,
        curve: float = 1.0,
    ) -> None:
        """Add one binding on top of the instrument's current controller profile.

        ``input`` is how the device spells the gesture
        (``synth_enum_tables()["controller_inputs"]``: ``"control-change"``,
        ``"channel-pressure"``, ``"poly-pressure"``, ``"pitch-bend"``,
        ``"velocity"``) and ``index`` is the CC number for
        ``"control-change"``; every other input is identified by its message
        status alone and ignores it. ``axis`` is what the gesture means
        (``synth_enum_tables()["controller_axes"]``), so the engine is reached
        by the meaning rather than by the controller number that carried it.

        ``lo`` / ``hi`` are the axis values at zero and full deflection in the
        axis's own unit -- normalized ``[0, 1]`` for the excitation axes and
        loudness, cents for ``"pitch-cents"`` and ``"vibrato-depth"`` -- and
        ``lo > hi`` inverts the gesture. ``curve`` is the exponent applied to
        the normalized input before the range maps it; keep the linear ``1.0``
        unless the device has not already shaped the gesture.

        Binding the same input twice with different axes is how one gesture
        reaches both. Raises :class:`SonareValueError` for an unknown ``input``
        or ``axis`` spelling and for a non-finite range or curve, and
        :class:`SonareError` when the table is full, when the axis is
        ``"none"``, or when a poly-pressure binding names an axis that is not
        one of the four excitation axes.

        Control-thread only: do not call concurrently with :meth:`process`.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_bind_controller"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        binding = SonareControllerBinding(
            input=_controller_input_value(input),
            index=index,
            axis=_controller_axis_value(axis),
            reserved=0,
            lo=lo,
            hi=hi,
            curve=curve,
        )
        _check(
            lib.sonare_engine_bind_controller(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(binding),
            )
        )

    def clear_controller_bindings(self, destination_id: int) -> None:
        """Drop every binding of the instrument's controller profile.

        The instrument keeps a profile; it resolves nothing until something is
        bound again.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_controller_bindings"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        _check(
            lib.sonare_engine_clear_controller_bindings(
                self._require_handle(), _to_c_uint32(destination_id, "destination_id")
            )
        )

    def controller_binding_count(self, destination_id: int) -> int:
        """Return the bindings the instrument's controller profile holds."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_controller_binding_count"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        out = ctypes.c_size_t()
        _check(
            lib.sonare_engine_controller_binding_count(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(out),
            )
        )
        return int(out.value)

    def set_controller_velocity_meaningful(self, destination_id: int, meaningful: bool) -> None:
        """Say whether note-on velocity is expression for this instrument.

        No fixed default is possible: a wind controller ships sending
        breath-derived velocity on one model and a constant on the next. When
        false the synth takes every note at full scale and the bound axes carry
        the dynamics alone.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_controller_velocity_meaningful"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        _check(
            lib.sonare_engine_set_controller_velocity_meaningful(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_int(1 if meaningful else 0, "meaningful"),
            )
        )

    def controller_velocity_meaningful(self, destination_id: int) -> bool:
        """Read back :meth:`set_controller_velocity_meaningful`."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_controller_velocity_meaningful"):
            raise RuntimeError("libsonare was built without the controller-profile ABI")
        out = ctypes.c_int()
        _check(
            lib.sonare_engine_controller_velocity_meaningful(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(out),
            )
        )
        return out.value != 0

    # -- articulation --------------------------------------------------------

    def set_articulation(self, destination_id: int, channel: int, articulation: str | int) -> None:
        """Set what one channel does with an overlapping note-on.

        ``articulation`` is one of ``synth_enum_tables()["articulations"]``:
        ``"poly"`` gives every note-on its own voice, ``"mono-retrigger"``
        stops the sounding note and starts over (what GS MONO MODE and CC126
        mean, and all they can reach), and ``"mono-legato"`` carries the
        sounding voice and only re-tunes it, so the exciter and the amplitude
        envelope never restart -- a wind player's slur, which no MIDI message
        names.

        The mode is per ``(destination_id, channel)``, so slurring one part
        leaves the rest of the rack alone. ``channel`` is 0..15 and an
        articulation outside the table is refused rather than clamped.

        ``"mono-legato"`` is a request, not a guarantee: an engine whose
        exciter is spent at the onset -- anything struck or plucked -- and a
        target pitch below what the engine's delay line holds both fall back to
        an ordinary note, which :meth:`legato_fallback_count` counts.

        Raises :class:`SonareValueError` for an unknown spelling,
        :class:`SonareError` for a destination nothing is bound to, and
        :class:`SonareError` with :attr:`ErrorCode.NOT_SUPPORTED` for an
        instrument that has no articulation of its own -- the two are different
        answers and both would otherwise read as "the call worked".

        Control-thread only: do not call concurrently with :meth:`process`.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_articulation"):
            raise RuntimeError("libsonare was built without the articulation ABI")
        _check(
            lib.sonare_engine_set_articulation(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(channel, "channel"),
                _to_c_int(_articulation_value(articulation), "articulation"),
            )
        )

    def articulation(self, destination_id: int, channel: int) -> str | int:
        """Read back :meth:`set_articulation` as its canonical name.

        Returns the raw ordinal for a value this binding has no name for, so a
        mode added to the library reaches a caller rather than raising.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_articulation"):
            raise RuntimeError("libsonare was built without the articulation ABI")
        out = ctypes.c_int()
        _check(
            lib.sonare_engine_articulation(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(channel, "channel"),
                ctypes.byref(out),
            )
        )
        return _articulation_name(int(out.value))

    def legato_fallback_count(self, destination_id: int) -> int:
        """Times a legato continuation was asked for and refused.

        Counted rather than inferred: a refusal sounds like an ordinary note,
        so nothing in the audio separates "this engine declines legato" from
        "the mode was never set". Saturates at ``2**32 - 1`` rather than
        wrapping, so a large value stays readable as "at least this many".

        Refuses on the same terms as :meth:`set_articulation` rather than
        answering zero: :class:`SonareError` for a destination nothing is bound
        to, and :class:`SonareError` with :attr:`ErrorCode.NOT_SUPPORTED` for
        an instrument that has no articulation of its own. The two are
        different answers, and an instrument that never had an articulation has
        refused nothing -- a host reading that zero would read it as "every
        slur took", which is the reading this counter exists to prevent.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_legato_fallback_count"):
            raise RuntimeError("libsonare was built without the articulation ABI")
        out = ctypes.c_uint32()
        _check(
            lib.sonare_engine_legato_fallback_count(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                ctypes.byref(out),
            )
        )
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
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(controller, "controller"),
                _to_c_uint32(param_id, "param_id"),
                _to_c_float(min_value, "min_value"),
                _to_c_float(max_value, "max_value"),
            )
        )

    def bind_midi_cc_binding(self, binding: MidiCcBinding) -> None:
        """Bind a full 7/14-bit CC, RPN, or NRPN descriptor to the live engine."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_bind_midi_cc_binding"):
            raise RuntimeError("libsonare was built without full live-MIDI binding support")
        c_binding = _cc_binding_to_c(binding)
        _check(
            lib.sonare_engine_bind_midi_cc_binding(self._require_handle(), ctypes.byref(c_binding))
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

    def set_midi_fx(self, destination_id: int, config_json: str) -> None:
        """Install/replace a live non-destructive MIDI-FX insert for ``destination_id``.

        ``config_json`` accepts the same fields as the offline MIDI-FX bake, but
        scheduled/live MIDI events are transformed at dispatch time and clip
        contents are left unmodified.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_fx"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_set_midi_fx(
                self._require_handle(),
                _to_c_uint32(destination_id, "destination_id"),
                config_json.encode("utf-8"),
            )
        )

    def clear_midi_fx(self, destination_id: int = 0) -> None:
        """Clear the live MIDI-FX insert for ``destination_id`` (default 0)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_clear_midi_fx"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_clear_midi_fx(
                self._require_handle(), _to_c_uint32(destination_id, "destination_id")
            )
        )

    def set_midi_input_source(self, destination_id: int = 0) -> None:
        """Enable the engine-owned live MIDI input source for ``destination_id``.

        Hosts can then push timestamped events with
        :meth:`push_midi_input_note_on` / ``_note_off`` / ``_cc``; the engine
        drains them at block boundaries.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_engine_set_midi_input_source"):
            raise RuntimeError("libsonare was built without live-MIDI support")
        _check(
            lib.sonare_engine_set_midi_input_source(
                self._require_handle(), _to_c_uint32(destination_id, "destination_id")
            )
        )

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
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(note, "note"),
                _to_c_uint8(velocity, "velocity"),
                _to_c_int64(port_time_samples, "port_time_samples"),
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
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(note, "note"),
                _to_c_uint8(velocity, "velocity"),
                _to_c_int64(port_time_samples, "port_time_samples"),
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
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(controller, "controller"),
                _to_c_uint8(value, "value"),
                _to_c_int64(port_time_samples, "port_time_samples"),
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
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(note, "note"),
                _to_c_uint8(velocity, "velocity"),
                _to_c_int64(render_frame, "render_frame"),
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
                _to_c_uint32(destination_id, "destination_id"),
                _to_c_uint8(group, "group"),
                _to_c_uint8(channel, "channel"),
                _to_c_uint8(note, "note"),
                _to_c_uint8(velocity, "velocity"),
                _to_c_int64(render_frame, "render_frame"),
            )
        )
