"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import functools
import json
import operator
from collections.abc import Callable, Mapping, Sequence
from typing import Any, Literal, TypeAlias, cast, get_args

import numpy as np

from ._errors import _entry, _malformed_output, _refuse, _refuse_type, _require_open
from ._ffi import (
    SONARE_OK,
    SONARE_VC_PRESET_NEUTRAL_MONITOR,
    SonareRealtimeVoiceChangerConfig,
    SonareVoiceChangeConfig,
)
from ._narrowing import _domain_float, _is_number, _resolve_zero_default
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _as_float32_buffer,
    _bool_flag,
    _check,
    _check_realtime,
    _from_c_float_array,
    _get_lib,
    _joined_names,
    _max_block_size,
    _narrow_int,
    _not_planar_channels,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _token_ordinal,
    _utf8_arg,
    _validate_samples,
)
from ._types_engine import PlanarChannels
from ._types_enums import _VOICE_PRESET_IDS, VoicePresetId

_FORMANT_MODES = {"relative": 0, "absolute": 1}

FormantMode: TypeAlias = Literal["relative", "absolute"]
"""How ``formant_factor`` relates to the pitch shift."""
VoiceEngine: TypeAlias = Literal["grain", "psola"]
"""Pitch-shift engine of the voice changer."""
PitchCorrectionMode: TypeAlias = Literal["off", "auto", "robot"]
"""Pitch correction: off, snap to the key scale (``"auto"``) or to one pitch class (``"robot"``)."""
ScaleKind: TypeAlias = Literal["major", "naturalMinor", "chromatic"]
"""Scale the pitch correction snaps to."""
VocoderCarrierSource: TypeAlias = Literal["internal", "external"]
"""Where the vocoder carrier comes from: the built-in oscillator or an external input."""
VocoderCarrierWave: TypeAlias = Literal["saw", "pulse", "ensemble"]
"""Waveform of the built-in vocoder carrier."""
CharacterType: TypeAlias = Literal["none", "megaphone", "radio", "bbdChorus", "strobo"]
"""Character stage type."""
ReverbVariation: TypeAlias = Literal["reverb", "echo", "tempoDelay", "dubEcho"]
"""Space stage variation; every variation shares ``reverb_mix``."""
NoteDivision: TypeAlias = Literal["quarter", "eighth", "dottedEighth", "sixteenth"]
"""Note length a tempo-synced delay repeats at."""
MidiRelease: TypeAlias = Literal["hold", "input"]
"""What the voices do once every held note is released: keep the last one or follow the input."""

# Enum-valued POD fields: token spellings in the order of the C ordinals.
_ENUM_FIELD_NAMES: dict[str, tuple[str, ...]] = {
    "formant_mode": get_args(FormantMode),
    "voice_engine": get_args(VoiceEngine),
    "pitch_correction_mode": get_args(PitchCorrectionMode),
    "pitch_correction_scale": get_args(ScaleKind),
    "vocoder_carrier_source": get_args(VocoderCarrierSource),
    "vocoder_carrier_wave": get_args(VocoderCarrierWave),
    "character_type": get_args(CharacterType),
    "reverb_variation": get_args(ReverbVariation),
    "reverb_variation_division": get_args(NoteDivision),
    "midi_release": get_args(MidiRelease),
}
# On/off POD fields: a Python bool on this side, the C 0/1 flag on the other.
_BOOL_FIELD_NAMES = frozenset({"limiter_enable_isp_limiter", "vocoder_enabled"})
_HARMONY_STEP_COUNT = 3


def _enum_ordinal(field: str, value: object) -> int:
    """Map an enum spelling to its C ordinal, refusing a non-string or an unknown one."""
    names = _ENUM_FIELD_NAMES[field]
    if not isinstance(value, str):
        raise _refuse_type(f"{field} must be one of {', '.join(names)}")
    if value not in names:
        raise _refuse(f"{field} must be one of {', '.join(names)}")
    return names.index(value)


def _enum_name(field: str, ordinal: int) -> str:
    """Map a C ordinal read from the library to its spelling."""
    names = _ENUM_FIELD_NAMES[field]
    if not 0 <= int(ordinal) < len(names):
        raise _malformed_output(f"unknown {field.replace('_', ' ')} ordinal: {ordinal}")
    return names[int(ordinal)]


@_entry("voice_change")
def voice_change(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    pitch_semitones: float = 0.0,
    formant_factor: float = 1.0,
    formant_mode: FormantMode = "relative",
) -> np.ndarray:
    """Apply a voice-change effect with independent pitch and formant control.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        pitch_semitones: Pitch shift in semitones (positive = up).
        formant_factor: Formant scaling factor. Its meaning depends on
            ``formant_mode``.
        formant_mode: ``"relative"`` (default) applies ``formant_factor`` as a
            warp after the pitch shift, so the formants end up at the factor
            times the pitch ratio ``2**(pitch_semitones / 12)``. ``"absolute"``
            makes ``formant_factor`` the formant shift relative to the input
            (1.0 keeps the formants where they were): the warp applied is
            ``formant_factor / 2**(pitch_semitones / 12)``.

    Returns:
        ``float32`` array of voice-changed samples.

    Raises:
        SonareValueError: ``formant_mode`` is not ``"relative"`` or
            ``"absolute"``, a relative ``formant_factor`` is non-finite or
            outside [0.55, 1.65], or an absolute request needs a warp outside
            [0.55, 1.65]. The message names the reachable ``formant_factor``
            range.
    """
    _validate_samples(samples)
    if not isinstance(formant_mode, str) or formant_mode not in _FORMANT_MODES:
        raise _refuse(f"formant_mode must be one of {', '.join(_FORMANT_MODES)}")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareVoiceChangeConfig(
        pitch_semitones=_to_c_float(pitch_semitones, "pitch_semitones"),
        formant_factor=_to_c_float(formant_factor, "formant_factor"),
        formant_mode=_FORMANT_MODES[formant_mode],
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_voice_change(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        # A refused factor is an argument refusal in either formant mode.
        _check(rc, flattened="config")
        return _from_c_float_array(out, out_length.value)


RealtimeVoiceChangerPreset = dict[str, Any]
"""A voice-changer configuration document with camelCase keys.

Either a preset document (metadata plus a nested ``dsp`` section or a ``macros``
map) or a flat config. JSON text is parsed by the caller (``json.loads``) first.
"""


def _voice_config_to_json(config: object) -> bytes:
    """A preset id is checked against the built-in ids; a mapping is a document."""
    if isinstance(config, str):
        _token_ordinal(config, _VOICE_PRESET_IDS, "config")
        return _utf8_arg(config, "config")
    if isinstance(config, Mapping):
        return _utf8_arg(json.dumps(dict(config), separators=(",", ":")), "config")
    raise _refuse_type(
        "config must be a preset id or a mapping (parse JSON text with json.loads first)"
    )


def _voice_config_to_pod(lib: ctypes.CDLL, config: object) -> SonareRealtimeVoiceChangerConfig:
    """Resolve a preset id or a document mapping into the flat C config."""
    pod = SonareRealtimeVoiceChangerConfig()
    _check(
        lib.sonare_realtime_voice_changer_config_from_json(
            _voice_config_to_json(config), ctypes.byref(pod)
        )
    )
    return pod


class RealtimeVoiceChanger:
    """Streaming realtime voice changer backed by the libsonare C API."""

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("RealtimeVoiceChanger")
    def __init__(
        self,
        sample_rate: int,
        *,
        num_channels: int,
        config: VoicePresetId | RealtimeVoiceChangerPreset | RealtimeVoiceChangerConfig = (
            "neutralMonitor"
        ),
        max_block_size: int | None = None,
    ) -> None:
        """Create a voice changer for ``num_channels`` (1 or 2) at ``sample_rate``.

        ``config`` is a built-in preset id, a configuration document mapping or a
        :class:`RealtimeVoiceChangerConfig` record, the same three kinds
        :meth:`set_config` takes.
        ``max_block_size`` bounds each native call; omitted, the library default
        applies. The ``process_*`` methods chunk longer buffers to it.
        """
        # Set first so a failed loader or create leaves a valid attribute for
        # __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        self._channels = _narrow_int(num_channels, "num_channels", _C_INT_MIN, _C_INT_MAX)
        c_sample_rate = _to_c_int(sample_rate, "sample_rate")
        c_block = _to_c_int(
            _resolve_zero_default(max_block_size, "max_block_size"), "max_block_size"
        )
        c_channels = _to_c_int(self._channels, "num_channels")
        pod = (
            config._to_pod()
            if isinstance(config, RealtimeVoiceChangerConfig)
            else _voice_config_to_pod(self._lib, config)
        )
        rc = self._lib.sonare_realtime_voice_changer_create(
            ctypes.byref(pod), c_sample_rate, c_block, c_channels, ctypes.byref(self._handle)
        )
        _check(rc)

    @_entry("RealtimeVoiceChanger.close")
    def close(self) -> None:
        """Release the native handle. Idempotent, so a double close is a no-op."""
        if self._handle:
            self._lib.sonare_realtime_voice_changer_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> RealtimeVoiceChanger:
        """Return self so the object works as a context manager."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close on leaving the ``with`` block; never suppresses an exception."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "RealtimeVoiceChanger")

    @_entry("RealtimeVoiceChanger.reset")
    def reset(self) -> None:
        """Clear the processing state. Raises ``SonareError`` (``InvalidState``) when closed."""
        rc = self._lib.sonare_realtime_voice_changer_reset(self._require_handle())
        _check(rc)

    @_entry("RealtimeVoiceChanger.set_config")
    def set_config(
        self, config: VoicePresetId | RealtimeVoiceChangerPreset | RealtimeVoiceChangerConfig
    ) -> None:
        """Apply a preset id, a configuration document mapping or a config record.

        A :class:`RealtimeVoiceChangerConfig` record takes the realtime-safe path with
        no JSON round-trip. Raises ``TypeError`` for any other type,
        ``SonareValueError`` for an unknown preset id or a record field outside its
        range, and ``SonareError`` when the document is refused or the changer closed;
        the previous configuration stays in effect on a refusal.
        """
        if isinstance(config, RealtimeVoiceChangerConfig):
            self._set_record(config)
            return
        pod = _voice_config_to_pod(self._lib, config)
        rc = self._lib.sonare_realtime_voice_changer_set_config(
            self._require_handle(), ctypes.byref(pod)
        )
        _check(rc)

    @property
    def max_block_size(self) -> int:
        """The largest block one native call accepts; ``process_*`` chunk longer input to it."""
        return _max_block_size(
            self._lib.sonare_realtime_voice_changer_max_block_size, self._require_handle()
        )

    @property
    def latency_samples(self) -> int:
        """Processing latency in samples."""
        out = ctypes.c_int()
        rc = self._lib.sonare_realtime_voice_changer_latency_samples(
            self._require_handle(), ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

    @property
    def non_finite_discard_count(self) -> int:
        """Return the blocks in which the chain discarded its own state.

        Advisory telemetry, and the only thing that separates a degraded stream
        from a clean one. Every stage of this chain leaves an in-domain finite
        value where a non-finite one was -- the input scrub and the
        inter-sample-peak limiter substitute silence, the sample-domain limiter
        folds an infinity onto its ceiling -- so the output stays finite, in
        range and free of any error while carrying samples unrelated to the
        input. A non-zero count is what says the samples in between were not
        computed from what was supplied.

        Monotonic for the lifetime of the handle. The unit is one processed
        block, never a channel, so a stereo block that discards on both channels
        adds one and the number does not depend on a dimension you did not
        choose.
        """
        out = ctypes.c_size_t()
        rc = self._lib.sonare_realtime_voice_changer_non_finite_discard_count(
            self._require_handle(), ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

    @_entry("RealtimeVoiceChanger.config_json")
    def config_json(self) -> str:
        """Return the live (normalized) configuration as a JSON document.

        Useful for syncing UI state or roundtripping the post-normalize
        configuration into a different language binding.
        """
        out = ctypes.c_char_p()
        rc = self._lib.sonare_realtime_voice_changer_config_json(
            self._require_handle(), ctypes.byref(out)
        )
        _check(rc)
        try:
            if not out:
                return ""
            return ctypes.string_at(out).decode("utf-8")
        finally:
            if out:
                self._lib.sonare_free_string(out)

    @property
    def config(self) -> RealtimeVoiceChangerConfig:
        """The live (normalized) configuration as a :class:`RealtimeVoiceChangerConfig` record.

        Read without JSON on either side; :meth:`config_json` gives the document.
        """
        pod = SonareRealtimeVoiceChangerConfig()
        rc = self._lib.sonare_realtime_voice_changer_get_config(
            self._require_handle(), ctypes.byref(pod)
        )
        _check(rc)
        return RealtimeVoiceChangerConfig._from_pod(pod)

    def _set_record(self, config: RealtimeVoiceChangerConfig) -> None:
        pod = config._to_pod()
        rc = self._lib.sonare_realtime_voice_changer_set_config(
            self._require_handle(), ctypes.byref(pod)
        )
        _check(rc)

    @_entry("RealtimeVoiceChanger.process_mono")
    def process_mono(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        carrier: Sequence[float] | list[float] | np.ndarray | None = None,
    ) -> np.ndarray:
        """Process a mono buffer block-by-block and return the result as ndarray.

        ``carrier`` is an external vocoder carrier of the same length as
        ``samples``. It is read only while the vocoder runs with the ``"external"``
        carrier source; with ``"internal"`` it is ignored. A carrier of a different
        length raises ``SonareValueError``. With an external carrier configured and
        none given, the call raises ``SonareError`` (invalid state) and writes nothing.

        Returns a ``numpy.ndarray`` of dtype ``float32`` (changed from
        ``list[float]`` in the prior implementation). The realtime path is
        zero-copy: each ``max_block_size`` block aliases the input/output
        numpy buffers as ``c_float*`` via ``ctypes.from_buffer``, so the
        per-block Python overhead is independent of block size.

        Cross-surface contract note: this Python facade accepts an
        arbitrarily long buffer and internally chunks it into
        ``max_block_size`` blocks as a convenience. The Node, WASM, and C ABI
        surfaces instead REJECT a single block larger than ``max_block_size``
        (the caller must chunk). Keep this in mind when porting realtime code
        between surfaces.
        """
        in_buf = _as_float32_buffer(samples)
        carrier_buf = None if carrier is None else _as_float32_buffer(carrier, arg_name="carrier")
        if carrier_buf is not None and int(carrier_buf.shape[0]) != int(in_buf.shape[0]):
            raise _refuse("carrier must have the same length as samples")
        return self._process_mono(in_buf, carrier_buf)

    def _process_mono(self, in_buf: np.ndarray, carrier_buf: np.ndarray | None) -> np.ndarray:
        total = int(in_buf.shape[0])
        out_buf = np.empty(total, dtype=np.float32)
        step = self.max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            in_block = in_buf[pos : pos + length]
            out_block = out_buf[pos : pos + length]
            # `from_buffer` requires C-contiguous memory; slices of a
            # contiguous 1-D float32 array are guaranteed contiguous.
            c_in = (ctypes.c_float * length).from_buffer(in_block)
            c_out = (ctypes.c_float * length).from_buffer(out_block)
            c_carrier = (
                None
                if carrier_buf is None
                else (ctypes.c_float * length).from_buffer(carrier_buf[pos : pos + length])
            )
            rc = self._lib.sonare_realtime_voice_changer_process_mono(
                self._require_handle(), c_in, c_carrier, c_out, _to_c_size_t(length, "length")
            )
            _check_realtime(rc)
        return out_buf

    @_entry("RealtimeVoiceChanger.process_interleaved")
    def process_interleaved(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        num_channels: int | None = None,
        carrier: Sequence[float] | list[float] | np.ndarray | None = None,
    ) -> np.ndarray:
        """Process an interleaved (LRLR...) buffer block-by-block.

        Returns a ``numpy.ndarray`` of dtype ``float32`` in the same
        interleaved layout as the input. ``carrier`` is a mono external vocoder
        carrier with one sample per frame, shared by every channel; a different
        length raises ``SonareValueError``.
        """
        return self._process_interleaved(samples, num_channels, carrier)

    def _process_interleaved(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        num_channels: int | None,
        carrier: Sequence[float] | list[float] | np.ndarray | None,
    ) -> np.ndarray:
        # Narrowed rather than coerced: int() takes 2.7 as 2, which lays the
        # buffer out as a stereo interleave nobody asked for.
        ch = (
            self._channels
            if num_channels is None
            else _narrow_int(num_channels, "num_channels", _C_INT_MIN, _C_INT_MAX)
        )
        in_buf = _as_float32_buffer(samples)
        total_samples = int(in_buf.shape[0])
        if ch <= 0 or total_samples % ch != 0:
            raise _refuse("interleaved samples length must be divisible by num_channels")
        frames = total_samples // ch
        carrier_buf = None if carrier is None else _as_float32_buffer(carrier, arg_name="carrier")
        if carrier_buf is not None and int(carrier_buf.shape[0]) != frames:
            raise _refuse("carrier must hold one sample per frame of samples")
        out_buf = np.empty(total_samples, dtype=np.float32)
        step = self.max_block_size
        for frame in range(0, frames, step):
            block_frames = min(step, frames - frame)
            start = frame * ch
            end = start + block_frames * ch
            in_block = in_buf[start:end]
            out_block = out_buf[start:end]
            c_in = (ctypes.c_float * (block_frames * ch)).from_buffer(in_block)
            c_out = (ctypes.c_float * (block_frames * ch)).from_buffer(out_block)
            c_carrier = (
                None
                if carrier_buf is None
                else (ctypes.c_float * block_frames).from_buffer(
                    carrier_buf[frame : frame + block_frames]
                )
            )
            rc = self._lib.sonare_realtime_voice_changer_process_interleaved(
                self._require_handle(),
                c_in,
                c_carrier,
                c_out,
                _to_c_size_t(block_frames, "block_frames"),
                _to_c_int(ch, "ch"),
            )
            _check_realtime(rc)
        return out_buf

    @_entry("RealtimeVoiceChanger.process_planar")
    def process_planar(
        self,
        channels: PlanarChannels,
        carrier: Sequence[float] | list[float] | np.ndarray | None = None,
    ) -> list[np.ndarray]:
        """Process planar audio (one buffer per channel, 1 or 2, equal lengths) block-by-block.

        One channel takes the mono path and two the stereo path; the inputs are not
        modified, and the processed planes come back as ``float32`` ndarrays in the
        input order. Another channel count or unequal lengths raise
        ``SonareValueError``; two channels on a changer prepared for one raise
        ``SonareError`` (invalid state). ``carrier`` is a mono external vocoder
        carrier with one sample per frame, shared by every channel; a different
        length raises ``SonareValueError``.
        """
        return self._process_planar(channels, carrier)

    def _process_planar(
        self,
        channels: PlanarChannels,
        carrier: Sequence[float] | list[float] | np.ndarray | None,
    ) -> list[np.ndarray]:
        if isinstance(channels, np.ndarray) and channels.ndim != 2:
            raise _not_planar_channels("channels")
        if not isinstance(channels, (Sequence, np.ndarray)):
            raise _not_planar_channels("channels")
        # Private copies the C call may write into; the caller's buffers stay untouched.
        # Zero frames is a no-op here, as on the mono and interleaved paths.
        planes = [
            np.array(_as_float32_buffer(plane, arg_name="channels"), dtype=np.float32, copy=True)
            for plane in channels
        ]
        if len(planes) not in (1, 2):
            raise _refuse("channels must hold 1 or 2 planes")
        total = int(planes[0].shape[0])
        if any(int(plane.shape[0]) != total for plane in planes):
            raise _refuse("all channels must have the same length")
        carrier_buf = None if carrier is None else _as_float32_buffer(carrier, arg_name="carrier")
        if carrier_buf is not None and int(carrier_buf.shape[0]) != total:
            raise _refuse("carrier must have the same length as the channels")
        if len(planes) == 1:
            return [self._process_mono(planes[0], carrier_buf)]
        out_left, out_right = planes
        step = self.max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            c_left = (ctypes.c_float * length).from_buffer(out_left[pos : pos + length])
            c_right = (ctypes.c_float * length).from_buffer(out_right[pos : pos + length])
            c_carrier = (
                None
                if carrier_buf is None
                else (ctypes.c_float * length).from_buffer(carrier_buf[pos : pos + length])
            )
            rc = self._lib.sonare_realtime_voice_changer_process_planar_stereo(
                self._require_handle(),
                c_left,
                c_right,
                c_carrier,
                _to_c_size_t(length, "length"),
            )
            _check_realtime(rc)
        return planes

    @_entry("RealtimeVoiceChanger.note_on")
    def note_on(self, note: int, velocity: float = 1.0) -> None:
        """Hold MIDI ``note`` from the start of the next processed block.

        Held notes become the voices' pitch targets, the newest leading, up to
        ``1 + harmony_voices`` of them. ``note`` outside [0, 127] or
        ``velocity`` outside (0, 1] raises ``SonareValueError``; the velocity is
        validated only and does not change the sound. A grain-engine
        handle, or a full event queue (the event is dropped), raises
        ``SonareError`` (invalid state).
        """
        rc = self._lib.sonare_realtime_voice_changer_note_on(
            self._require_handle(),
            _to_c_int(note, "note"),
            _to_c_float(velocity, "velocity"),
        )
        _check_realtime(rc)

    @_entry("RealtimeVoiceChanger.note_off")
    def note_off(self, note: int) -> None:
        """Release MIDI ``note``; releasing a note that is not held does nothing."""
        rc = self._lib.sonare_realtime_voice_changer_note_off(
            self._require_handle(), _to_c_int(note, "note")
        )
        _check_realtime(rc)

    @_entry("RealtimeVoiceChanger.all_notes_off")
    def all_notes_off(self) -> None:
        """Release every held note; succeeds even when the event queue is full."""
        _check_realtime(
            self._lib.sonare_realtime_voice_changer_all_notes_off(self._require_handle())
        )


@_entry("realtime_voice_changer_render")
def realtime_voice_changer_render(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    config: VoicePresetId
    | RealtimeVoiceChangerPreset
    | RealtimeVoiceChangerConfig
    | None = "neutralMonitor",
    num_channels: int = 1,
) -> np.ndarray:
    """Apply the realtime voice changer to a buffer offline.

    ``config`` takes the same values as :class:`RealtimeVoiceChanger`: a preset id,
    a configuration document mapping or a :class:`RealtimeVoiceChangerConfig`
    record; ``None`` selects the neutral monitor.

    When ``num_channels`` is 2 the input must be interleaved (LRLR…) and the
    output is returned interleaved in the same layout.

    The chain's processing latency (the engine's own: retune grain plus warp for
    the grain engine, ``ceil(sample_rate / f0_min)`` for psola; plus the
    ISP-limiter lookahead when enabled) is compensated internally — a silent tail
    is flushed and the leading pre-roll is dropped — so the result is time-aligned
    with the input rather than shifted by the chain latency.

    Returns a ``numpy.ndarray`` of dtype ``float32`` (changed from
    ``list[float]`` in the prior implementation). ``len(result)`` still
    equals ``len(samples)`` so existing length-based assertions keep
    working.
    """
    num_channels = _narrow_int(num_channels, "num_channels", 1, 2)
    in_buf = _validate_samples(samples)
    if int(in_buf.shape[0]) % num_channels != 0:
        raise _refuse("samples length must be a multiple of num_channels")
    lib = _get_lib()
    c_array, length = _to_c_float_array(in_buf)
    if config is None:
        pod = None
    elif isinstance(config, RealtimeVoiceChangerConfig):
        pod = config._to_pod()
    else:
        pod = _voice_config_to_pod(lib, config)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_realtime_voice_changer_render(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                None if pod is None else ctypes.byref(pod),
                _to_c_int(num_channels, "num_channels"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@_entry("realtime_voice_changer_preset_names")
def realtime_voice_changer_preset_names() -> list[str]:
    """Return the names of the built-in realtime voice-changer presets."""
    return _joined_names(
        _get_lib().sonare_realtime_voice_changer_preset_names(),
        "realtime voice changer preset names",
    )


@_entry("realtime_voice_changer_preset_json")
def realtime_voice_changer_preset_json(name: str) -> str:
    """Return the JSON text of the named preset. Raises ``SonareError`` for an unknown name."""
    lib = _get_lib()
    out = ctypes.c_char_p()
    rc = lib.sonare_realtime_voice_changer_preset_json(_utf8_arg(name, "name"), ctypes.byref(out))
    _check(rc)
    try:
        return ctypes.string_at(out).decode("utf-8")
    finally:
        if out:
            lib.sonare_free_string(out)


@_entry("validate_realtime_voice_changer_preset_json")
def validate_realtime_voice_changer_preset_json(json_text: str) -> dict[str, object]:
    """Validate a preset JSON text; return the verdict as a dict rather than raising."""
    lib = _get_lib()
    normalized = ctypes.c_char_p()
    error = ctypes.c_char_p()
    rc = lib.sonare_realtime_voice_changer_validate_preset_json(
        _utf8_arg(json_text, "json_text"), ctypes.byref(normalized), ctypes.byref(error)
    )
    if rc == SONARE_OK:
        try:
            return {"ok": True, "normalizedJson": ctypes.string_at(normalized).decode("utf-8")}
        finally:
            if normalized:
                lib.sonare_free_string(normalized)
    try:
        message = ctypes.string_at(error).decode("utf-8") if error else "invalid preset JSON"
        return {"ok": False, "error": message}
    finally:
        if error:
            lib.sonare_free_string(error)


@_entry("voice_character_preset_id")
def voice_character_preset_id(ordinal: int) -> str | None:
    """Map a voice-character preset enum ordinal to its canonical id string.

    Args:
        ordinal: A ``SONARE_VC_PRESET_*`` enum value.

    Returns:
        The canonical preset id (e.g. ``"brightIdol"``) or ``None`` when the
        ordinal is out of range.
    """
    raw = _get_lib().sonare_voice_character_preset_id(_to_c_int(ordinal, "ordinal"))
    if not raw:
        return None
    return cast(str, raw.decode("utf-8"))


@functools.lru_cache(maxsize=1)
def _neutral_monitor_defaults() -> dict[str, Any]:
    """Field values of the built-in ``neutralMonitor`` preset.

    These are the single source of truth for the default-constructed
    :class:`RealtimeVoiceChangerConfig`, so that ``RealtimeVoiceChangerConfig()``
    followed by :meth:`RealtimeVoiceChanger.set_config` reproduces the same
    neutral voice as the C++ ``realtime_voice_changer_preset(NeutralMonitor)``
    idiom instead of a bypassed-gate / flat-EQ configuration.
    """
    pod = SonareRealtimeVoiceChangerConfig()
    _check(
        _get_lib().sonare_realtime_voice_changer_preset_config(
            _to_c_int(SONARE_VC_PRESET_NEUTRAL_MONITOR, "SONARE_VC_PRESET_NEUTRAL_MONITOR"),
            ctypes.byref(pod),
        )
    )
    values: dict[str, Any] = {}
    for name, *_ in pod._fields_:
        value = getattr(pod, name)
        if name == "harmony_steps":
            value = tuple(int(step) for step in value)
        elif name in _ENUM_FIELD_NAMES:
            value = _enum_name(name, value)
        elif name == "retune_grain_size_samples":
            value = value or None
        elif name in _BOOL_FIELD_NAMES:
            value = bool(value)
        values[name] = value
    return values


def _neutral_default(name: str) -> Callable[[], Any]:
    """Return a zero-arg factory yielding the preset default for ``name``.

    The return type is ``Any`` because the same helper seeds both float and int
    fields; the concrete POD value carries the correct numeric type.
    """
    return lambda: _neutral_monitor_defaults()[name]


# The grain lengths the retune engine runs at as given, rather than rounding.
_GRAIN_SIZE_MIN = 4
_GRAIN_SIZE_MAX = 8192
_GRAIN_SIZE_STEP = 4


def _grain_size_request(grain_size_samples: int | None, name: str) -> int:
    """Map a grain size to the C argument; ``None`` is automatic (C ``0``).

    An explicit grain outside the engine's domain is refused, never rounded to
    the nearest one it can run.
    """
    if grain_size_samples is None:
        return 0
    if not _is_number(grain_size_samples):
        raise _refuse_type(f"{name} must be a number")
    if grain_size_samples == 0:
        raise _refuse(f"{name} must not be 0; omit it for the automatic grain")
    # Narrowed rather than coerced: int() would take 4.5 as the grain 4.
    try:
        value = operator.index(grain_size_samples)
    except TypeError:
        value = None
    if (
        value is None
        or not _GRAIN_SIZE_MIN <= value <= _GRAIN_SIZE_MAX
        or value % _GRAIN_SIZE_STEP != 0
    ):
        raise _refuse(
            f"{name} must be a multiple of {_GRAIN_SIZE_STEP} "
            f"in [{_GRAIN_SIZE_MIN}, {_GRAIN_SIZE_MAX}]"
        )
    return value


@dataclasses.dataclass(frozen=True)
class RealtimeVoiceChangerConfig:
    """Flat mirror of :class:`SonareRealtimeVoiceChangerConfig`.

    Field order matches the C POD struct in ``sonare_c.h``. The default value of
    every field is seeded from the built-in ``neutralMonitor`` preset (fetched
    from the library once and cached), so a default-constructed instance is a
    usable neutral voice rather than a bypassed-gate / flat-EQ configuration.
    An out-of-range entry is refused by
    :meth:`RealtimeVoiceChanger.set_config` with a ``SonareValueError`` and
    at creation with a coded ``SonareError`` (``INVALID_FORMAT``, the creation
    document being a JSON entry point).

    ``formant_mode`` is ``"relative"`` (default) or ``"absolute"`` and is fixed
    when the changer is created: :meth:`RealtimeVoiceChanger.set_config`
    with a different mode raises. In absolute mode ``formant_factor`` is the
    formant shift relative to the input, ``formant_amount`` is ignored, one
    analysis frame of latency is added, and a configuration whose warp
    ``formant_factor / 2**(retune_pitch_semitones / 12)`` leaves [0.55, 1.65] is
    refused, naming the reachable ``formant_factor`` range, the same way.

    The enum-valued fields hold snake_case spellings: ``voice_engine``
    (``"grain"`` or ``"psola"``, fixed at creation), ``pitch_correction_mode``,
    ``pitch_correction_scale``, ``vocoder_carrier_source``,
    ``vocoder_carrier_wave``, ``character_type``, ``reverb_variation``,
    ``reverb_variation_division`` and ``midi_release``. ``harmony_steps`` holds
    exactly three scale-degree offsets. An unknown spelling raises
    ``SonareValueError``.

    The gate, compressor and limiter ``*_attack_ms`` / ``*_release_ms`` times are
    one time constant: about 63% of the way at the set time.
    """

    input_gain_db: float = dataclasses.field(default_factory=_neutral_default("input_gain_db"))
    """Input gain in dB, [-24, 24]."""
    output_gain_db: float = dataclasses.field(default_factory=_neutral_default("output_gain_db"))
    """Output gain in dB, [-36, 12]."""
    wet_mix: float = dataclasses.field(default_factory=_neutral_default("wet_mix"))
    """Dry/wet mix in [0, 1]; 1 is fully processed."""
    retune_pitch_semitones: float = dataclasses.field(
        default_factory=_neutral_default("retune_pitch_semitones")
    )
    """Retune pitch shift in semitones; outside finite [-24, 24] it is refused."""
    retune_mix: float = dataclasses.field(default_factory=_neutral_default("retune_mix"))
    """Retune wet amount in [0, 1]."""
    retune_grain_size_samples: int | None = dataclasses.field(
        default_factory=_neutral_default("retune_grain_size_samples")
    )
    """Retune grain length in samples as configured; ``None`` derives it from the sample rate.

    An explicit grain must be a multiple of 4 in [4, 8192] and ``0`` is refused;
    the grain in use is resolved at ``prepare``.
    """
    formant_factor: float = dataclasses.field(default_factory=_neutral_default("formant_factor"))
    """Formant scale factor, [0.55, 1.65]; 1 leaves the formants alone. See ``formant_mode``."""
    formant_amount: float = dataclasses.field(default_factory=_neutral_default("formant_amount"))
    """Strength of the formant displacement in [0, 1]; ignored in absolute mode."""
    formant_body: float = dataclasses.field(default_factory=_neutral_default("formant_body"))
    """Formant body amount in [-1, 1]."""
    formant_brightness: float = dataclasses.field(
        default_factory=_neutral_default("formant_brightness")
    )
    """Formant brightness amount in [-1, 1]."""
    formant_nasal: float = dataclasses.field(default_factory=_neutral_default("formant_nasal"))
    """Formant nasal amount in [-1, 1]."""
    formant_mode: FormantMode = dataclasses.field(default_factory=_neutral_default("formant_mode"))
    """How ``formant_factor`` acts: ``"relative"`` or ``"absolute"``; fixed at creation."""
    eq_high_pass_hz: float = dataclasses.field(default_factory=_neutral_default("eq_high_pass_hz"))
    """High-pass corner in Hz, [20, 300]."""
    eq_body_db: float = dataclasses.field(default_factory=_neutral_default("eq_body_db"))
    """Body band gain in dB, [-12, 12]."""
    eq_presence_db: float = dataclasses.field(default_factory=_neutral_default("eq_presence_db"))
    """Presence band gain in dB, [-12, 12]."""
    eq_air_db: float = dataclasses.field(default_factory=_neutral_default("eq_air_db"))
    """Air band gain in dB, [-12, 12]."""
    gate_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("gate_threshold_db")
    )
    """Gate threshold in dB, [-90, -12]."""
    gate_attack_ms: float = dataclasses.field(default_factory=_neutral_default("gate_attack_ms"))
    """Gate attack in ms, one time constant."""
    gate_release_ms: float = dataclasses.field(default_factory=_neutral_default("gate_release_ms"))
    """Gate release in ms, one time constant."""
    gate_range_db: float = dataclasses.field(default_factory=_neutral_default("gate_range_db"))
    """Gate attenuation depth in dB, [0, 80]."""
    compressor_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("compressor_threshold_db")
    )
    """Compressor threshold in dB, [-60, 0]."""
    compressor_ratio: float = dataclasses.field(
        default_factory=_neutral_default("compressor_ratio")
    )
    """Compression ratio, [1, 20]."""
    compressor_attack_ms: float = dataclasses.field(
        default_factory=_neutral_default("compressor_attack_ms")
    )
    """Compressor attack in ms, one time constant."""
    compressor_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("compressor_release_ms")
    )
    """Compressor release in ms, one time constant."""
    compressor_makeup_gain_db: float = dataclasses.field(
        default_factory=_neutral_default("compressor_makeup_gain_db")
    )
    """Compressor makeup gain in dB, [-12, 12]."""
    deesser_frequency_hz: float = dataclasses.field(
        default_factory=_neutral_default("deesser_frequency_hz")
    )
    """De-esser centre frequency in Hz, [3000, 12000]."""
    deesser_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("deesser_threshold_db")
    )
    """De-esser threshold in dB, [-60, -6]."""
    deesser_ratio: float = dataclasses.field(default_factory=_neutral_default("deesser_ratio"))
    """De-esser ratio, [1, 20]."""
    deesser_range_db: float = dataclasses.field(
        default_factory=_neutral_default("deesser_range_db")
    )
    """Maximum de-esser attenuation in dB, [0, 24]."""
    reverb_mix: float = dataclasses.field(default_factory=_neutral_default("reverb_mix"))
    """Space-stage wet level in [0, 0.45]; shared by every ``reverb_variation``."""
    reverb_time_ms: float = dataclasses.field(default_factory=_neutral_default("reverb_time_ms"))
    """Reverb decay time in ms, [40, 1800]."""
    reverb_damping: float = dataclasses.field(default_factory=_neutral_default("reverb_damping"))
    """Reverb high-frequency damping in [0, 1]."""
    reverb_seed: int = dataclasses.field(default_factory=_neutral_default("reverb_seed"))
    """Integer seed of the reverb's random generator (32-bit signed)."""
    limiter_ceiling_db: float = dataclasses.field(
        default_factory=_neutral_default("limiter_ceiling_db")
    )
    """Limiter ceiling in dB, [-12, -1]."""
    limiter_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("limiter_release_ms")
    )
    """Limiter release in ms, one time constant."""
    # Appended in ABI version 2 (kept at the END to match the C POD layout).
    limiter_enable_isp_limiter: bool = dataclasses.field(
        default_factory=_neutral_default("limiter_enable_isp_limiter")
    )
    """``True`` enables the 4x-oversampled inter-sample-peak (true-peak) limiter."""
    limiter_isp_ceiling_dbtp: float = dataclasses.field(
        default_factory=_neutral_default("limiter_isp_ceiling_dbtp")
    )
    """True-peak ceiling in dBTP of the inter-sample-peak limiter."""
    voice_engine: VoiceEngine = dataclasses.field(default_factory=_neutral_default("voice_engine"))
    """Pitch engine, ``"grain"`` (f0-less SOLA retune) or ``"psola"`` (tracks f0); fixed at
    creation."""
    voice_engine_f0_min_hz: float = dataclasses.field(
        default_factory=_neutral_default("voice_engine_f0_min_hz")
    )
    """Lowest tracked f0 in Hz, [50, 200]; sets the psola latency. Fixed at creation."""
    pitch_correction_mode: PitchCorrectionMode = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_mode")
    )
    """Pitch correction: ``"off"``, ``"auto"`` (scale snap) or ``"robot"`` (one pitch class)."""
    pitch_correction_amount: float = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_amount")
    )
    """Correction strength in [0, 1]."""
    pitch_correction_speed_ms: float = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_speed_ms")
    )
    """Correction speed in ms, [0, 400]."""
    pitch_correction_key_root: int = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_key_root")
    )
    """Scale root, 0 = C to 11 = B; unused for a chromatic scale."""
    pitch_correction_scale: ScaleKind = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_scale")
    )
    """Scale the correction snaps to: ``"major"``, ``"naturalMinor"`` or ``"chromatic"``."""
    pitch_correction_robot_octave: int = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_robot_octave")
    )
    """Octave offset of the robot mode, [-1, 1]."""
    harmony_voices: int = dataclasses.field(default_factory=_neutral_default("harmony_voices"))
    """Harmony voices beside the lead, [0, 3]."""
    harmony_steps: tuple[int, int, int] = dataclasses.field(
        default_factory=_neutral_default("harmony_steps")
    )
    """Scale degrees of the three harmony voices from the lead, each in [-7, 7]; 0 is unison."""
    harmony_level: float = dataclasses.field(default_factory=_neutral_default("harmony_level"))
    """Level of the harmony voices in [0, 1]."""
    harmony_lead_level: float = dataclasses.field(
        default_factory=_neutral_default("harmony_lead_level")
    )
    """Level of the lead voice in [0, 1]."""
    harmony_detune_cents: float = dataclasses.field(
        default_factory=_neutral_default("harmony_detune_cents")
    )
    """Detune of the harmony voices in cents, [0, 30]."""
    harmony_delay_ms: float = dataclasses.field(
        default_factory=_neutral_default("harmony_delay_ms")
    )
    """Harmony delay in ms, [0, 40]."""
    vocoder_enabled: bool = dataclasses.field(default_factory=_neutral_default("vocoder_enabled"))
    """``True`` enables the channel vocoder."""
    vocoder_bands: int = dataclasses.field(default_factory=_neutral_default("vocoder_bands"))
    """Vocoder band count, [8, 32]. Fixed at creation."""
    vocoder_attack_ms: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_attack_ms")
    )
    """Vocoder envelope attack in ms, [0.5, 50]."""
    vocoder_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_release_ms")
    )
    """Vocoder envelope release in ms, [2, 300]."""
    vocoder_sibilance: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_sibilance")
    )
    """Vocoder sibilance amount in [0, 1]."""
    vocoder_carrier_source: VocoderCarrierSource = dataclasses.field(
        default_factory=_neutral_default("vocoder_carrier_source")
    )
    """Carrier source, ``"internal"`` or ``"external"``."""
    vocoder_carrier_wave: VocoderCarrierWave = dataclasses.field(
        default_factory=_neutral_default("vocoder_carrier_wave")
    )
    """Internal carrier waveform: ``"saw"``, ``"pulse"`` or ``"ensemble"``."""
    vocoder_ensemble_detune_cents: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_ensemble_detune_cents")
    )
    """Detune of the ensemble carrier in cents, [0, 30]."""
    vocoder_level: float = dataclasses.field(default_factory=_neutral_default("vocoder_level"))
    """Vocoder output level in [0, 1]."""
    character_type: CharacterType = dataclasses.field(
        default_factory=_neutral_default("character_type")
    )
    """Character stage type: ``"none"``, ``"megaphone"``, ``"radio"``, ``"bbdChorus"`` or
    ``"strobo"``."""
    character_amount: float = dataclasses.field(
        default_factory=_neutral_default("character_amount")
    )
    """Character stage amount in [0, 1]."""
    character_tone: float = dataclasses.field(default_factory=_neutral_default("character_tone"))
    """Character stage tone in [-1, 1]."""
    reverb_variation: ReverbVariation = dataclasses.field(
        default_factory=_neutral_default("reverb_variation")
    )
    """Space stage variation: ``"reverb"``, ``"echo"``, ``"tempoDelay"`` or ``"dubEcho"``."""
    reverb_variation_delay_ms: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_delay_ms")
    )
    """Echo delay in ms, [1, 3000]."""
    reverb_variation_feedback: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_feedback")
    )
    """Echo feedback in [0, 0.95]."""
    reverb_variation_tempo_bpm: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_tempo_bpm")
    )
    """Tempo of the tempo delay in BPM, [30, 300]."""
    reverb_variation_division: NoteDivision = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_division")
    )
    """Note length of the tempo delay: ``"quarter"``, ``"eighth"``, ``"dottedEighth"`` or
    ``"sixteenth"``."""
    midi_release: MidiRelease = dataclasses.field(default_factory=_neutral_default("midi_release"))
    """After every held note is released: ``"hold"`` keeps the last note, ``"input"`` follows the
    input."""

    @classmethod
    def _from_pod(cls, pod: SonareRealtimeVoiceChangerConfig) -> RealtimeVoiceChangerConfig:
        """Copy field-for-field out of a ctypes struct."""
        values: dict[str, Any] = {}
        for name, *_ in pod._fields_:
            if name == "struct_size":
                continue
            value = getattr(pod, name)
            if name == "harmony_steps":
                value = tuple(int(step) for step in value)
            elif name in _ENUM_FIELD_NAMES:
                value = _enum_name(name, value)
            elif name == "retune_grain_size_samples":
                value = value or None
            elif name in _BOOL_FIELD_NAMES:
                value = bool(value)
            values[name] = value
        return cls(**values)

    def _to_pod(self) -> SonareRealtimeVoiceChangerConfig:
        """Copy field-for-field into a freshly allocated ctypes struct."""
        out = SonareRealtimeVoiceChangerConfig()
        for name, *_ in out._fields_:
            if name == "struct_size":
                continue
            value = getattr(self, name)
            if name == "harmony_steps":
                # An array element bypasses the struct's field guard, so narrow here.
                if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
                    raise _refuse_type("harmony_steps must be a sequence of 3 integers")
                if len(value) != _HARMONY_STEP_COUNT:
                    raise _refuse(f"harmony_steps must hold {_HARMONY_STEP_COUNT} integers")
                for index, step in enumerate(value):
                    out.harmony_steps[index] = _narrow_int(
                        step, f"harmony_steps[{index}]", _C_INT_MIN, _C_INT_MAX
                    )
            elif name in _ENUM_FIELD_NAMES:
                setattr(out, name, _enum_ordinal(name, value))
            elif name == "retune_grain_size_samples":
                out.retune_grain_size_samples = _grain_size_request(value, name)
            elif name in _BOOL_FIELD_NAMES:
                setattr(out, name, _bool_flag(value, name))
            else:
                setattr(out, name, value)
        return out


def _resolve_preset_ordinal(preset: VoicePresetId, key: str) -> int:
    if isinstance(preset, str):
        _utf8_arg(preset, "preset")
    return _token_ordinal(preset, _VOICE_PRESET_IDS, key)


@_entry("realtime_voice_changer_preset_config")
def realtime_voice_changer_preset_config(preset: VoicePresetId) -> RealtimeVoiceChangerConfig:
    """Return the canonical (normalised) config for a built-in preset.

    Skips the JSON round-trip that ``realtime_voice_changer_preset_json``
    incurs.

    Raises:
        TypeError: If ``preset`` is not a string.
        SonareValueError: If ``preset`` is not a :data:`VoicePresetId` token.
    """
    lib = _get_lib()
    pod = SonareRealtimeVoiceChangerConfig()
    rc = lib.sonare_realtime_voice_changer_preset_config(
        _to_c_int(_resolve_preset_ordinal(preset, "preset"), "preset"), ctypes.byref(pod)
    )
    _check(rc)
    return RealtimeVoiceChangerConfig._from_pod(pod)


class StreamingRetune:
    """Block-by-block mono pitch shifter (grain overlap-add).

    The streaming counterpart of the offline ``pitch_shift``: state persists
    across :meth:`process_mono` calls, so consecutive blocks of a live stream
    join without a seam. Call :meth:`prepare` before processing.

    ``pitch_semitones`` must be finite and in [-24, 24] and ``mix`` finite and in
    [0, 1]; a value outside either raises :class:`SonareValueError`, and a
    refused :meth:`set_config` keeps the previous controls.
    A block holding a non-finite sample is rejected rather than zeroed, since it
    would otherwise enter the grain history and persist into every later block.

    ``grain_size_samples=None`` derives the grain from the sample rate at
    :meth:`prepare`; an explicit value must be a multiple of 4 in [4, 8192], and
    ``0`` is refused rather than read as automatic.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    @_entry("StreamingRetune")
    def __init__(
        self,
        *,
        pitch_semitones: float = 0.0,
        mix: float = 1.0,
        grain_size_samples: int | None = None,
    ) -> None:
        # Set first so the non-finite rejection below leaves a valid attribute
        # for __del__/close() instead of raising AttributeError over it.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        requested_grain_size = _grain_size_request(grain_size_samples, "grain_size_samples")
        handle = ctypes.c_void_p()
        rc = self._lib.sonare_streaming_retune_create(
            _domain_float(pitch_semitones, "pitch_semitones"),
            _domain_float(mix, "mix"),
            _to_c_int(requested_grain_size, "grain_size_samples"),
            ctypes.byref(handle),
        )
        _check(rc)
        self._handle = handle

    @_entry("StreamingRetune.close")
    def close(self) -> None:
        """Release the native handle. Idempotent, so a double close is a no-op."""
        if self._handle:
            self._lib.sonare_streaming_retune_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> StreamingRetune:
        """Return self so the object works as a context manager."""
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        """Close on leaving the ``with`` block; never suppresses an exception."""
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "StreamingRetune")

    @_entry("StreamingRetune.prepare")
    def prepare(self, sample_rate: int, max_block_size: int) -> None:
        """Allocate native state for an integer ``sample_rate`` in Hz and resolve the grain size."""
        _check(
            self._lib.sonare_streaming_retune_prepare(
                self._require_handle(),
                _to_c_int(
                    _narrow_int(sample_rate, "sample_rate", _C_INT_MIN, _C_INT_MAX), "sample_rate"
                ),
                _to_c_int(max_block_size, "max_block_size"),
            )
        )

    @_entry("StreamingRetune.reset")
    def reset(self) -> None:
        """Clear grain / overlap-add state without changing the config."""
        _check(self._lib.sonare_streaming_retune_reset(self._require_handle()))

    @_entry("StreamingRetune.set_config")
    def set_config(
        self,
        *,
        pitch_semitones: float | None = None,
        mix: float | None = None,
        grain_size_samples: int | None = None,
    ) -> None:
        """Update the live controls; omitted keywords keep their current value.

        ``pitch_semitones`` must be finite and in [-24, 24], ``mix`` finite and in
        [0, 1], and an explicit ``grain_size_samples`` a multiple of 4 in [4, 8192]
        (``0`` is refused); a refused update keeps the previous controls.
        ``grain_size_samples`` is structural: :meth:`config` reports it at once, and
        the grain in use (:attr:`grain_size_samples`) follows at the next
        :meth:`prepare`. An omitted ``grain_size_samples`` keeps the configured
        value, including automatic, so re-preparing at another sample rate
        re-derives the grain.
        """
        current_pitch, current_mix, current_grain = self._read_config()
        requested_grain_size = (
            current_grain
            if grain_size_samples is None
            else _grain_size_request(grain_size_samples, "grain_size_samples")
        )
        _check(
            self._lib.sonare_streaming_retune_set_config(
                self._require_handle(),
                _domain_float(
                    current_pitch if pitch_semitones is None else pitch_semitones,
                    "pitch_semitones",
                ),
                _domain_float(current_mix if mix is None else mix, "mix"),
                _to_c_int(requested_grain_size, "grain_size_samples"),
            )
        )

    def _read_config(self) -> tuple[float, float, int]:
        pitch_semitones = ctypes.c_float()
        mix = ctypes.c_float()
        grain_size_samples = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_config(
                self._require_handle(),
                ctypes.byref(pitch_semitones),
                ctypes.byref(mix),
                ctypes.byref(grain_size_samples),
            )
        )
        return float(pitch_semitones.value), float(mix.value), int(grain_size_samples.value)

    @_entry("StreamingRetune.config")
    def config(self) -> dict[str, float | int | None]:
        """The controls as configured; ``grain_size_samples`` is ``None`` when automatic.

        The grain in use is :attr:`grain_size_samples`.
        """
        pitch_semitones, mix, grain_size_samples = self._read_config()
        return {
            "pitch_semitones": pitch_semitones,
            "mix": mix,
            "grain_size_samples": grain_size_samples or None,
        }

    @property
    def grain_size_samples(self) -> int:
        """Resolved grain length in samples; 0 before :meth:`prepare`."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_grain_size_samples(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    @property
    def latency_samples(self) -> int:
        """Fixed overlap-add latency in samples (one grain); 0 before prepare."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_latency_samples(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    @_entry("StreamingRetune.process_mono")
    def process_mono(self, samples: Sequence[float] | list[float] | np.ndarray) -> np.ndarray:
        """Process one mono block, returning the shifted samples (same length)."""
        # The C entry point works in place and _as_float32_buffer is zero-copy
        # for a contiguous float32 ndarray, so copy before handing it over: the
        # Node and WASM surfaces leave the caller's samples untouched and this
        # one would otherwise overwrite them.
        buffer = np.array(_as_float32_buffer(samples), dtype=np.float32, copy=True, order="C")
        _check(
            self._lib.sonare_streaming_retune_process_mono(
                self._require_handle(),
                buffer.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                ctypes.c_size_t(len(buffer)),
            )
        )
        return buffer
