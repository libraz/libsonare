# ruff: noqa: F405
"""Project bounce and instrument-rendering methods."""

from __future__ import annotations

import ctypes
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, SupportsIndex, TypeVar, cast

import numpy as np

from ._engine_conversions import _POSITIVE_COUNT, _POSITIVE_INT, _given_positive
from ._errors import _entry, _malformed_output, _refuse, _refuse_type
from ._narrowing import _is_number
from ._project_model import *  # noqa: F403
from ._project_model import (
    _SOURCE_BACKENDS,
    ProjectArticulationBinding,
    ProjectControllerProfileBinding,
    ProjectDiagnosticCode,
    ProjectDiagnosticSeverity,
    ProjectInstrumentBinding,
    ProjectSf2InstrumentBinding,
    SourceBackend,
    _make_instrument_callbacks,
)
from ._project_synth import (
    SYNTH_ENUM_TABLES as SYNTH_ENUM_TABLES,
)
from ._project_synth import _articulation_value
from ._project_synth import (
    synth_enum_tables as synth_enum_tables,
)
from ._runtime import (
    SonareBuiltinInstrumentBinding,
    SonareEngineMetronomeConfig,
    SonareInstrumentBinding,
    SonareProjectArticulationBinding,
    SonareProjectBounceOptions,
    SonareProjectCompileResult,
    SonareProjectControllerProfileBinding,
    SonareSf2InstrumentBinding,
    SonareSf2ProgramStatus,
    SonareSynthInstrumentBinding,
    _bool_flag,
    _check,
    _from_c_float_array,
    _get_lib,
    _narrow_double,
    _narrow_int,
    _out_float_array,
    _Respelling,
    _to_c_size_t,
    _to_c_uint32,
    _utf8_arg,
)
from ._types_engine import EngineMetronomeConfig

_Row = TypeVar("_Row")


def _binding_destination(binding: ProjectInstrumentBinding[object]) -> int:
    """The destination a binding renders; an omitted id is destination 0."""
    return 0 if binding.destination_id is None else binding.destination_id


def _planar_bounce(interleaved: np.ndarray, num_channels: int) -> np.ndarray:
    """Return an interleaved bounce as contiguous ``(channels, frames)`` planes."""
    return np.ascontiguousarray(interleaved.reshape(-1, num_channels).T)


def _sound_font_uri(uri: str | None, subject: str = "uri") -> bytes | None:
    """Encode an optional SoundFont URI, preserving None as the default selector."""
    if uri is None:
        return None
    try:
        encoded = _utf8_arg(uri, subject)
    except UnicodeEncodeError:
        raise _refuse(f"{subject} must contain well-formed Unicode") from None
    if not encoded:
        raise _refuse(f"{subject} must not be empty")
    return encoded


def _bounce_metronome(config: EngineMetronomeConfig) -> SonareEngineMetronomeConfig:
    """Convert a project bounce click config with project keyword names."""
    if config.click_samples is not None and config.click_sec is not None:
        raise _refuse("metronome.click_samples and metronome.click_sec are mutually exclusive")
    return SonareEngineMetronomeConfig._given(
        enabled=_bool_flag(config.enabled, "metronome.enabled"),
        beat_gain=_narrow_double(config.beat_gain, "metronome.beat_gain"),
        accent_gain=_narrow_double(config.accent_gain, "metronome.accent_gain"),
        click_samples=None
        if config.click_samples is None
        else _narrow_int(config.click_samples, "metronome.click_samples", 1, 384000),
        click_sec=None
        if config.click_sec is None
        else _narrow_double(config.click_sec, "metronome.click_sec"),
    )


def _bounce_bindings(
    controller_profiles: Sequence[ProjectControllerProfileBinding],
    articulations: Sequence[ProjectArticulationBinding],
) -> tuple[
    ctypes.Array | None,
    list[bytes],
    ctypes.Array | None,
]:
    """Convert typed instrument-control bindings and keep their strings alive."""
    profile_names: list[bytes] = []
    c_profiles = (SonareProjectControllerProfileBinding * len(controller_profiles))()
    for index, row in enumerate(controller_profiles):
        path = f"controller_profiles[{index}]"
        c_profiles[index].destination_id = _to_c_uint32(
            row.destination_id, f"{path}.destination_id"
        )
        preset_path = f"{path}.preset_name"
        try:
            encoded_name = _utf8_arg(row.preset_name, preset_path)
        except UnicodeEncodeError:
            raise _refuse(f"{preset_path} must contain well-formed Unicode") from None
        if not encoded_name:
            raise _refuse(f"{preset_path} must not be empty")
        profile_names.append(encoded_name)
        c_profiles[index].preset_name = encoded_name

    c_articulations = (SonareProjectArticulationBinding * len(articulations))()
    for index, row in enumerate(articulations):
        path = f"articulations[{index}]"
        c_articulations[index].destination_id = _to_c_uint32(
            row.destination_id, f"{path}.destination_id"
        )
        c_articulations[index].channel = _narrow_int(row.channel, f"{path}.channel", 0, 15)
        c_articulations[index].articulation = _articulation_value(
            row.articulation, f"{path}.articulation"
        )
    return (
        c_profiles if controller_profiles else None,
        profile_names,
        c_articulations if articulations else None,
    )


def _sequence_rows(value: Sequence[_Row] | None, name: str) -> list[_Row]:
    """Copy a facade sequence while refusing scalar strings and non-sequences by name."""
    if value is None:
        return []
    if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
        raise _refuse_type(f"{name} must be a sequence")
    return list(value)


def _require_integer_shape(value: object, name: str) -> None:
    if isinstance(value, bool) or not (isinstance(value, SupportsIndex) or _is_number(value)):
        raise _refuse_type(f"{name} must be an integer")


def _preflight_bounce_shapes(
    *,
    instruments: Sequence[ProjectInstrumentBinding[ExternalInstrument]] | None,
    builtin_instruments: Sequence[ProjectInstrumentBinding[BuiltinSynthConfig]] | None,
    synth_instruments: Sequence[ProjectInstrumentBinding[SynthPatch]] | None,
    sf2_instruments: Sequence[ProjectSf2InstrumentBinding] | None,
    controller_profiles: Sequence[ProjectControllerProfileBinding] | None,
    articulations: Sequence[ProjectArticulationBinding] | None,
    metronome: EngineMetronomeConfig | None,
) -> tuple[
    list[ProjectInstrumentBinding[ExternalInstrument]],
    list[ProjectInstrumentBinding[BuiltinSynthConfig]],
    list[ProjectInstrumentBinding[SynthPatch]],
    list[ProjectSf2InstrumentBinding],
    list[ProjectControllerProfileBinding],
    list[ProjectArticulationBinding],
]:
    """Refuse malformed binding shapes before validating scalar domains."""
    if metronome is not None and not isinstance(metronome, EngineMetronomeConfig):
        raise _refuse_type("metronome must be an EngineMetronomeConfig")

    instrument_rows = _sequence_rows(instruments, "instruments")
    builtin_rows = _sequence_rows(builtin_instruments, "builtin_instruments")
    synth_rows = _sequence_rows(synth_instruments, "synth_instruments")
    for name, bindings, expected in (
        ("instruments", instrument_rows, ExternalInstrument),
        ("builtin_instruments", builtin_rows, BuiltinSynthConfig),
        ("synth_instruments", synth_rows, SynthPatch),
    ):
        for index, binding in enumerate(bindings):
            if not isinstance(binding, ProjectInstrumentBinding):
                raise _refuse_type(f"{name}[{index}] must be a ProjectInstrumentBinding")
            if binding.destination_id is not None:
                _require_integer_shape(binding.destination_id, f"{name}[{index}].destination_id")
            instrument = binding.instrument
            if expected is ExternalInstrument:
                if not callable(getattr(instrument, "render", None)):
                    raise _refuse_type(
                        f"{name}[{index}].instrument must provide a "
                        "render(channels, num_frames) method"
                    )
            elif not isinstance(instrument, expected):
                raise _refuse_type(f"{name}[{index}].instrument must be a {expected.__name__}")

    sf2_bindings = _sequence_rows(sf2_instruments, "sf2_instruments")
    for index, binding in enumerate(sf2_bindings):
        if not isinstance(binding, ProjectSf2InstrumentBinding):
            raise _refuse_type(f"sf2_instruments[{index}] must be a ProjectSf2InstrumentBinding")
        if binding.destination_id is not None:
            _require_integer_shape(
                binding.destination_id, f"sf2_instruments[{index}].destination_id"
            )
        if not isinstance(binding.instrument, Sf2InstrumentConfig):
            raise _refuse_type(f"sf2_instruments[{index}].instrument must be a Sf2InstrumentConfig")
        if binding.sound_font_uri is not None and not isinstance(binding.sound_font_uri, str):
            raise _refuse_type(f"sf2_instruments[{index}].sound_font_uri must be a str")

    control_rows = _sequence_rows(controller_profiles, "controller_profiles")
    for index, row in enumerate(control_rows):
        path = f"controller_profiles[{index}]"
        if not isinstance(row, ProjectControllerProfileBinding):
            raise _refuse_type(f"{path} must be a ProjectControllerProfileBinding")
        _require_integer_shape(row.destination_id, f"{path}.destination_id")
        if not isinstance(row.preset_name, str):
            raise _refuse_type(f"{path}.preset_name must be a str")

    articulation_rows = _sequence_rows(articulations, "articulations")
    for index, row in enumerate(articulation_rows):
        path = f"articulations[{index}]"
        if not isinstance(row, ProjectArticulationBinding):
            raise _refuse_type(f"{path} must be a ProjectArticulationBinding")
        _require_integer_shape(row.destination_id, f"{path}.destination_id")
        _require_integer_shape(row.channel, f"{path}.channel")
        if not isinstance(row.articulation, str):
            raise _refuse_type(f"{path}.articulation must be a str")

    return (
        instrument_rows,
        builtin_rows,
        synth_rows,
        sf2_bindings,
        control_rows,
        articulation_rows,
    )


def _bounce_options(
    total_frames: int | None,
    block_size: int | None,
    num_channels: int | None,
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

    @_entry("Project.count_in_frames")
    def count_in_frames(self, count_in_bars: int) -> int:
        """Return the pre-roll length at the project's initial tempo and meter."""
        bars = _narrow_int(count_in_bars, "count_in_bars", 0, 16)
        out_frames = ctypes.c_int64()
        _check(
            _get_lib().sonare_project_count_in_frames(
                self._require_handle(),
                bars,
                ctypes.byref(out_frames),
            )
        )
        if out_frames.value < 0:
            raise _malformed_output("frames must be non-negative")
        return int(out_frames.value)

    @_entry("Project.bounce")
    def bounce(
        self,
        *,
        instruments: Sequence[ProjectInstrumentBinding[ExternalInstrument]] | None = None,
        builtin_instruments: Sequence[ProjectInstrumentBinding[BuiltinSynthConfig]] | None = None,
        synth_instruments: Sequence[ProjectInstrumentBinding[SynthPatch]] | None = None,
        sf2_instruments: Sequence[ProjectSf2InstrumentBinding] | None = None,
        total_frames: int | None = None,
        block_size: int | None = None,
        num_channels: int | None = None,
        instrument_latency_samples: int | None = 0,
        metronome: EngineMetronomeConfig | None = None,
        count_in_bars: int = 0,
        controller_profiles: Sequence[ProjectControllerProfileBinding] | None = None,
        articulations: Sequence[ProjectArticulationBinding] | None = None,
    ) -> ProjectBounceResult:
        """Compile and render the project offline to planar float audio.

        Returns a frozen :class:`ProjectBounceResult` whose ``channels`` array
        has shape ``(num_channels, frames)``. Deterministic: the same project +
        options + instruments yields bit-identical channel arrays within one
        build. An omitted option takes
        the library default: the project's sample rate, the scene master's
        layout width (2 with no master), the default render block, and a render
        length derived from the arrangement (musical end + the longest
        instrument release tail). An explicit 0 or a negative value is refused.

        Instrument keywords take sequences of binding records. Their
        ``destination_id`` defaults to 0 and matches
        :meth:`set_track_midi_destination`. MIDI tracks routed to a destination
        with no binding render to silence, and a destination named twice,
        within one keyword or across keywords, is refused.

        Args:
            instruments: :class:`ProjectInstrumentBinding` records containing
                host-supplied :class:`ExternalInstrument` objects. Their
                callbacks run synchronously on the calling thread for the
                duration of this method; an instrument's ``latency_samples``
                adds per-instrument PDC and ``tail_samples`` extends the
                auto-length render. The first exception raised inside any
                callback is re-raised. A callback instrument shared by tracks
                routed to distinct channel strips raises :class:`SonareError`
                with ``NOT_SUPPORTED``, since callback audio has no source-track
                attribution; the same holds for a latency-bearing source-aware
                instrument.
            builtin_instruments: :class:`ProjectInstrumentBinding` records
                containing :class:`BuiltinSynthConfig` patches for the built-in
                polyphonic oscillator synth.
            synth_instruments: :class:`ProjectInstrumentBinding` records
                containing NativeSynth :class:`SynthPatch` instances.
                Select a preset with ``SynthPatch(preset=...)``. A patch whose
                ``engine_mode`` is
                ``"sample"`` reads the :class:`SampleBank` on its own
                :attr:`SynthPatch.sample_bank`; the bank is borrowed for the
                call, must outlive the bounce, and must not be added to while
                it runs. A patch's :attr:`SynthPatch.use_gm_programs` selects GM
                program/drum-kit following for its destination.
            sf2_instruments: :class:`ProjectSf2InstrumentBinding` records
                containing :class:`Sf2InstrumentConfig` patches. Set
                ``sound_font_uri`` to select a named font; ``None`` selects the
                default. An empty URI is refused. Programs it does not cover, or
                no SoundFont at all, play through the built-in GM fallback bank
                (see :meth:`sound_font_manifest`).
            total_frames / block_size / num_channels: Render length, block
                size and output width; omitted takes the library default.
            instrument_latency_samples: Host-instrument latency compensation in
                samples fed to the compiler. ``None`` selects the default 0.
            metronome: Optional :class:`EngineMetronomeConfig`; clicks are mixed
                when ``enabled`` is true. A disabled config renders like ``None``.
            count_in_bars: Silent or clicked pre-roll bars in ``[0, 16]``.
            controller_profiles: :class:`ProjectControllerProfileBinding` records for
                instruments that support controller profiles.
            articulations: :class:`ProjectArticulationBinding` records for each
                destination and channel.
        """
        (
            external_bindings,
            builtin_bindings,
            synth_bindings,
            sf2_bindings,
            profile_rows,
            articulation_rows,
        ) = _preflight_bounce_shapes(
            instruments=instruments,
            builtin_instruments=builtin_instruments,
            synth_instruments=synth_instruments,
            sf2_instruments=sf2_instruments,
            controller_profiles=controller_profiles,
            articulations=articulations,
            metronome=metronome,
        )
        for name, value in (
            ("total_frames", total_frames),
            ("block_size", block_size),
            ("num_channels", num_channels),
            ("instrument_latency_samples", instrument_latency_samples),
        ):
            if value is not None:
                _require_integer_shape(value, name)
        count_in_bars_value = _narrow_int(count_in_bars, "count_in_bars", 0, 16)
        lib = _get_lib()
        # Passed unconverted so the struct's own narrowing sees each caller
        # value; int() would truncate a fraction past it.
        options = _bounce_options(
            total_frames,
            block_size,
            num_channels,
            0 if instrument_latency_samples is None else instrument_latency_samples,
        )
        options.count_in_bars = count_in_bars_value
        c_metronome = _bounce_metronome(metronome) if metronome is not None else None

        c_profiles, profile_names, c_articulations = _bounce_bindings(
            profile_rows, articulation_rows
        )

        errors: list[BaseException] = []
        keepalive: list[object] = []
        if external_bindings:
            c_external = (SonareInstrumentBinding * len(external_bindings))()
            for i, binding in enumerate(external_bindings):
                inst = binding.instrument
                c_external[i].destination_id = _to_c_uint32(
                    _binding_destination(binding), f"instruments[{i}].destination_id"
                )
                c_external[i].callbacks = _make_instrument_callbacks(inst, errors, keepalive)
            options.instruments = c_external
            options.instrument_count = _to_c_size_t(len(external_bindings), "count")
        if builtin_bindings:
            c_builtin = (SonareBuiltinInstrumentBinding * len(builtin_bindings))()
            for i, binding in enumerate(builtin_bindings):
                config = binding.instrument
                c_builtin[i].destination_id = _to_c_uint32(
                    _binding_destination(binding), f"builtin_instruments[{i}].destination_id"
                )
                c_builtin[i].config = config._to_c()
            options.builtin_instruments = c_builtin
            options.builtin_instrument_count = _to_c_size_t(len(builtin_bindings), "count")
        # Each converted patch borrows its engine params; copying it into the
        # binding array copies bytes only, so these stay alive past the C call.
        c_patches = []
        if synth_bindings:
            c_synth = (SonareSynthInstrumentBinding * len(synth_bindings))()
            for i, binding in enumerate(synth_bindings):
                synth_patch = binding.instrument
                c_synth[i].destination_id = _to_c_uint32(
                    _binding_destination(binding), f"synth_instruments[{i}].destination_id"
                )
                c_patch = synth_patch._to_c()
                c_patches.append(c_patch)
                c_synth[i].patch = c_patch
                c_synth[i].use_gm_programs = 1 if synth_patch.use_gm_programs else 0
                bank = synth_patch.sample_bank
                c_synth[i].sample_bank = bank._require_handle() if bank is not None else None
            options.synth_instruments = c_synth
            options.synth_instrument_count = _to_c_size_t(len(synth_bindings), "count")
        c_sf2 = None
        sf2_uri_storage: list[bytes] = []
        if sf2_bindings:
            c_sf2 = (SonareSf2InstrumentBinding * len(sf2_bindings))()
            for i, binding in enumerate(sf2_bindings):
                sf2_config = binding.instrument
                c_sf2[i].destination_id = _to_c_uint32(
                    _binding_destination(binding), f"sf2_instruments[{i}].destination_id"
                )
                c_sf2[i].config = sf2_config._to_c()
                encoded_uri = _sound_font_uri(
                    binding.sound_font_uri, f"sf2_instruments[{i}].sound_font_uri"
                )
                if encoded_uri is not None:
                    c_sf2[i].sound_font_uri = encoded_uri
                    sf2_uri_storage.append(encoded_uri)
            options.sf2_instruments = c_sf2
            options.sf2_instrument_count = _to_c_size_t(len(sf2_bindings), "count")

        if c_metronome is not None:
            options.metronome = ctypes.pointer(c_metronome)
        if c_profiles is not None:
            options.controller_profiles = c_profiles
            options.controller_profile_count = _to_c_size_t(len(c_profiles), "controller_profiles")
        if c_articulations is not None:
            options.articulations = c_articulations
            options.articulation_count = _to_c_size_t(len(c_articulations), "articulations")
        # A Python instrument callback may reenter and edit the project, so read
        # the effective rate before entering the native renderer.
        resolved_sample_rate = int(self.sample_rate)

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
            respelled = {
                "metronome.enabled": _Respelling("metronome.enabled"),
                "metronome.beat_gain": _Respelling("metronome.beat_gain"),
                "metronome.accent_gain": _Respelling("metronome.accent_gain"),
                "metronome.click_samples": _Respelling("metronome.click_samples"),
                "metronome.click_sec": _Respelling("metronome.click_sec"),
            }
            _check(rc, flattened="options", respelled=respelled)
            if errors:
                raise errors[0]
            if width.value <= 0:
                raise _malformed_output("project bounce returned no channels")
            interleaved = _from_c_float_array(out, int(out_frames.value) * width.value)
        channels = _planar_bounce(interleaved, width.value)
        # Keep strings borrowed by the native bounce alive through the C call.
        del profile_names
        del sf2_uri_storage
        return ProjectBounceResult(channels=channels, sample_rate=resolved_sample_rate)

    @_entry("Project.load_sound_font")
    def load_sound_font(
        self, data: bytes | bytearray | memoryview, *, uri: str | None = None
    ) -> None:
        """Load (parse) SoundFont 2 bytes into the project.

        The presets / instruments / sample headers and the sample PCM (decoded
        to a float pool) replace the SoundFont at ``uri``. Omit ``uri`` to
        select the default SoundFont. The input buffer is not referenced after
        the call. Raises :class:`SonareError`
        (``INVALID_FORMAT``) on malformed input (the previous SoundFont is kept).
        """
        if not isinstance(data, (bytes, bytearray, memoryview)):
            raise _refuse_type("data must be bytes-like")
        buf = bytes(data)
        if not buf:
            raise _refuse("data must not be empty")
        c_data = (ctypes.c_uint8 * len(buf)).from_buffer_copy(buf)
        encoded_uri = _sound_font_uri(uri)
        lib = _get_lib()
        _check(
            lib.sonare_project_load_sound_font(
                self._require_handle(), encoded_uri, c_data, ctypes.c_size_t(len(buf))
            )
        )

    @_entry("Project.clear_sound_font")
    def clear_sound_font(self, *, uri: str | None = None) -> None:
        """Release the SoundFont at ``uri``, or the default one when omitted."""
        encoded_uri = _sound_font_uri(uri)
        lib = _get_lib()
        _check(lib.sonare_project_clear_sound_font(self._require_handle(), encoded_uri))

    @_entry("Project.sound_font_preset_count")
    def sound_font_preset_count(self, *, uri: str | None = None) -> int:
        """Return the preset count at ``uri``, or in the default SoundFont."""
        encoded_uri = _sound_font_uri(uri)
        lib = _get_lib()
        out = ctypes.c_size_t()
        _check(
            lib.sonare_project_sound_font_preset_count(
                self._require_handle(), encoded_uri, ctypes.byref(out)
            )
        )
        return int(out.value)

    @_entry("Project.sound_font_manifest")
    def sound_font_manifest(self, *, uri: str | None = None) -> list[Sf2ProgramStatus]:
        """Enumerate the programs the arrangement plays and their backends.

        Returns one :class:`Sf2ProgramStatus` per (channel, bank, program)
        combination a note actually plays through, in first-use order. Each
        entry reports whether it resolves in the loaded SoundFont
        (``"sf2"``, GS variation/drum fallbacks included) or would fall back to
        the built-in synth (``"synth"``).
        Without a loaded SoundFont every entry is a synth fallback.
        """
        encoded_uri = _sound_font_uri(uri)
        lib = _get_lib()
        handle = self._require_handle()
        total = ctypes.c_size_t()
        _check(
            lib.sonare_project_sound_font_manifest(
                handle, encoded_uri, None, 0, ctypes.byref(total)
            )
        )
        count = int(total.value)
        if count == 0:
            return []
        entries = (SonareSf2ProgramStatus * count)()
        _check(
            lib.sonare_project_sound_font_manifest(
                handle, encoded_uri, entries, _to_c_size_t(count, "count"), ctypes.byref(total)
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
