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
from collections.abc import Mapping, Sequence

import numpy as np

from ._ffi_types_mastering_project import SonareSampleDesc, SonareSampleZoneDesc
from ._runtime import (
    _UINT8_MAX,
    _UINT32_MAX,
    ErrorCode,
    SonareValueError,
    _check,
    _generic_error,
    _get_lib,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _validate_samples,
)
from ._runtime import _synth_enum_value as _synth_enum_value

# NativeSynth patch enum names (mirror the SonareSynth* enums in
# sonare_c_types.h; 0 / "default" keeps the base patch's value).
_SYNTH_ENGINE_MODES = {
    "default": 0,
    "subtractive": 1,
    "fm": 2,
    "karplus-strong": 3,
    "modal": 4,
    "additive": 5,
    "percussion": 6,
    "piano": 7,
    "pipe-organ": 8,
    "bowed-string": 9,
    "reed": 10,
    "brass": 11,
    "flute": 12,
    "plucked-string": 13,
    "vocal": 14,
    "free-reed": 15,
    "harpsichord": 16,
    "sample": 17,
}
# Sample-engine overrides (SonareSampleLoopMode / SonareSampleKeyTrack). Both
# reserve "default" = 0, which keeps whatever the bank recorded for the sample.
# They are not part of SYNTH_ENUM_TABLES: sonare_synth_enum_names has no kind
# for them, and that table's contract is "the names the C ABI supplies".
_SAMPLE_LOOP_MODES = {
    "default": 0,
    "none": 1,
    "continuous": 2,
    "key-down": 3,
}
_SAMPLE_KEY_TRACKS = {
    "default": 0,
    "on": 1,
    "off": 2,
}
# The sample's own looping, in the SF2 sampleModes values SonareSampleDesc
# takes. A different scale from _SAMPLE_LOOP_MODES above, which is the patch's
# override of it, so the two never share a table.
_SAMPLE_DESC_LOOP_MODES = {
    "none": 0,
    "continuous": 1,
    "key-down": 3,
}
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
    "moog-ladder": 2,
    "diode-ladder": 3,
    "sallen-key": 4,
}
_SYNTH_FILTER_OUTPUTS = {
    "default": 0,
    "lowpass": 1,
    "bandpass": 2,
    "highpass": 3,
}
_SYNTH_BODY_TYPES = {
    "default": 0,
    "none": 1,
    "guitar": 2,
    "violin": 3,
    "wood-tube": 4,
    "brass-bell": 5,
    "vocal": 6,
}
_SYNTH_MOD_SOURCES = {
    "none": 0,
    "amp-env": 1,
    "filter-env": 2,
    "lfo1": 3,
    "lfo2": 4,
    "velocity": 5,
    "key-track": 6,
    "mod-wheel": 7,
    "random": 8,
}
_SYNTH_MOD_DESTINATIONS = {
    "none": 0,
    "pitch-cents": 1,
    "cutoff-cents": 2,
    "amp-gain": 3,
    "pan-units": 4,
    "resonance-q": 5,
    "vibrato-depth-cents": 6,
    "filter-env-depth": 7,
    "lfo1-rate-scale": 8,
}
SYNTH_ENUM_TABLES = {
    "engine_modes": tuple(_SYNTH_ENGINE_MODES),
    "waveforms": tuple(_SYNTH_OSC_WAVEFORMS),
    "builtin_waveforms": tuple(_BUILTIN_SYNTH_WAVEFORMS),
    "filter_models": tuple(_SYNTH_FILTER_MODELS),
    "filter_outputs": tuple(_SYNTH_FILTER_OUTPUTS),
    "body_types": tuple(_SYNTH_BODY_TYPES),
    "mod_sources": tuple(_SYNTH_MOD_SOURCES),
    "mod_destinations": tuple(_SYNTH_MOD_DESTINATIONS),
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
}


def synth_enum_tables() -> dict[str, tuple[str, ...]]:
    """Canonical NativeSynth enum-name tables supplied by the C ABI."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_synth_enum_names"):
        raise RuntimeError("libsonare was built without the NativeSynth enum ABI")
    out: dict[str, tuple[str, ...]] = {}
    for key, kind in _SYNTH_ENUM_KINDS.items():
        raw = lib.sonare_synth_enum_names(_to_c_int(kind, "kind"))
        if not raw:
            out[key] = ()
            continue
        out[key] = tuple(name for name in raw.decode("utf-8").split("\n") if name)
    return out


def _synth_enum_name(value: int, names: Mapping[str, int]) -> str | int:
    for name, ordinal in names.items():
        if ordinal == value:
            return name
    return value


def _strip_va_prefix(name: str) -> str:
    return name[3:] if name.startswith("va:") else name


def _sample_loop_value(mode: str | int) -> int:
    """Resolve a :class:`SynthPatch` sample loop override to its C ordinal."""
    return _synth_enum_value(mode, _SAMPLE_LOOP_MODES, "sample loop mode")


def _sample_key_track_value(value: str | int) -> int:
    """Resolve a :class:`SynthPatch` sample key-track override to its C ordinal."""
    return _synth_enum_value(value, _SAMPLE_KEY_TRACKS, "sample key track")


def _sample_desc_loop_value(mode: str | int) -> int:
    """Resolve a sample's own loop mode to its SF2 ``sampleModes`` value.

    Bounded by the field's own domain rather than by the spellings below: an
    SF2 zone carries any of the four, and the core reads anything but 1 or 3 as
    unlooped. That makes the reserved 2 a documented alternative rather than the
    silent substitution an out-of-domain value would get.
    """
    # sampleModes is two bits wide; 2 is reserved and means no loop.
    if isinstance(mode, int) and not isinstance(mode, bool) and 0 <= mode <= 3:
        return mode
    return _synth_enum_value(mode, _SAMPLE_DESC_LOOP_MODES, "sample loop mode")


def _unsigned(value: int, name: str, limit: int) -> int:
    """Range-check an unsigned integer bound for a C field.

    ctypes truncates an out-of-range integer into the field's width silently, so
    a negative index would arrive as a huge one and a key of 300 as 44. Rejecting
    here is what keeps the value the C ABI sees the one the caller wrote.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise SonareValueError(f"{name} must be an integer")
    if value < 0 or value > limit:
        raise SonareValueError(f"{name} must be in [0, {limit}]")
    return value


def _finite(value: float, name: str) -> float:
    v = float(value)
    if not np.isfinite(v):
        raise SonareValueError(f"{name} must be a finite number")
    return v


class SampleBank:
    """Host-supplied PCM the NativeSynth sample engine plays.

    The other door into sampled playback is a SoundFont
    (:meth:`Project.load_soundfont`), which brings a container and its own
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
        from libsonare import SampleBank, SynthPatch

        tone = np.sin(2 * np.pi * 440.0 * np.arange(24000) / 48000.0).astype(np.float32)
        with SampleBank() as bank:
            index = bank.add_sample(tone, root_key=69, source_rate=48000.0)
            bank.add_zone(0, sample_index=index)  # the whole keyboard
            audio = project.bounce_with_synth_instrument(
                SynthPatch(engine_mode="sample", sample_set=0),
                sample_bank=bank,
                total_frames=24000,
            )
    """

    def __init__(self) -> None:
        # Set first so a failed create or a missing build leaves a valid
        # attribute for __del__/close() instead of raising AttributeError.
        self._handle: ctypes.c_void_p | None = None
        lib = _get_lib()
        if not hasattr(lib, "sonare_sample_bank_create"):
            raise RuntimeError("libsonare was built without the sample-bank ABI")
        handle = lib.sonare_sample_bank_create()
        if not handle:
            raise _generic_error(int(ErrorCode.OUT_OF_MEMORY))
        self._handle = ctypes.c_void_p(handle)

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        """Release the native bank (idempotent). Nothing rendering may hold it."""
        if self._handle is not None:
            _get_lib().sonare_sample_bank_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> SampleBank:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        if self._handle is None:
            raise RuntimeError("SampleBank is closed")
        return self._handle

    # -- content ------------------------------------------------------------

    def add_sample(
        self,
        data: Sequence[float] | np.ndarray,
        *,
        root_key: int = 60,
        fine_tune_cents: float = 0.0,
        source_rate: float = 0.0,
        loop_start: int = 0,
        loop_end: int = 0,
        loop_mode: str | int = 0,
    ) -> int:
        """Copy mono float frames into the bank and return the new sample's index.

        Args:
            data: Mono PCM as a 1-D float buffer; copied, so it may be freed
                afterwards. Decoding is the caller's job.
            root_key: MIDI key at which the sample sounds at its recorded pitch
                (0-127). The default is middle C, which is also what the C ABI
                substitutes for an unset value.
            fine_tune_cents: Tuning offset applied on top of the recorded pitch.
            source_rate: Rate the sample was recorded at. ``0.0`` means "the
                render's own rate", so a bank built without rate information
                plays back unresampled.
            loop_start / loop_end: Frame offsets INSIDE this sample. Both are
                clamped into it, and a loop that survives the clamp empty is
                dropped, so a malformed loop plays as an unlooped sample rather
                than as a wrap over nothing.
            loop_mode: The sample's own looping, in SoundFont ``sampleModes``
                values: ``"none"`` (0), ``"continuous"`` (1) or ``"key-down"``
                (3). :attr:`SynthPatch.sample_loop` overrides this per patch.

        Raises:
            SonareValueError: For an empty, non-finite or non-1-D buffer, or an
                argument outside its field's range.
            SonareError: ``OutOfMemory`` when the bank would exceed its sample
                budget.
        """
        buf = _validate_samples("SampleBank.add_sample", data, arg_name="data")
        desc = SonareSampleDesc(
            root_key=_unsigned(root_key, "root_key", 127),
            fine_tune_cents=_finite(fine_tune_cents, "fine_tune_cents"),
            source_rate=_finite(source_rate, "source_rate"),
            loop_start=_unsigned(loop_start, "loop_start", _UINT32_MAX),
            loop_end=_unsigned(loop_end, "loop_end", _UINT32_MAX),
            loop_mode=_sample_desc_loop_value(loop_mode),
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
            )
        )
        return int(out_index.value)

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
            SonareError: ``InvalidParameter`` for a sample index the bank does
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
