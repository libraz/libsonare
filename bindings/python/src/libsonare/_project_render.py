# ruff: noqa: F405
"""Project bounce and instrument-rendering methods."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, cast

import numpy as np

from ._engine_conversions import _POSITIVE_COUNT, _POSITIVE_INT, _given_positive
from ._errors import _entry, _malformed_output, _refuse, _refuse_type
from ._project_model import *  # noqa: F403
from ._project_model import (
    _SOURCE_BACKENDS,
    ProjectDiagnosticCode,
    ProjectDiagnosticSeverity,
    SourceBackend,
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
    _check,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _to_c_size_t,
)


def _reshape_bounce(interleaved: np.ndarray, num_channels: int) -> np.ndarray:
    """Shape an interleaved bounce as ``(frames, channels)`` at the width it was mixed at."""
    return interleaved.reshape(-1, num_channels)


def _bounce_options(
    total_frames: int | None,
    block_size: int | None,
    num_channels: int | None,
    sample_rate: int | None,
    instrument_latency_samples: int,
) -> SonareProjectBounceOptions:
    """Build the C bounce options, leaving each omitted field at the init value.

    The struct is seeded by ``sonare_project_bounce_options_init``; a given value is written
    unconverted so the struct's own narrowing sees it, and a 0 is left to the C refusal.
    """
    return SonareProjectBounceOptions._given(
        total_frames=_given_positive(total_frames, "total_frames", _POSITIVE_COUNT),
        block_size=_given_positive(block_size, "block_size", _POSITIVE_INT),
        num_channels=_given_positive(num_channels, "num_channels", _POSITIVE_INT),
        sample_rate=_given_positive(sample_rate, "sample_rate", _POSITIVE_INT),
        instrument_latency_samples=instrument_latency_samples,
    )


def _diagnostic_token(name_fn: Callable[[int], bytes | None], ordinal: int, what: str) -> str:
    """Read a diagnostic ordinal's token from the library's own name table."""
    token = name_fn(ordinal)
    if token is None:
        raise _malformed_output(f"unknown {what} ordinal: {ordinal}")
    return token.decode("ascii")


def _compile_diagnostics(
    result: SonareProjectCompileResult,
) -> tuple[ProjectDiagnostic, ...]:
    """Read the diagnostics out of a native compile result, line ``i`` of
    ``messages`` becoming ``diagnostics[i].message``."""
    lib = _get_lib()
    messages = result.messages.decode("utf-8") if result.messages else ""
    message_lines = messages.splitlines()
    return tuple(
        ProjectDiagnostic(
            code=cast(
                ProjectDiagnosticCode,
                _diagnostic_token(
                    lib.sonare_project_diagnostic_code_name,
                    int(result.diagnostics[i].code),
                    "diagnostic code",
                ),
            ),
            severity=cast(
                ProjectDiagnosticSeverity,
                _diagnostic_token(
                    lib.sonare_project_diagnostic_severity_name,
                    int(result.diagnostics[i].severity),
                    "diagnostic severity",
                ),
            ),
            target_id=int(result.diagnostics[i].target_id) or None,
            message=message_lines[i] if i < len(message_lines) else "",
        )
        for i in range(int(result.diagnostic_count))
    )


class _ProjectRenderMixin:
    if TYPE_CHECKING:

        def _require_handle(self) -> ctypes.c_void_p: ...

        def last_bounce_compile_result(self) -> ProjectCompileResult | None: ...

    @_entry("Project.bounce")
    def bounce(
        self,
        *,
        instruments: Sequence[tuple[int, ExternalInstrument]] | None = None,
        builtin_instruments: Sequence[tuple[int, BuiltinSynthConfig]] | None = None,
        synth_instruments: Sequence[tuple[int, SynthPatch | str]] | None = None,
        sf2_instruments: Sequence[tuple[int, Sf2InstrumentConfig]] | None = None,
        total_frames: int | None = None,
        block_size: int | None = None,
        num_channels: int | None = None,
        sample_rate: int | None = None,
        instrument_latency_samples: int = 0,
    ) -> np.ndarray:
        """Compile + render the project offline to interleaved float audio.

        Returns a ``(frames, channels)`` float32 ndarray at the width the master
        was mixed at. Deterministic: the same project + options + instruments
        yields a bit-identical array within one build. An omitted option takes
        the library default: the project's sample rate, the scene master's
        layout width (2 with no master), the default render block, and a render
        length derived from the arrangement (musical end + the longest
        instrument release tail). An explicit 0 or a negative value is refused.

        Each instrument keyword is a sequence of ``(destination_id, config)``
        bindings (``destination_id`` matches :meth:`set_track_midi_destination`).
        MIDI tracks routed to a destination with no binding render to silence,
        and a destination named twice, within one keyword or across keywords, is
        refused.

        Args:
            instruments: Host-supplied :class:`ExternalInstrument` objects. Their
                callbacks run synchronously on the calling thread for the
                duration of this method; an instrument's ``latency_samples``
                adds per-instrument PDC and ``tail_samples`` extends the
                auto-length render. The first exception raised inside any
                callback is re-raised. A callback instrument shared by tracks
                routed to distinct channel strips raises :class:`SonareError`
                with ``NOT_SUPPORTED``, since callback audio has no source-track
                attribution; the same holds for a latency-bearing source-aware
                instrument.
            builtin_instruments: :class:`BuiltinSynthConfig` patches for the
                built-in polyphonic oscillator synth.
            synth_instruments: NativeSynth patches: a :class:`SynthPatch` or a
                preset name string (``"sawLead"`` or ``"va:sawLead"``; see
                :func:`synth_preset_names`). An unknown preset name raises
                :class:`SonareValueError`. A patch whose ``engine_mode`` is
                ``"sample"`` reads the :class:`SampleBank` on its own
                :attr:`SynthPatch.sample_bank`; the bank is borrowed for the
                call, must outlive the bounce, and must not be added to while
                it runs. A patch's :attr:`SynthPatch.use_gm_programs` selects GM
                program/drum-kit following for its destination.
            sf2_instruments: :class:`Sf2InstrumentConfig` players fed by the
                loaded SoundFont (:meth:`load_sound_font`); programs it does not
                cover, or no SoundFont at all, play through the built-in GM
                fallback bank (see :meth:`sound_font_manifest`).
            total_frames / block_size / num_channels / sample_rate: Render
                length, block size, output width and rate; omitted takes the
                library default.
            instrument_latency_samples: Host-instrument latency compensation in
                samples fed to the compiler.
        """
        lib = _get_lib()
        # Passed unconverted so the struct's own narrowing sees each caller
        # value; int() would truncate a fraction past it.
        options = _bounce_options(
            total_frames, block_size, num_channels, sample_rate, instrument_latency_samples
        )

        errors: list[BaseException] = []
        keepalive: list[object] = []
        if instruments is not None:
            external_bindings = [(dst, inst) for dst, inst in instruments]
            c_external = (SonareInstrumentBinding * len(external_bindings))()
            for i, (dst, inst) in enumerate(external_bindings):
                if not callable(getattr(inst, "render", None)):
                    raise _refuse_type(
                        "each instrument must provide a render(channels, num_frames) method"
                    )
                c_external[i].destination_id = dst
                c_external[i].callbacks = _make_instrument_callbacks(inst, errors, keepalive)
            if external_bindings:
                options.instruments = c_external
                options.instrument_count = _to_c_size_t(len(external_bindings), "count")
        if builtin_instruments is not None:
            builtin_bindings = [(dst, config) for dst, config in builtin_instruments]
            c_builtin = (SonareBuiltinInstrumentBinding * len(builtin_bindings))()
            for i, (dst, config) in enumerate(builtin_bindings):
                c_builtin[i].destination_id = dst
                c_builtin[i].config = config._to_c()
            if builtin_bindings:
                options.builtin_instruments = c_builtin
                options.builtin_instrument_count = _to_c_size_t(len(builtin_bindings), "count")
        # Each converted patch borrows its engine params; copying it into the
        # binding array copies bytes only, so these stay alive past the C call.
        c_patches = []
        if synth_instruments is not None:
            synth_bindings = [(dst, _synth_patch_arg(patch)) for dst, patch in synth_instruments]
            c_synth = (SonareSynthInstrumentBinding * len(synth_bindings))()
            for i, (dst, synth_patch) in enumerate(synth_bindings):
                c_synth[i].destination_id = dst
                c_patch = synth_patch._to_c()
                c_patches.append(c_patch)
                c_synth[i].patch = c_patch
                c_synth[i].use_gm_programs = 1 if synth_patch.use_gm_programs else 0
                bank = synth_patch.sample_bank
                c_synth[i].sample_bank = bank._require_handle() if bank is not None else None
            if synth_bindings:
                options.synth_instruments = c_synth
                options.synth_instrument_count = _to_c_size_t(len(synth_bindings), "count")
        if sf2_instruments is not None:
            sf2_bindings = [(dst, sf2_config) for dst, sf2_config in sf2_instruments]
            c_sf2 = (SonareSf2InstrumentBinding * len(sf2_bindings))()
            for i, (dst, sf2_config) in enumerate(sf2_bindings):
                c_sf2[i].destination_id = dst
                c_sf2[i].config = sf2_config._to_c()
            if sf2_bindings:
                options.sf2_instruments = c_sf2
                options.sf2_instrument_count = _to_c_size_t(len(sf2_bindings), "count")

        width = ctypes.c_int()
        with _out_float_array(lib) as (out, out_frames):
            rc = lib.sonare_project_bounce(
                self._require_handle(),
                ctypes.byref(options),
                ctypes.byref(out),
                ctypes.byref(out_frames),
                ctypes.byref(width),
            )
            # `keepalive` pins the ctypes trampolines until the bounce returns.
            keepalive.clear()
            _check(rc, flattened="options")
            if errors:
                raise errors[0]
            interleaved = _from_c_float_array(out, int(out_frames.value) * width.value)
        return _reshape_bounce(interleaved, width.value)

    @_entry("Project.load_sound_font")
    def load_sound_font(self, data: bytes | bytearray | memoryview) -> None:
        """Load (parse) SoundFont 2 bytes into the project.

        The presets / instruments / sample headers and the sample PCM (decoded
        to a float pool) replace any previously loaded SoundFont; the input
        buffer is not referenced after the call. Raises :class:`SonareError`
        (``INVALID_FORMAT``) on malformed input (the previous SoundFont is kept).
        """
        lib = _get_lib()
        buf = bytes(data)
        if not buf:
            raise _refuse("SoundFont data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        _check(
            lib.sonare_project_load_soundfont(
                self._require_handle(), c_data, ctypes.c_size_t(len(buf))
            )
        )

    @_entry("Project.clear_sound_font")
    def clear_sound_font(self) -> None:
        """Release the project's loaded SoundFont (no-op when none is loaded)."""
        lib = _get_lib()
        _check(lib.sonare_project_clear_soundfont(self._require_handle()))

    @property
    def sound_font_preset_count(self) -> int:
        """Number of presets in the loaded SoundFont (0 when none is loaded)."""
        lib = _get_lib()
        out = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_preset_count(self._require_handle(), ctypes.byref(out)))
        return int(out.value)

    @_entry("Project.sound_font_manifest")
    def sound_font_manifest(self) -> list[Sf2ProgramStatus]:
        """Enumerate the programs the arrangement plays and their backends.

        Returns one :class:`Sf2ProgramStatus` per (channel, bank, program)
        combination a note actually plays through, in first-use order. Each
        entry reports whether it resolves in the loaded SoundFont
        (``"sf2"``, GS variation/drum fallbacks included) or would fall back to
        the built-in synth (``"synth"``).
        Without a loaded SoundFont every entry is a synth fallback.
        """
        lib = _get_lib()
        handle = self._require_handle()
        total = ctypes.c_size_t()
        _check(lib.sonare_project_soundfont_manifest(handle, None, 0, ctypes.byref(total)))
        count = int(total.value)
        if count == 0:
            return []
        entries = (SonareSf2ProgramStatus * count)()
        _check(
            lib.sonare_project_soundfont_manifest(
                handle, entries, _to_c_size_t(count, "count"), ctypes.byref(total)
            )
        )
        return [
            Sf2ProgramStatus(
                channel=int(e.channel),
                bank=int(e.bank),
                program=int(e.program),
                backend=cast(SourceBackend, _SOURCE_BACKENDS[int(e.backend)]),
                preset_name=e.preset_name.decode("utf-8", errors="replace"),
            )
            for e in entries[: min(count, int(total.value))]
        ]
