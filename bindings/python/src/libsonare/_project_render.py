# ruff: noqa: F405
"""Project bounce and instrument-rendering methods."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence
from typing import TYPE_CHECKING

import numpy as np

from ._project_model import *  # noqa: F403
from ._project_model import (
    _make_instrument_callbacks,
    _synth_patch_arg,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareBuiltinInstrumentBinding,
    SonareInstrumentBinding,
    SonareProjectBounceOptions,
    SonareProjectCompileResult,
    SonareSf2InstrumentBinding,
    SonareSf2ProgramStatus,
    SonareSynthInstrumentBinding,
    SonareValueError,
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
)


def _reshape_bounce(interleaved: np.ndarray, num_channels: int) -> np.ndarray:
    channels = num_channels if num_channels > 0 else 2
    if channels > 0 and interleaved.size % channels == 0:
        return interleaved.reshape(-1, channels)
    return interleaved.reshape(-1, 1)


class _ProjectRenderMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

        def last_bounce_compile_result(self) -> ProjectCompileResult: ...

    def compile(self) -> ProjectCompileResult:
        """Compile the project into an RT-readable timeline.

        Returns a :class:`ProjectCompileResult` with ``has_timeline``,
        ``messages``, and structured ``diagnostics``. The result remains
        iterable as ``(has_timeline, messages)`` for legacy callers. Never
        throws on bad project content; it surfaces error diagnostics.
        """
        lib = _get_lib()
        result = SonareProjectCompileResult()
        _check(lib.sonare_project_compile(self._require_handle(), ctypes.byref(result)))
        try:
            has_timeline = bool(result.has_timeline)
            messages = result.messages.decode("utf-8") if result.messages else ""
            message_lines = messages.splitlines()
            diagnostics = tuple(
                ProjectDiagnostic(
                    code=int(result.diagnostics[i].code),
                    severity=int(result.diagnostics[i].severity),
                    target_id=int(result.diagnostics[i].target_id),
                    message=message_lines[i] if i < len(message_lines) else "",
                )
                for i in range(int(result.diagnostic_count))
            )
            return ProjectCompileResult(has_timeline, messages, diagnostics)
        finally:
            lib.sonare_project_free_compile_result(ctypes.byref(result))

    def bounce(
        self,
        *,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project offline to interleaved float audio.

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic: the
        same project + options yields a bit-identical array within one build.
        Zero-valued options take native defaults (project sample rate, 2
        channels, block 128). Omitting ``total_frames`` (or passing <= 0) makes
        the native layer auto-derive the render length from the arrangement.

        Note: MIDI tracks render to silence here because no instrument is bound;
        use :meth:`bounce_with_builtin_instrument` to render MIDI through the
        built-in synth.
        """
        lib = _get_lib()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        with _out_float_array(lib) as (out, out_len):
            _check(
                lib.sonare_project_bounce(
                    self._require_handle(),
                    ctypes.byref(options),
                    ctypes.byref(out),
                    ctypes.byref(out_len),
                )
            )
            interleaved = _from_c_float_array(out, int(out_len.value))
        return _reshape_bounce(interleaved, num_channels)

    def bounce_with_builtin_instrument(
        self,
        instrument: BuiltinSynthConfig | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, BuiltinSynthConfig]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the built-in synth.

        Unlike :meth:`bounce`, MIDI tracks routed to a bound destination render
        through the built-in polyphonic oscillator synth, so a MIDI-only
        arrangement produces audible (non-silent) output without the caller
        supplying its own instrument callbacks.

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass
                ``None`` for the default sine patch. Ignored when ``instruments``
                is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames: Render length in frames; <= 0 auto-derives the length
                from the arrangement (musical end + the synth's release tail).
            block_size / num_channels / sample_rate / instrument_latency_samples:
                As :meth:`bounce` (0 takes native defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + patch.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_builtin_instruments"):
            raise RuntimeError("libsonare was built without the built-in instrument bounce ABI")
        if instruments is None:
            patch = instrument if instrument is not None else BuiltinSynthConfig()
            bindings = [(int(destination_id), patch)]
        else:
            bindings = [(int(dst), patch) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareBuiltinInstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].config = patch._to_c()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        with _out_float_array(lib) as (out, out_len):
            _check(
                lib.sonare_project_bounce_with_builtin_instruments(
                    self._require_handle(),
                    ctypes.byref(options),
                    c_bindings if count else None,
                    ctypes.c_size_t(count),
                    ctypes.byref(out),
                    ctypes.byref(out_len),
                )
            )
            interleaved = _from_c_float_array(out, int(out_len.value))
        return _reshape_bounce(interleaved, num_channels)

    def bounce_with_synth_instrument(
        self,
        instrument: SynthPatch | str | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, SynthPatch | str]] | None = None,
        auto_select_gm: bool = False,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the NativeSynth.

        Like :meth:`bounce_with_builtin_instrument`, but each bound destination
        renders through the patch-driven NativeSynth — the full synthesizer
        (subtractive / FM / Karplus-Strong / modal / additive / percussion /
        extended-waveguide-piano engines plus the realism layer).

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass a
                :class:`SynthPatch`, a preset name string (``"saw-lead"`` or
                ``"va:saw-lead"``; see :func:`synth_preset_names`), or ``None``
                for the default subtractive patch. Ignored when ``instruments``
                is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0). Unlike the
                JS helpers, Python keeps this as a binding argument instead of a
                :class:`SynthPatch` field.
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            auto_select_gm: Resolve each MIDI channel from its GM bank/program
                messages and use the GM drum-kit map on channel 10. This is
                useful for general MIDI files; an explicit preset remains the
                fixed patch fallback.
            total_frames: Render length in frames; <= 0 auto-derives the length
                from the arrangement (musical end + the patch's release tail).
            block_size / num_channels / sample_rate / instrument_latency_samples:
                As :meth:`bounce` (0 takes native defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + patch. Raises :class:`SonareError` for an
        unknown preset name.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_synth_instruments"):
            raise RuntimeError("libsonare was built without the NativeSynth bounce ABI")
        if instruments is None:
            bindings = [(int(destination_id), _synth_patch_arg(instrument))]
        else:
            bindings = [(int(dst), _synth_patch_arg(patch)) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareSynthInstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].patch = patch._to_c()
            c_bindings[i].use_gm_programs = bool(auto_select_gm)
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        with _out_float_array(lib) as (out, out_len):
            _check(
                lib.sonare_project_bounce_with_synth_instruments(
                    self._require_handle(),
                    ctypes.byref(options),
                    c_bindings if count else None,
                    ctypes.c_size_t(count),
                    ctypes.byref(out),
                    ctypes.byref(out_len),
                )
            )
            interleaved = _from_c_float_array(out, int(out_len.value))
        return _reshape_bounce(interleaved, num_channels)

    def load_soundfont(self, data: bytes | bytearray | memoryview) -> None:
        """Load (parse) SoundFont 2 bytes into the project.

        The presets / instruments / sample headers and the sample PCM (decoded
        to a float pool) replace any previously loaded SoundFont; the input
        buffer is not referenced after the call. Raises :class:`SonareError`
        on malformed input (the previous SoundFont is kept).
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_load_soundfont"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        buf = bytes(data)
        if not buf:
            raise SonareValueError("SoundFont data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_project_load_soundfont(
                self._require_handle(), c_data, ctypes.c_size_t(len(buf))
            )
        )

    def clear_soundfont(self) -> None:
        """Release the project's loaded SoundFont (no-op when none is loaded)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_clear_soundfont"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        _check(lib.sonare_project_clear_soundfont(self._require_handle()))

    def soundfont_preset_count(self) -> int:
        """Number of presets in the loaded SoundFont (0 when none is loaded)."""
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_soundfont_preset_count"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        out = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_preset_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    def soundfont_manifest(self) -> list[Sf2ProgramStatus]:
        """Enumerate the programs the arrangement plays and their backends.

        Returns one :class:`Sf2ProgramStatus` per (channel, bank, program)
        combination a note actually plays through, in first-use order. Each
        entry reports whether it resolves in the loaded SoundFont
        (:data:`SOURCE_BACKEND_SF2`, GS variation/drum fallbacks included) or
        would fall back to the built-in synth (:data:`SOURCE_BACKEND_SYNTH`).
        Without a loaded SoundFont every entry is a synth fallback.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_soundfont_manifest"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        handle = self._require_handle()
        total = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_manifest(handle, None, 0, ctypes.byref(total)))
        count = int(total.value)
        if count == 0:
            return []
        entries = (SonareSf2ProgramStatus * count)()
        _check(
            lib.sonare_project_soundfont_manifest(
                handle, entries, ctypes.c_size_t(count), ctypes.byref(total)
            )
        )
        return [
            Sf2ProgramStatus(
                channel=int(e.channel),
                bank=int(e.bank),
                program=int(e.program),
                backend=int(e.backend),
                preset_name=e.preset_name.decode("utf-8", errors="replace"),
            )
            for e in entries[: min(count, int(total.value))]
        ]

    def bounce_with_sf2_instrument(
        self,
        instrument: Sf2InstrumentConfig | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, Sf2InstrumentConfig]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through the SF2 player.

        Like :meth:`bounce_with_builtin_instrument`, but each bound destination
        renders through a GS-compatible SoundFont player fed by the project's
        loaded SoundFont (:meth:`load_soundfont`): 16 MIDI channels per player,
        channel 10 drums via bank 128, GS NRPN part edits and GS/GM SysEx
        resets honored. Programs the SoundFont does not cover — including
        bouncing with no SoundFont loaded at all — play through the built-in
        synthesizer GM fallback bank (the data-free floor; see
        :meth:`soundfont_manifest` for the per-program backend).

        Args:
            instrument: Patch bound to ``destination_id`` (default 0). Pass
                ``None`` for the default patch. Ignored when ``instruments`` is
                given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, patch)``
                bindings, overriding ``instrument`` / ``destination_id``.
            total_frames / block_size / num_channels / sample_rate /
                instrument_latency_samples: As :meth:`bounce` (0 takes native
                defaults).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + SoundFont + patch.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_sf2_instruments"):
            raise RuntimeError("libsonare was built without the SoundFont ABI")
        if instruments is None:
            patch = instrument if instrument is not None else Sf2InstrumentConfig()
            bindings = [(int(destination_id), patch)]
        else:
            bindings = [(int(dst), patch) for dst, patch in instruments]
        count = len(bindings)
        c_bindings = (SonareSf2InstrumentBinding * count)()
        for i, (dst, patch) in enumerate(bindings):
            c_bindings[i].destination_id = dst
            c_bindings[i].config = patch._to_c()
        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        with _out_float_array(lib) as (out, out_len):
            _check(
                lib.sonare_project_bounce_with_sf2_instruments(
                    self._require_handle(),
                    ctypes.byref(options),
                    c_bindings if count else None,
                    ctypes.c_size_t(count),
                    ctypes.byref(out),
                    ctypes.byref(out_len),
                )
            )
            interleaved = _from_c_float_array(out, int(out_len.value))
        return _reshape_bounce(interleaved, num_channels)

    def bounce_with_instruments(
        self,
        instrument: ExternalInstrument | None = None,
        *,
        destination_id: int = 0,
        instruments: Sequence[tuple[int, ExternalInstrument]] | None = None,
        total_frames: int = 0,
        block_size: int = 0,
        num_channels: int = 0,
        sample_rate: int = 0,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project, driving MIDI tracks through host instruments.

        Unlike :meth:`bounce_with_builtin_instrument`, each bound destination is
        rendered by a caller-supplied :class:`ExternalInstrument` (a real
        sampler/synth), letting MIDI tracks route through your own sound source.
        The instrument's callbacks run synchronously on the calling thread for
        the duration of this method, so no thread-safety machinery is required.

        Args:
            instrument: Instrument bound to ``destination_id`` (default 0).
                Ignored when ``instruments`` is given.
            destination_id: Destination id for ``instrument`` (matches
                :meth:`set_track_midi_destination`; default 0).
            instruments: Optional explicit list of ``(destination_id, instrument)``
                bindings, overriding ``instrument`` / ``destination_id``.
                    total_frames / block_size / num_channels / sample_rate /
                        instrument_latency_samples: As :meth:`bounce` (0 takes native
                        defaults; an instrument's own ``latency_samples`` attribute adds
                        per-instrument PDC; ``tail_samples`` extends auto-length
                        bounces).

        Returns a ``(frames, channels)`` float32 ndarray. Deterministic for a
        fixed project + options + instrument behaviour. Raises the first
        exception raised inside any instrument callback. A callback instrument
        shared by tracks routed to distinct channel strips raises
        :class:`SonareError` with ``NOT_SUPPORTED`` because callback audio has
        no source-track attribution; the same constraint applies to a
        latency-bearing source-aware instrument.
        """
        lib = _get_lib()
        if not hasattr(lib, "sonare_project_bounce_with_instruments"):
            raise RuntimeError("libsonare was built without the external-instrument bounce ABI")
        if instruments is None:
            if instrument is None:
                raise SonareValueError(
                    "bounce_with_instruments requires `instrument` or `instruments`"
                )
            bindings = [(int(destination_id), instrument)]
        else:
            bindings = [(int(dst), inst) for dst, inst in instruments]
        count = len(bindings)

        errors: list[BaseException] = []
        keepalive: list[object] = []
        c_bindings = (SonareInstrumentBinding * count)()
        for i, (dst, inst) in enumerate(bindings):
            if not callable(getattr(inst, "render", None)):
                raise TypeError(
                    "each instrument must provide a render(channels, num_frames) method"
                )
            c_bindings[i].destination_id = dst
            c_bindings[i].callbacks = _make_instrument_callbacks(inst, errors, keepalive)

        options = SonareProjectBounceOptions(
            total_frames=int(total_frames),
            block_size=int(block_size),
            num_channels=int(num_channels),
            sample_rate=int(sample_rate),
            instrument_latency_samples=int(instrument_latency_samples),
        )
        with _out_float_array(lib) as (out, out_len):
            rc = lib.sonare_project_bounce_with_instruments(
                self._require_handle(),
                ctypes.byref(options),
                c_bindings if count else None,
                ctypes.c_size_t(count),
                ctypes.byref(out),
                ctypes.byref(out_len),
            )
            # `keepalive` pins the ctypes trampolines until the bounce returns.
            keepalive.clear()
            _check(rc)
            if errors:
                raise errors[0]
            interleaved = _from_c_float_array(out, int(out_len.value))
        return _reshape_bounce(interleaved, num_channels)
