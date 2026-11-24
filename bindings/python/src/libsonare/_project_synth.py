"""NativeSynth patch enum-name tables, coercion helpers and the sample bank.

The tables map the :class:`SynthPatch` / :class:`Project` facade's string enum
spellings to the integer ordinals the C ABI expects, and expose the canonical
enum-name tables. :class:`SampleBank` is the one public class defined here: the
host-supplied PCM the sample engine plays. Everything reaches callers through
``_project.py`` (``SYNTH_ENUM_TABLES`` / ``synth_enum_tables`` / ``SampleBank``)
rather than from this module directly.
"""

from __future__ import annotations

import contextlib
import ctypes
import json
from collections.abc import Mapping, Sequence
from typing import Literal, cast, get_args

import numpy as np

from ._errors import (
    _entry,
    _malformed_output,
    _refuse,
    _refuse_integer,
    _refuse_type,
    _require_open,
)
from ._ffi_types_mastering_project import SonareSampleDesc, SonareSampleZoneDesc
from ._narrowing import _is_number, _narrow_double, _optional_nonzero
from ._runtime import (
    _UINT8_MAX,
    _UINT32_MAX,
    _check,
    _get_lib,
    _joined_names,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _token_arg,
    _utf8_arg,
    _validate_samples,
)
from ._runtime import _synth_enum_value as _synth_enum_value

# NativeSynth patch enum names (mirror the SonareSynth* enums in
# sonare_c_types.h; 0 / "default" keeps the base patch's value).
# Not seventeen interchangeable choices: selecting a mode blanks every engine
# section but its own, and four engines have nothing to voice until a section is
# supplied -- "fm" needs operators, "modal" a mode table, "percussion" a kit and
# "sample" a sample bank. Each renders exact silence from an otherwise default
# patch; the other thirteen sound and differ from one another.
_SYNTH_ENGINE_MODES = {
    "default": 0,
    "subtractive": 1,
    "fm": 2,
    "karplusStrong": 3,
    "modal": 4,
    "additive": 5,
    "percussion": 6,
    "piano": 7,
    "pipeOrgan": 8,
    "bowedString": 9,
    "reed": 10,
    "brass": 11,
    "flute": 12,
    "pluckedString": 13,
    "vocal": 14,
    "freeReed": 15,
    "harpsichord": 16,
    "sample": 17,
    "singing": 18,
}
# Sample-engine overrides (SonareSampleLoopMode / SonareSampleKeyTrack). Both
# reserve "default" = 0, which keeps whatever the bank recorded for the sample.
_SAMPLE_LOOP_MODES = {
    "default": 0,
    "none": 1,
    "continuous": 2,
    "keyDown": 3,
}
_SAMPLE_KEY_TRACKS = {
    "default": 0,
    "on": 1,
    "off": 2,
}
# SonareSynthRetrigger; "default" = 0 keeps the base patch's mode.
_SYNTH_RETRIGGERS = {
    "default": 0,
    "free": 1,
    "note": 2,
}
# SonareSynthLfoShape; "default" = 0 keeps the base patch's shape.
_SYNTH_LFO_SHAPES = {
    "default": 0,
    "triangle": 1,
    "sine": 2,
    "random": 3,
}
# The sample's own looping, in the SF2 sampleModes values SonareSampleDesc
# takes. A different scale from _SAMPLE_LOOP_MODES above, which is the patch's
# override of it, so the two never share a table.
_SAMPLE_DESC_LOOP_MODES = {
    "none": 0,
    "continuous": 1,
    "keyDown": 3,
}
# SonarePartRigMode.
_PART_RIG_MODES = {"bank": 0, "none": 1, "chain": 2}
# SONARE_PART_RIG_ALL_PARTS: every part of a destination.
PART_RIG_ALL_PARTS = 0xFF
"""Part selector that addresses every part of a destination."""


def _part_rig_mode_value(mode: PartRigMode, key: str) -> int:
    """Resolve a part-rig mode token to the C value."""
    return _PART_RIG_MODES[_token_arg(mode, PART_RIG_MODES, key)]


def _part_rig_mode_name(value: int) -> PartRigMode:
    """Spell a C part-rig mode ordinal."""
    return cast(PartRigMode, _synth_enum_name(value, _PART_RIG_MODES))


def _part_rig_inserts_arg(
    mode_value: int, inserts: Sequence[Mapping[str, object]] | None
) -> bytes | None:
    """Serialize a chain's inserts to the C JSON form; None unless the mode is a chain.

    Each insert is forwarded as given: the core refuses a wrong shape, a missing
    processor or an unknown key by its place under ``inserts``.
    """
    if inserts is None:
        return None
    if isinstance(inserts, (str, bytes, Mapping)) or not isinstance(inserts, Sequence):
        raise _refuse_type("inserts must be a sequence of insert mappings")
    entries = [dict(item) if isinstance(item, Mapping) else item for item in inserts]
    return _utf8_arg(json.dumps(entries), "inserts")


_SYNTH_OSC_WAVEFORMS = {
    "default": 0,
    "sine": 1,
    "saw": 2,
    "square": 3,
    "triangle": 4,
    "noise": 5,
}
_BUILTIN_SYNTH_WAVEFORMS = {
    "sine": 0,
    "saw": 1,
    "sawtooth": 1,
    "square": 2,
    "triangle": 3,
}
_SYNTH_FILTER_MODELS = {
    "default": 0,
    "svf": 1,
    "transistorLadder": 2,
    "diodeLadder": 3,
    "sallenKey": 4,
    "svf4Pole": 5,
}
_SYNTH_FILTER_OUTPUTS = {
    "default": 0,
    "lowPass": 1,
    "bandPass": 2,
    "highPass": 3,
}
_SYNTH_BODY_TYPES = {
    "default": 0,
    "none": 1,
    "guitar": 2,
    "violin": 3,
    "woodTube": 4,
    "brassBell": 5,
    "vocal": 6,
}
_SYNTH_MOD_SOURCES = {
    "none": 0,
    "ampEnv": 1,
    "filterEnv": 2,
    "lfo1": 3,
    "lfo2": 4,
    "velocity": 5,
    "keyTrack": 6,
    "modWheel": 7,
    "random": 8,
    "breath": 9,
    "aftertouch": 10,
    "expressionCc": 11,
    "pitchBend": 12,
}
_SYNTH_MOD_DESTINATIONS = {
    "none": 0,
    "pitchCents": 1,
    "cutoffCents": 2,
    "ampGain": 3,
    "panUnits": 4,
    "resonanceQ": 5,
    "vibratoDepthCents": 6,
    "filterEnvDepth": 7,
    "lfo1RateScale": 8,
    "excitationForce": 9,
    "excitationPosition": 10,
    "excitationBrightness": 11,
    "spectrumMorph": 12,
}
# Controller-profile enum names (mirror SonareControllerInput /
# SonareControllerAxis in sonare_c_engine.h). They describe a device gesture
# rather than a patch field, but sonare_synth_enum_names serves them, so they
# are part of the table set whose contract is "the names the C ABI supplies".
_CONTROLLER_INPUTS = {
    "controlChange": 0,
    "channelPressure": 1,
    "polyPressure": 2,
    "pitchBend": 3,
    "velocity": 4,
}
_CONTROLLER_AXES = {
    "none": 0,
    "excitation": 1,
    "position": 2,
    "brightness": 3,
    "morph": 4,
    "loudness": 5,
    "pitchCents": 6,
    "vibratoDepth": 7,
}
# Articulation names (mirror SonareArticulation in sonare_c_engine.h): what a
# channel does with a note-on while another note on it is still held.
_ARTICULATIONS = {
    "poly": 0,
    "monoRetrigger": 1,
    "monoLegato": 2,
}
# The per-note dimensions MPE carries (mirror SonareMpeDimension), and which
# note a channel-addressed value of one belongs to when several are sounding on
# the channel (mirror SonareNoteTracking).
_MPE_DIMENSIONS = {
    "bend": 0,
    "pressure": 1,
    "timbre": 2,
}
_NOTE_TRACKINGS = {
    "last": 0,
    "lowest": 1,
    "highest": 2,
    "all": 3,
}
SynthEngineMode = Literal[
    "default",
    "subtractive",
    "fm",
    "karplusStrong",
    "modal",
    "additive",
    "percussion",
    "piano",
    "pipeOrgan",
    "bowedString",
    "reed",
    "brass",
    "flute",
    "pluckedString",
    "vocal",
    "freeReed",
    "harpsichord",
    "sample",
    "singing",
]
"""NativeSynth engine mode; ``"default"`` keeps the preset's."""

SynthOscWaveform = Literal["default", "sine", "saw", "square", "triangle", "noise"]
"""NativeSynth oscillator waveform; ``"default"`` keeps the preset's."""

SynthWaveform = Literal["sine", "saw", "sawtooth", "square", "triangle"]
"""Built-in synth waveform; ``"sawtooth"`` is ``"saw"``."""

SynthFilterModel = Literal[
    "default",
    "svf",
    "transistorLadder",
    "diodeLadder",
    "sallenKey",
    "svf4Pole",
]
"""NativeSynth filter model."""

SynthFilterOutput = Literal["default", "lowPass", "bandPass", "highPass"]
"""NativeSynth filter output tap."""

SynthBodyType = Literal["default", "none", "guitar", "violin", "woodTube", "brassBell", "vocal"]
"""NativeSynth body resonator."""

SampleLoopMode = Literal["default", "none", "continuous", "keyDown"]
"""A sample patch's loop override; ``"default"`` keeps the bank's."""

SampleKeyTrack = Literal["default", "on", "off"]
"""A sample patch's key-track override; ``"default"`` keeps the bank's."""

SynthRetrigger = Literal["default", "free", "note"]
"""NativeSynth envelope retrigger mode."""

SynthLfoShape = Literal["default", "triangle", "sine", "random"]
"""NativeSynth LFO shape."""

SampleDescLoopMode = Literal["none", "continuous", "keyDown"]
"""A bank sample's own looping (SoundFont ``sampleModes``)."""

SynthModSource = Literal[
    "none",
    "ampEnv",
    "filterEnv",
    "lfo1",
    "lfo2",
    "velocity",
    "keyTrack",
    "modWheel",
    "random",
    "breath",
    "aftertouch",
    "expressionCc",
    "pitchBend",
]
"""NativeSynth modulation source."""

SynthModDestination = Literal[
    "none",
    "pitchCents",
    "cutoffCents",
    "ampGain",
    "panUnits",
    "resonanceQ",
    "vibratoDepthCents",
    "filterEnvDepth",
    "lfo1RateScale",
    "excitationForce",
    "excitationPosition",
    "excitationBrightness",
    "spectrumMorph",
]
"""NativeSynth modulation destination."""

ControllerInput = Literal[
    "controlChange",
    "channelPressure",
    "polyPressure",
    "pitchBend",
    "velocity",
]
"""How a controller binding's device spells the gesture."""

ControllerAxis = Literal[
    "none",
    "excitation",
    "position",
    "brightness",
    "morph",
    "loudness",
    "pitchCents",
    "vibratoDepth",
]
"""The instrument axis a controller binding drives."""

Articulation = Literal["poly", "monoRetrigger", "monoLegato"]
"""What a channel does with a note-on while another note is held."""

MpeDimension = Literal["bend", "pressure", "timbre"]
"""A per-note MPE dimension."""

NoteTracking = Literal["last", "lowest", "highest", "all"]
"""Which held note a channel-wide controller value follows."""

PartRigMode = Literal["bank", "none", "chain"]
"""Which rig a part of a destination's instrument plays through."""
PART_RIG_MODES: tuple[str, ...] = get_args(PartRigMode)
"""Every accepted part-rig mode token."""


SYNTH_ENUM_TABLES = {
    "engine_modes": tuple(_SYNTH_ENGINE_MODES),
    "waveforms": tuple(_SYNTH_OSC_WAVEFORMS),
    "builtin_waveforms": tuple(_BUILTIN_SYNTH_WAVEFORMS),
    "filter_models": tuple(_SYNTH_FILTER_MODELS),
    "filter_outputs": tuple(_SYNTH_FILTER_OUTPUTS),
    "body_types": tuple(_SYNTH_BODY_TYPES),
    "mod_sources": tuple(_SYNTH_MOD_SOURCES),
    "mod_destinations": tuple(_SYNTH_MOD_DESTINATIONS),
    "controller_inputs": tuple(_CONTROLLER_INPUTS),
    "controller_axes": tuple(_CONTROLLER_AXES),
    "articulations": tuple(_ARTICULATIONS),
    "mpe_dimensions": tuple(_MPE_DIMENSIONS),
    "note_trackings": tuple(_NOTE_TRACKINGS),
    "sample_loop_modes": tuple(_SAMPLE_LOOP_MODES),
    "sample_key_tracks": tuple(_SAMPLE_KEY_TRACKS),
    "retriggers": tuple(_SYNTH_RETRIGGERS),
    "lfo_shapes": tuple(_SYNTH_LFO_SHAPES),
    "part_rig_modes": PART_RIG_MODES,
    "sample_desc_loop_modes": tuple(_SAMPLE_DESC_LOOP_MODES),
}
_SYNTH_ENUM_KINDS = {
    "engine_modes": 0,
    "waveforms": 1,
    "builtin_waveforms": 7,
    "filter_models": 2,
    "filter_outputs": 3,
    "body_types": 4,
    "mod_sources": 5,
    "mod_destinations": 6,
    "controller_inputs": 8,
    "controller_axes": 9,
    "articulations": 10,
    "mpe_dimensions": 11,
    "note_trackings": 12,
    "sample_loop_modes": 13,
    "sample_key_tracks": 14,
    "retriggers": 15,
    "part_rig_modes": 16,
    "sample_desc_loop_modes": 17,
    "lfo_shapes": 18,
}


@_entry("synth_enum_tables")
def synth_enum_tables() -> dict[str, tuple[str, ...]]:
    """Canonical NativeSynth enum-name tables supplied by the C ABI."""
    lib = _get_lib()
    out: dict[str, tuple[str, ...]] = {}
    for key, kind in _SYNTH_ENUM_KINDS.items():
        raw = lib.sonare_synth_enum_names(_to_c_int(kind, "kind"))
        out[key] = tuple(_joined_names(raw, "synth enum names"))
    return out


def _synth_enum_name(value: int, names: Mapping[str, int]) -> str:
    """Spell a C enum ordinal the library returned; one this binding cannot name is a defect."""
    for name, ordinal in names.items():
        if ordinal == value:
            return name
    raise _malformed_output(f"unknown synth enum ordinal: {value}")


def _strip_va_prefix(name: str) -> str:
    return name[3:] if name.startswith("va:") else name


def _sample_loop_value(mode: SampleLoopMode, key: str) -> int:
    """Resolve a :class:`SynthPatch` sample loop override to its C ordinal."""
    return _synth_enum_value(mode, _SAMPLE_LOOP_MODES, key)


def _sample_key_track_value(value: SampleKeyTrack, key: str) -> int:
    """Resolve a :class:`SynthPatch` sample key-track override to its C ordinal."""
    return _synth_enum_value(value, _SAMPLE_KEY_TRACKS, key)


def _controller_input_value(value: ControllerInput, key: str) -> int:
    """Resolve a controller-profile input spelling to its C ordinal."""
    return _synth_enum_value(value, _CONTROLLER_INPUTS, key)


def _controller_axis_value(value: ControllerAxis, key: str) -> int:
    """Resolve a controller-profile axis spelling to its C ordinal."""
    return _synth_enum_value(value, _CONTROLLER_AXES, key)


def _articulation_value(value: Articulation, key: str) -> int:
    """Resolve an articulation spelling to its C ordinal."""
    return _synth_enum_value(value, _ARTICULATIONS, key)


def _articulation_name(value: int) -> Articulation:
    """Spell a C articulation ordinal."""
    return cast(Articulation, _synth_enum_name(value, _ARTICULATIONS))


def _mpe_dimension_value(value: MpeDimension, key: str) -> int:
    """Resolve an MPE dimension spelling to its C ordinal."""
    return _synth_enum_value(value, _MPE_DIMENSIONS, key)


def _note_tracking_value(value: NoteTracking, key: str) -> int:
    """Resolve a note-tracking spelling to its C ordinal."""
    return _synth_enum_value(value, _NOTE_TRACKINGS, key)


def _note_tracking_name(value: int) -> NoteTracking:
    """Spell a C note-tracking ordinal."""
    return cast(NoteTracking, _synth_enum_name(value, _NOTE_TRACKINGS))


def _sample_desc_loop_value(mode: SampleDescLoopMode, key: str) -> int:
    """Resolve a sample's own loop mode token to its SF2 ``sampleModes`` value."""
    return _synth_enum_value(mode, _SAMPLE_DESC_LOOP_MODES, key)


def _unsigned(value: int, name: str, limit: int) -> int:
    """Range-check an unsigned integer bound for a C field.

    ctypes truncates an out-of-range integer into the field's width silently, so
    a negative index would arrive as a huge one and a key of 300 as 44. Rejecting
    here is what keeps the value the C ABI sees the one the caller wrote.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        message = f"{name} must be an integer"
        raise _refuse_integer(name, message) if _is_number(value) else _refuse_type(message)
    if value < 0 or value > limit:
        raise _refuse(f"{name} must be in [0, {limit}]")
    return value


def _finite(value: float, name: str) -> float:
    v = _narrow_double(value, name)
    if not np.isfinite(v):
        raise _refuse(f"{name} must be finite")
    return v


class SampleBank:
    """Host-supplied PCM the NativeSynth sample engine plays.

    The other door into sampled playback is a SoundFont
    (:meth:`Project.load_sound_font`), which brings a container and its own
    generator model. This one takes float frames a caller prepared elsewhere,
    described by nothing but a keymap: add each sample, map key/velocity
    rectangles onto them, and bind the bank alongside a
    :class:`SynthPatch` whose ``engine_mode`` is ``"sample"``.

    A bank is built once and then read as immutable data. Add every sample and
    zone BEFORE a render starts -- the sample pool is contiguous and moves as it
    grows, so a sample added while something sounds invalidates the voices
    reading it.

    The native handle is not garbage-collector aware, so close it deterministically
    -- as a context manager, or with :meth:`close`. ``__del__`` is a backstop
    only.

    Example::

        import numpy as np
        from libsonare import ProjectInstrumentBinding, SampleBank, SynthPatch

        tone = np.sin(2 * np.pi * 440.0 * np.arange(24000) / 48000.0).astype(np.float32)
        with SampleBank() as bank:
            index = bank.add_sample(tone, root_key=69, source_rate=48000.0)
            bank.add_zone(0, sample_index=index)  # the whole keyboard
            patch = SynthPatch(engine_mode="sample", sample_set=0, sample_bank=bank)
            binding = ProjectInstrumentBinding(instrument=patch)
            audio = project.bounce(synth_instruments=[binding], total_frames=24000).channels
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("SampleBank")
    def __init__(self) -> None:
        # Set first so a failed create or a missing build leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle: ctypes.c_void_p | None = None
        lib = _get_lib()
        handle = ctypes.c_void_p()
        _check(lib.sonare_sample_bank_create(ctypes.byref(handle)))
        self._handle = handle

    # -- lifecycle ----------------------------------------------------------

    @_entry("SampleBank.close")
    def close(self) -> None:
        """Release the native bank (idempotent). Nothing rendering may hold it."""
        if self._handle is not None:
            _get_lib().sonare_sample_bank_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> SampleBank:
        """Return the object itself for ``with`` use."""
        return self

    def __exit__(self, *_exc: object) -> None:
        """Close the object on leaving the ``with`` block; never suppresses exceptions."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "SampleBank")

    # -- content ------------------------------------------------------------

    @_entry("SampleBank.add_sample")
    def add_sample(
        self,
        data: Sequence[float] | np.ndarray,
        *,
        root_key: int = 60,
        fine_tune_cents: float = 0.0,
        source_rate: float | None = None,
        loop_start: int = 0,
        loop_end: int = 0,
        loop_mode: SampleDescLoopMode = "none",
    ) -> int:
        """Copy mono float frames into the bank and return the new sample's index.

        Args:
            data: Mono PCM as a 1-D float buffer; copied, so it may be freed
                afterwards. Decoding is the caller's job.
            root_key: MIDI key at which the sample sounds at its recorded pitch
                (0-127). The default is middle C, which is also what the C ABI
                substitutes for an unset value.
            fine_tune_cents: Tuning offset applied on top of the recorded pitch.
            source_rate: Rate the sample was recorded at. ``None`` means "the
                render's own rate", so a bank built without rate information
                plays back unresampled; an explicit 0 raises.
            loop_start / loop_end: Frame offsets INSIDE this sample. Both are
                clamped into it, and a loop that survives the clamp empty is
                dropped, so a malformed loop plays as an unlooped sample rather
                than as a wrap over nothing.
            loop_mode: The sample's own looping: ``"none"`` (the default),
                ``"continuous"`` or ``"keyDown"``, the SoundFont ``sampleModes``
                values 0, 1 and 3. :attr:`SynthPatch.sample_loop` overrides this
                per patch.

        Raises:
            SonareValueError: For an empty, non-finite or non-1-D buffer, or an
                argument outside its field's range.
            SonareError: ``OutOfMemory`` when the bank would exceed its sample
                budget.
        """
        buf = _validate_samples(data, arg_name="data")
        desc = SonareSampleDesc(
            root_key=_unsigned(root_key, "root_key", 127),
            fine_tune_cents=_finite(fine_tune_cents, "fine_tune_cents"),
            source_rate=_finite(
                _optional_nonzero(source_rate, "source_rate", "the render's own rate"),
                "source_rate",
            ),
            loop_start=_unsigned(loop_start, "loop_start", _UINT32_MAX),
            loop_end=_unsigned(loop_end, "loop_end", _UINT32_MAX),
            loop_mode=_sample_desc_loop_value(loop_mode, "loop_mode"),
        )
        c_array, length = _to_c_float_array(buf)
        out_index = ctypes.c_uint32()
        _check(
            _get_lib().sonare_sample_bank_add_sample(
                self._require_handle(),
                c_array,
                _to_c_size_t(length, "length"),
                ctypes.byref(desc),
                ctypes.byref(out_index),
            ),
            flattened="desc",
        )
        return int(out_index.value)

    @_entry("SampleBank.add_zone")
    def add_zone(
        self,
        set_index: int,
        *,
        sample_index: int,
        key_lo: int = 0,
        key_hi: int = 127,
        vel_lo: int = 1,
        vel_hi: int = 127,
        tune_cents: float = 0.0,
        gain: float = 1.0,
        pan_units: float = 0.0,
    ) -> None:
        """Append a key/velocity rectangle to keymap set ``set_index``.

        A patch names a set (:attr:`SynthPatch.sample_set`); the first zone in it
        covering a note is the one that sounds, so order zones from specific to
        general. Sets are dense: adding to set 3 creates sets 0-2 empty.

        Every bound defaults on its own, so narrowing one edge never collapses
        another: the defaults here spell out the whole keyboard at every
        velocity, and passing a bound explicitly narrows only that edge. A zero
        passed through reaches the C ABI's own sentinels -- an upper bound of
        zero reads as 127 and ``vel_lo`` of zero as 1, velocity zero being a
        note-off rather than a dynamic, while ``key_lo`` of zero is simply the
        lowest key. The one rectangle this cannot express is the single key 0.

        Args:
            set_index: Keymap set to append to (below 4096).
            sample_index: A value :meth:`add_sample` returned.
            key_lo / key_hi: Inclusive MIDI key bounds (0-127).
            vel_lo / vel_hi: Inclusive MIDI velocity bounds (1-127).
            tune_cents: Added to the sample's own fine tuning.
            gain: Linear zone gain; ``0.0`` reads as 1.0.
            pan_units: SoundFont pan units, clamped to -500..500.

        Raises:
            SonareValueError: For an argument outside its field's range.
            SonareValueError: For a sample index the bank does
                not have, an inverted key or velocity range, or a ``set_index``
                at or above 4096.
        """
        zone = SonareSampleZoneDesc(
            key_lo=_unsigned(key_lo, "key_lo", _UINT8_MAX),
            key_hi=_unsigned(key_hi, "key_hi", _UINT8_MAX),
            vel_lo=_unsigned(vel_lo, "vel_lo", _UINT8_MAX),
            vel_hi=_unsigned(vel_hi, "vel_hi", _UINT8_MAX),
            sample_index=_unsigned(sample_index, "sample_index", _UINT32_MAX),
            tune_cents=_finite(tune_cents, "tune_cents"),
            gain=_finite(gain, "gain"),
            pan_units=_finite(pan_units, "pan_units"),
        )
        _check(
            _get_lib().sonare_sample_bank_add_zone(
                self._require_handle(),
                ctypes.c_uint32(_unsigned(set_index, "set_index", _UINT32_MAX)),
                ctypes.byref(zone),
            )
        )

    @property
    def sample_count(self) -> int:
        """Samples added so far."""
        return self._count("sonare_sample_bank_sample_count")

    @property
    def set_count(self) -> int:
        """Keymap sets the bank has (one past the highest index used)."""
        return self._count("sonare_sample_bank_set_count")

    def _count(self, symbol: str) -> int:
        out = ctypes.c_size_t()
        _check(getattr(_get_lib(), symbol)(self._require_handle(), ctypes.byref(out)))
        return int(out.value)
