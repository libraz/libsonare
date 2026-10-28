"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import functools
import json
from collections.abc import Callable, Mapping, Sequence
from typing import Any, cast

import numpy as np

from ._errors import _not_supported, _require_open
from ._ffi import (
    SONARE_OK,
    SONARE_VC_PRESET_BRIGHT_IDOL,
    SONARE_VC_PRESET_CHOIR_HARMONY,
    SONARE_VC_PRESET_DARK_VILLAIN,
    SONARE_VC_PRESET_DEEP_NARRATOR,
    SONARE_VC_PRESET_HARD_TUNE,
    SONARE_VC_PRESET_MEGAPHONE,
    SONARE_VC_PRESET_NEUTRAL_MONITOR,
    SONARE_VC_PRESET_RADIO_VOICE,
    SONARE_VC_PRESET_ROBOT_MASCOT,
    SONARE_VC_PRESET_ROBOT_MONOTONE,
    SONARE_VC_PRESET_SOFT_WHISPER,
    SONARE_VC_PRESET_VOCODER_STRINGS,
    SonareRealtimeVoiceChangerConfig,
    SonareVoiceChangeConfig,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    ErrorCode,
    SonareValueError,
    _as_float32_buffer,
    _check,
    _check_realtime,
    _float_array_result,
    _from_c_float_array,
    _get_lib,
    _last_error,
    _narrow_int,
    _out_float_array,
    _to_c_double,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _utf8_arg,
    _validate_samples,
)

_VOICE_CHANGE_STRUCT_VERSION = 1
_FORMANT_MODES = {"relative": 0, "absolute": 1}
_REALTIME_CONFIG_STRUCT_VERSION = 2

# Enum-valued POD fields: snake_case spellings in the order of the C ordinals.
_ENUM_FIELD_NAMES: dict[str, tuple[str, ...]] = {
    "formant_mode": ("relative", "absolute"),
    "voice_engine_engine": ("grain", "psola"),
    "pitch_correction_mode": ("off", "auto", "robot"),
    "pitch_correction_scale": ("major", "natural_minor", "chromatic"),
    "vocoder_carrier_source": ("internal", "external"),
    "vocoder_carrier_wave": ("saw", "pulse", "ensemble"),
    "character_type": ("none", "megaphone", "radio", "bbd_chorus", "strobo"),
    "reverb_variation_variation": ("reverb", "echo", "tempo_delay", "dub_echo"),
    "reverb_variation_division": ("quarter", "eighth", "dotted_eighth", "sixteenth"),
    "midi_release": ("hold", "input"),
}
_HARMONY_STEP_COUNT = 3


def _enum_ordinal(field: str, value: object) -> int:
    """Map an enum spelling to its C ordinal, refusing an unknown spelling."""
    names = _ENUM_FIELD_NAMES[field]
    if not isinstance(value, str) or value not in names:
        expected = ", ".join(repr(n) for n in names)
        raise SonareValueError(f"{field} must be one of {expected}, got {value!r}")
    return names.index(value)


def _enum_name(field: str, ordinal: int) -> str:
    """Map a C ordinal read from the library to its spelling."""
    names = _ENUM_FIELD_NAMES[field]
    if not 0 <= int(ordinal) < len(names):
        raise SonareValueError(f"unknown {field} ordinal: {ordinal}")
    return names[int(ordinal)]


def voice_change(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    pitch_semitones: float = 0.0,
    formant_factor: float = 1.0,
    formant_mode: str = "relative",
) -> list[float]:
    """Apply a voice-change effect with independent pitch and formant control.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
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
        List of voice-changed samples.

    Raises:
        SonareValueError: ``formant_mode`` is not ``"relative"`` or
            ``"absolute"``, a relative ``formant_factor`` is non-finite or
            outside [0.55, 1.65], or an absolute request needs a warp outside
            [0.55, 1.65]. The message names the reachable ``formant_factor``
            range; the request is refused, not clamped.
    """
    _validate_samples("voice_change", samples, validate=True)
    if not isinstance(formant_mode, str) or formant_mode not in _FORMANT_MODES:
        raise SonareValueError(
            f"voice_change: formant_mode must be 'relative' or 'absolute', got {formant_mode!r}"
        )
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareVoiceChangeConfig(
        struct_version=_VOICE_CHANGE_STRUCT_VERSION,
        pitch_semitones=_to_c_float(pitch_semitones, "pitch_semitones"),
        formant_factor=_to_c_float(formant_factor, "formant_factor"),
        formant_mode=_FORMANT_MODES[formant_mode],
    )
    with _out_float_array(lib) as (out, out_length):
        rc = lib.sonare_voice_change_ex(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(config),
            ctypes.byref(out),
            ctypes.byref(out_length),
        )
        # A refused factor is an argument refusal in either formant mode.
        _check(rc)
        return _float_array_result(out, out_length.value)


def _voice_config_to_json(preset: str | Mapping[str, object]) -> bytes:
    if isinstance(preset, str):
        return _utf8_arg(preset, "preset")
    return _utf8_arg(json.dumps(preset, separators=(",", ":")), "preset")


class RealtimeVoiceChanger:
    """Streaming realtime voice changer backed by the libsonare C API."""

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    def __init__(
        self,
        sample_rate: int,
        preset: str | Mapping[str, object] = "neutral-monitor",
        *,
        max_block_size: int = 128,
        channels: int = 1,
    ) -> None:
        # Set first so a failed loader or create leaves a valid attribute for
        # __del__/close() instead of raising AttributeError.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        self._max_block_size = int(max_block_size)
        self._channels = int(channels)
        rc = self._lib.sonare_realtime_voice_changer_create_json(
            _voice_config_to_json(preset),
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_int(max_block_size, "max_block_size"),
            _to_c_int(channels, "channels"),
            ctypes.byref(self._handle),
        )
        _check(rc)

    def close(self) -> None:
        if self._handle:
            self._lib.sonare_realtime_voice_changer_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> RealtimeVoiceChanger:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "RealtimeVoiceChanger")

    def reset(self) -> None:
        rc = self._lib.sonare_realtime_voice_changer_reset(self._require_handle())
        _check(rc)

    def set_config(self, preset: str | Mapping[str, object]) -> None:
        rc = self._lib.sonare_realtime_voice_changer_set_config_json(
            self._require_handle(), _voice_config_to_json(preset)
        )
        _check(rc)

    def latency_samples(self) -> int:
        out = ctypes.c_int()
        rc = self._lib.sonare_realtime_voice_changer_latency_samples(
            self._require_handle(), ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

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
        out = ctypes.c_uint32()
        rc = self._lib.sonare_realtime_voice_changer_non_finite_discard_count(
            self._require_handle(), ctypes.byref(out)
        )
        _check_realtime(rc)
        return int(out.value)

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

    def config_pod(self) -> RealtimeVoiceChangerConfig:
        """Return the live (normalized) configuration as a flat POD dataclass.

        Faster than :meth:`config_json` because it skips JSON serialisation on
        the C side and parsing on the Python side.
        """
        if not hasattr(self._lib, "sonare_realtime_voice_changer_get_config"):
            raise _not_supported(
                "loaded libsonare is missing sonare_realtime_voice_changer_get_config; "
                "rebuild the shared library."
            )
        pod = SonareRealtimeVoiceChangerConfig()
        rc = self._lib.sonare_realtime_voice_changer_get_config(
            self._require_handle(), ctypes.byref(pod)
        )
        _check(rc)
        return RealtimeVoiceChangerConfig.from_pod(pod)

    def set_config_pod(self, config: RealtimeVoiceChangerConfig) -> None:
        """Realtime-safe configuration update via the flat POD path.

        Equivalent to :meth:`set_config` but skips the JSON round-trip. A
        non-finite or out-of-range field raises ``SonareValueError`` naming the
        field and its range, and the previous configuration stays in effect.
        """
        if not hasattr(self._lib, "sonare_realtime_voice_changer_set_config"):
            raise _not_supported(
                "loaded libsonare is missing sonare_realtime_voice_changer_set_config; "
                "rebuild the shared library."
            )
        pod = config.to_pod()
        rc = self._lib.sonare_realtime_voice_changer_set_config(
            self._require_handle(), ctypes.byref(pod)
        )
        _check(rc)

    def process_mono(self, samples: Sequence[float] | list[float] | np.ndarray) -> np.ndarray:
        """Process a mono buffer block-by-block and return the result as ndarray.

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
        return self._process_mono(_as_float32_buffer(samples), None)

    def process_mono_with_carrier(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        carrier: Sequence[float] | list[float] | np.ndarray,
    ) -> np.ndarray:
        """Process a mono buffer with an external vocoder ``carrier`` of equal length.

        The carrier is read only while the vocoder runs with the ``"external"``
        carrier source; with ``"internal"`` it is ignored. A carrier of a
        different length raises ``SonareValueError``. With an external carrier
        configured, the carrier-less ``process_*`` methods raise ``SonareError``
        (invalid state) and write nothing.
        """
        in_buf = _as_float32_buffer(samples)
        carrier_buf = _as_float32_buffer(carrier, arg_name="carrier")
        if int(carrier_buf.shape[0]) != int(in_buf.shape[0]):
            raise SonareValueError("carrier must have the same length as samples")
        return self._process_mono(in_buf, carrier_buf)

    def _process_mono(self, in_buf: np.ndarray, carrier_buf: np.ndarray | None) -> np.ndarray:
        total = int(in_buf.shape[0])
        out_buf = np.empty(total, dtype=np.float32)
        step = self._max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            in_block = in_buf[pos : pos + length]
            out_block = out_buf[pos : pos + length]
            # `from_buffer` requires C-contiguous memory; slices of a
            # contiguous 1-D float32 array are guaranteed contiguous.
            c_in = (ctypes.c_float * length).from_buffer(in_block)
            c_out = (ctypes.c_float * length).from_buffer(out_block)
            if carrier_buf is None:
                rc = self._lib.sonare_realtime_voice_changer_process_mono(
                    self._require_handle(), c_in, c_out, _to_c_size_t(length, "length")
                )
            else:
                c_carrier = (ctypes.c_float * length).from_buffer(carrier_buf[pos : pos + length])
                rc = self._lib.sonare_realtime_voice_changer_process_mono_with_carrier(
                    self._require_handle(), c_in, c_carrier, c_out, _to_c_size_t(length, "length")
                )
            _check_realtime(rc)
        return out_buf

    def process_interleaved(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        channels: int | None = None,
    ) -> np.ndarray:
        """Process an interleaved (LRLR...) buffer block-by-block.

        Returns a ``numpy.ndarray`` of dtype ``float32`` in the same
        interleaved layout as the input.
        """
        return self._process_interleaved(samples, channels, None)

    def process_interleaved_with_carrier(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        carrier: Sequence[float] | list[float] | np.ndarray,
        channels: int | None = None,
    ) -> np.ndarray:
        """Process an interleaved buffer with a mono external vocoder ``carrier``.

        ``carrier`` has one sample per frame and is shared by every channel; a
        different length raises ``SonareValueError``.
        """
        return self._process_interleaved(samples, channels, carrier)

    def _process_interleaved(
        self,
        samples: Sequence[float] | list[float] | np.ndarray,
        channels: int | None,
        carrier: Sequence[float] | list[float] | np.ndarray | None,
    ) -> np.ndarray:
        # Narrowed rather than coerced: int() takes 2.7 as 2, which lays the
        # buffer out as a stereo interleave nobody asked for.
        ch = (
            self._channels
            if channels is None
            else _narrow_int(channels, "channels", _C_INT_MIN, _C_INT_MAX)
        )
        in_buf = _as_float32_buffer(samples)
        total_samples = int(in_buf.shape[0])
        if ch <= 0 or total_samples % ch != 0:
            raise SonareValueError("interleaved samples length must be divisible by channels")
        frames = total_samples // ch
        carrier_buf = None if carrier is None else _as_float32_buffer(carrier, arg_name="carrier")
        if carrier_buf is not None and int(carrier_buf.shape[0]) != frames:
            raise SonareValueError("carrier must hold one sample per frame of samples")
        out_buf = np.empty(total_samples, dtype=np.float32)
        step = self._max_block_size
        for frame in range(0, frames, step):
            block_frames = min(step, frames - frame)
            start = frame * ch
            end = start + block_frames * ch
            in_block = in_buf[start:end]
            out_block = out_buf[start:end]
            c_in = (ctypes.c_float * (block_frames * ch)).from_buffer(in_block)
            c_out = (ctypes.c_float * (block_frames * ch)).from_buffer(out_block)
            if carrier_buf is None:
                rc = self._lib.sonare_realtime_voice_changer_process_interleaved(
                    self._require_handle(),
                    c_in,
                    c_out,
                    _to_c_size_t(block_frames, "block_frames"),
                    _to_c_int(ch, "ch"),
                )
            else:
                c_carrier = (ctypes.c_float * block_frames).from_buffer(
                    carrier_buf[frame : frame + block_frames]
                )
                rc = self._lib.sonare_realtime_voice_changer_process_interleaved_with_carrier(
                    self._require_handle(),
                    c_in,
                    c_carrier,
                    c_out,
                    _to_c_size_t(block_frames, "block_frames"),
                    _to_c_int(ch, "ch"),
                )
            _check_realtime(rc)
        return out_buf

    def process_planar_stereo(
        self,
        left: Sequence[float] | list[float] | np.ndarray,
        right: Sequence[float] | list[float] | np.ndarray,
    ) -> tuple[np.ndarray, np.ndarray]:
        """Process planar (non-interleaved) stereo audio block-by-block.

        ``left`` and ``right`` are separate channel buffers of equal length.
        The handle must have been prepared with at least 2 channels. Returns a
        ``(left, right)`` tuple of ``numpy.ndarray`` (dtype ``float32``)
        processed in place.
        """
        return self._process_planar_stereo(left, right, None)

    def process_planar_stereo_with_carrier(
        self,
        left: Sequence[float] | list[float] | np.ndarray,
        right: Sequence[float] | list[float] | np.ndarray,
        carrier: Sequence[float] | list[float] | np.ndarray,
    ) -> tuple[np.ndarray, np.ndarray]:
        """Process planar stereo with a mono external vocoder ``carrier``.

        ``carrier`` is shared by both channels and must match their length.
        """
        return self._process_planar_stereo(left, right, carrier)

    def _process_planar_stereo(
        self,
        left: Sequence[float] | list[float] | np.ndarray,
        right: Sequence[float] | list[float] | np.ndarray,
        carrier: Sequence[float] | list[float] | np.ndarray | None,
    ) -> tuple[np.ndarray, np.ndarray]:
        left_buf = _as_float32_buffer(left, arg_name="left")
        right_buf = _as_float32_buffer(right, arg_name="right")
        total = int(left_buf.shape[0])
        if total != int(right_buf.shape[0]):
            raise SonareValueError("left and right channels must have equal length")
        carrier_buf = None if carrier is None else _as_float32_buffer(carrier, arg_name="carrier")
        if carrier_buf is not None and int(carrier_buf.shape[0]) != total:
            raise SonareValueError("carrier must have the same length as the channels")
        # Copy into fresh contiguous output buffers; the C call mutates in place.
        out_left = np.array(left_buf, dtype=np.float32, copy=True)
        out_right = np.array(right_buf, dtype=np.float32, copy=True)
        step = self._max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            l_block = out_left[pos : pos + length]
            r_block = out_right[pos : pos + length]
            c_left = (ctypes.c_float * length).from_buffer(l_block)
            c_right = (ctypes.c_float * length).from_buffer(r_block)
            if carrier_buf is None:
                rc = self._lib.sonare_realtime_voice_changer_process_planar_stereo(
                    self._require_handle(), c_left, c_right, _to_c_size_t(length, "length")
                )
            else:
                c_carrier = (ctypes.c_float * length).from_buffer(carrier_buf[pos : pos + length])
                rc = self._lib.sonare_realtime_voice_changer_process_planar_stereo_with_carrier(
                    self._require_handle(),
                    c_left,
                    c_right,
                    c_carrier,
                    _to_c_size_t(length, "length"),
                )
            _check_realtime(rc)
        return out_left, out_right

    def note_on(self, note: int, velocity: float = 1.0) -> None:
        """Hold MIDI ``note`` from the start of the next processed block.

        Held notes become the voices' pitch targets, the newest leading, up to
        ``1 + harmony_voices`` of them. ``note`` outside [0, 127] or
        ``velocity`` outside (0, 1] raises ``SonareValueError``. A grain-engine
        handle, or a full event queue (the event is dropped), raises
        ``SonareError`` (invalid state).
        """
        rc = self._lib.sonare_realtime_voice_changer_note_on(
            self._require_handle(),
            _to_c_int(note, "note"),
            _to_c_float(velocity, "velocity"),
        )
        _check_realtime(rc)

    def note_off(self, note: int) -> None:
        """Release MIDI ``note``; releasing a note that is not held does nothing."""
        rc = self._lib.sonare_realtime_voice_changer_note_off(
            self._require_handle(), _to_c_int(note, "note")
        )
        _check_realtime(rc)

    def all_notes_off(self) -> None:
        """Release every held note; succeeds even when the event queue is full."""
        _check_realtime(
            self._lib.sonare_realtime_voice_changer_all_notes_off(self._require_handle())
        )


def voice_change_realtime(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 48000,
    preset: str | Mapping[str, object] = "neutral-monitor",
    *,
    channels: int = 1,
) -> np.ndarray:
    """Apply the realtime voice changer to a buffer offline.

    When ``channels`` > 1 the input must be interleaved (LRLR…) and the
    output is returned interleaved in the same layout.

    The chain's processing latency (retune grain + ISP-limiter lookahead) is
    compensated internally — a silent tail is flushed and the leading pre-roll
    is dropped — so the result is time-aligned with the input rather than
    shifted by the chain latency.

    Returns a ``numpy.ndarray`` of dtype ``float32`` (changed from
    ``list[float]`` in the prior implementation). ``len(result)`` still
    equals ``len(samples)`` so existing length-based assertions keep
    working.
    """
    if channels < 1 or channels > 2:
        raise SonareValueError("channels must be 1 or 2")
    in_buf = _validate_samples("voice_change_realtime", samples, validate=True)
    if channels == 2 and int(in_buf.shape[0]) % 2 != 0:
        raise SonareValueError(
            "voice_change_realtime: interleaved stereo input length must be even"
        )
    lib = _get_lib()
    c_array, length = _to_c_float_array(in_buf)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_voice_change_realtime(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                _voice_config_to_json(preset),
                _to_c_int(channels, "channels"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


def realtime_voice_changer_preset_names() -> list[str]:
    raw = _get_lib().sonare_realtime_voice_changer_preset_names()
    if not raw:
        return []
    # The C API returns a newline-separated list (matching every other
    # *_names API in libsonare); split on '\n' and drop empty entries.
    return [s for s in raw.decode("utf-8").split("\n") if s.strip()]


def realtime_voice_changer_preset_json(name: str) -> str:
    lib = _get_lib()
    out = ctypes.c_char_p()
    rc = lib.sonare_realtime_voice_changer_preset_json(_utf8_arg(name, "name"), ctypes.byref(out))
    _check(rc)
    try:
        return ctypes.string_at(out).decode("utf-8")
    finally:
        if out:
            lib.sonare_free_string(out)


def validate_realtime_voice_changer_preset_json(json_text: str) -> dict[str, object]:
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


_VC_PRESET_NAME_TO_ORDINAL: dict[str, int] = {
    "neutral-monitor": SONARE_VC_PRESET_NEUTRAL_MONITOR,
    "bright-idol": SONARE_VC_PRESET_BRIGHT_IDOL,
    "soft-whisper": SONARE_VC_PRESET_SOFT_WHISPER,
    "deep-narrator": SONARE_VC_PRESET_DEEP_NARRATOR,
    "robot-mascot": SONARE_VC_PRESET_ROBOT_MASCOT,
    "dark-villain": SONARE_VC_PRESET_DARK_VILLAIN,
    "hard-tune": SONARE_VC_PRESET_HARD_TUNE,
    "robot-monotone": SONARE_VC_PRESET_ROBOT_MONOTONE,
    "choir-harmony": SONARE_VC_PRESET_CHOIR_HARMONY,
    "vocoder-strings": SONARE_VC_PRESET_VOCODER_STRINGS,
    "megaphone": SONARE_VC_PRESET_MEGAPHONE,
    "radio-voice": SONARE_VC_PRESET_RADIO_VOICE,
}


def voice_character_preset_id(preset: int) -> str | None:
    """Map a voice-character preset enum ordinal to its canonical id string.

    Args:
        preset: A ``SONARE_VC_PRESET_*`` enum value.

    Returns:
        The canonical preset id (e.g. ``"bright-idol"``) or ``None`` when the
        ordinal is out of range. This is the reverse of
        :data:`_VC_PRESET_NAME_TO_ORDINAL`.
    """
    raw = _get_lib().sonare_voice_character_preset_id(_to_c_int(preset, "preset"))
    if not raw:
        return None
    return cast(str, raw.decode("utf-8"))


@functools.lru_cache(maxsize=1)
def _neutral_monitor_defaults() -> dict[str, Any]:
    """Field values of the built-in ``neutral-monitor`` preset.

    These are the single source of truth for the default-constructed
    :class:`RealtimeVoiceChangerConfig`, so that ``RealtimeVoiceChangerConfig()``
    followed by :func:`RealtimeVoiceChanger.set_config_pod` reproduces the same
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
        values[name] = value
    return values


def _neutral_default(name: str) -> Callable[[], Any]:
    """Return a zero-arg factory yielding the preset default for ``name``.

    The return type is ``Any`` because the same helper seeds both float and int
    fields; the concrete POD value carries the correct numeric type.
    """
    return lambda: _neutral_monitor_defaults()[name]


@dataclasses.dataclass
class RealtimeVoiceChangerConfig:
    """Flat mirror of :class:`SonareRealtimeVoiceChangerConfig`.

    Field order matches the C POD struct in ``sonare_c.h``. The default value of
    every field is seeded from the built-in ``neutral-monitor`` preset (fetched
    from the library once and cached), so a default-constructed instance is a
    usable neutral voice rather than a bypassed-gate / flat-EQ configuration.
    An out-of-range entry is refused by
    :func:`RealtimeVoiceChanger.set_config_pod` and at creation with a
    ``SonareValueError``; nothing is clamped.

    ``formant_mode`` is ``"relative"`` (default) or ``"absolute"`` and is fixed
    when the changer is created: :func:`RealtimeVoiceChanger.set_config_pod`
    with a different mode raises. In absolute mode ``formant_factor`` is the
    formant shift relative to the input, ``formant_amount`` is ignored, one
    analysis frame of latency is added, and a configuration whose warp
    ``formant_factor / 2**(retune_semitones / 12)`` leaves [0.55, 1.65] raises
    a ``SonareValueError`` naming the reachable ``formant_factor`` range.

    The enum-valued fields hold snake_case spellings: ``voice_engine_engine``
    (``"grain"`` or ``"psola"``, fixed at creation), ``pitch_correction_mode``,
    ``pitch_correction_scale``, ``vocoder_carrier_source``,
    ``vocoder_carrier_wave``, ``character_type``, ``reverb_variation_variation``,
    ``reverb_variation_division`` and ``midi_release``. ``harmony_steps`` holds
    exactly three scale-degree offsets. An unknown spelling raises
    ``SonareValueError``.

    The gate, compressor and limiter ``*_attack_ms`` / ``*_release_ms`` times are
    one time constant: about 63% of the way at the set time.
    """

    input_gain_db: float = dataclasses.field(default_factory=_neutral_default("input_gain_db"))
    output_gain_db: float = dataclasses.field(default_factory=_neutral_default("output_gain_db"))
    wet_mix: float = dataclasses.field(default_factory=_neutral_default("wet_mix"))
    retune_semitones: float = dataclasses.field(
        default_factory=_neutral_default("retune_semitones")
    )
    retune_mix: float = dataclasses.field(default_factory=_neutral_default("retune_mix"))
    retune_grain_size: int = dataclasses.field(
        default_factory=_neutral_default("retune_grain_size")
    )
    formant_factor: float = dataclasses.field(default_factory=_neutral_default("formant_factor"))
    formant_amount: float = dataclasses.field(default_factory=_neutral_default("formant_amount"))
    formant_body: float = dataclasses.field(default_factory=_neutral_default("formant_body"))
    formant_brightness: float = dataclasses.field(
        default_factory=_neutral_default("formant_brightness")
    )
    formant_nasal: float = dataclasses.field(default_factory=_neutral_default("formant_nasal"))
    formant_mode: str = dataclasses.field(default_factory=_neutral_default("formant_mode"))
    eq_highpass_hz: float = dataclasses.field(default_factory=_neutral_default("eq_highpass_hz"))
    eq_body_db: float = dataclasses.field(default_factory=_neutral_default("eq_body_db"))
    eq_presence_db: float = dataclasses.field(default_factory=_neutral_default("eq_presence_db"))
    eq_air_db: float = dataclasses.field(default_factory=_neutral_default("eq_air_db"))
    gate_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("gate_threshold_db")
    )
    gate_attack_ms: float = dataclasses.field(default_factory=_neutral_default("gate_attack_ms"))
    gate_release_ms: float = dataclasses.field(default_factory=_neutral_default("gate_release_ms"))
    gate_range_db: float = dataclasses.field(default_factory=_neutral_default("gate_range_db"))
    compressor_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("compressor_threshold_db")
    )
    compressor_ratio: float = dataclasses.field(
        default_factory=_neutral_default("compressor_ratio")
    )
    compressor_attack_ms: float = dataclasses.field(
        default_factory=_neutral_default("compressor_attack_ms")
    )
    compressor_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("compressor_release_ms")
    )
    compressor_makeup_gain_db: float = dataclasses.field(
        default_factory=_neutral_default("compressor_makeup_gain_db")
    )
    deesser_frequency_hz: float = dataclasses.field(
        default_factory=_neutral_default("deesser_frequency_hz")
    )
    deesser_threshold_db: float = dataclasses.field(
        default_factory=_neutral_default("deesser_threshold_db")
    )
    deesser_ratio: float = dataclasses.field(default_factory=_neutral_default("deesser_ratio"))
    deesser_range_db: float = dataclasses.field(
        default_factory=_neutral_default("deesser_range_db")
    )
    reverb_mix: float = dataclasses.field(default_factory=_neutral_default("reverb_mix"))
    reverb_time_ms: float = dataclasses.field(default_factory=_neutral_default("reverb_time_ms"))
    reverb_damping: float = dataclasses.field(default_factory=_neutral_default("reverb_damping"))
    reverb_seed: int = dataclasses.field(default_factory=_neutral_default("reverb_seed"))
    limiter_ceiling_db: float = dataclasses.field(
        default_factory=_neutral_default("limiter_ceiling_db")
    )
    limiter_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("limiter_release_ms")
    )
    # Appended in ABI version 2 (kept at the END to match the C POD layout).
    limiter_enable_isp_limiter: int = dataclasses.field(
        default_factory=_neutral_default("limiter_enable_isp_limiter")
    )
    limiter_isp_ceiling_dbtp: float = dataclasses.field(
        default_factory=_neutral_default("limiter_isp_ceiling_dbtp")
    )
    voice_engine_engine: str = dataclasses.field(
        default_factory=_neutral_default("voice_engine_engine")
    )
    voice_engine_f0_min_hz: float = dataclasses.field(
        default_factory=_neutral_default("voice_engine_f0_min_hz")
    )
    pitch_correction_mode: str = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_mode")
    )
    pitch_correction_amount: float = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_amount")
    )
    pitch_correction_speed_ms: float = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_speed_ms")
    )
    pitch_correction_key_root: int = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_key_root")
    )
    pitch_correction_scale: str = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_scale")
    )
    pitch_correction_robot_octave: int = dataclasses.field(
        default_factory=_neutral_default("pitch_correction_robot_octave")
    )
    harmony_voices: int = dataclasses.field(default_factory=_neutral_default("harmony_voices"))
    harmony_steps: tuple[int, int, int] = dataclasses.field(
        default_factory=_neutral_default("harmony_steps")
    )
    harmony_level: float = dataclasses.field(default_factory=_neutral_default("harmony_level"))
    harmony_lead_level: float = dataclasses.field(
        default_factory=_neutral_default("harmony_lead_level")
    )
    harmony_detune_cents: float = dataclasses.field(
        default_factory=_neutral_default("harmony_detune_cents")
    )
    harmony_delay_ms: float = dataclasses.field(
        default_factory=_neutral_default("harmony_delay_ms")
    )
    vocoder_enabled: int = dataclasses.field(default_factory=_neutral_default("vocoder_enabled"))
    vocoder_bands: int = dataclasses.field(default_factory=_neutral_default("vocoder_bands"))
    vocoder_attack_ms: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_attack_ms")
    )
    vocoder_release_ms: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_release_ms")
    )
    vocoder_sibilance: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_sibilance")
    )
    vocoder_carrier_source: str = dataclasses.field(
        default_factory=_neutral_default("vocoder_carrier_source")
    )
    vocoder_carrier_wave: str = dataclasses.field(
        default_factory=_neutral_default("vocoder_carrier_wave")
    )
    vocoder_ensemble_detune_cents: float = dataclasses.field(
        default_factory=_neutral_default("vocoder_ensemble_detune_cents")
    )
    vocoder_level: float = dataclasses.field(default_factory=_neutral_default("vocoder_level"))
    character_type: str = dataclasses.field(default_factory=_neutral_default("character_type"))
    character_amount: float = dataclasses.field(
        default_factory=_neutral_default("character_amount")
    )
    character_tone: float = dataclasses.field(default_factory=_neutral_default("character_tone"))
    reverb_variation_variation: str = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_variation")
    )
    reverb_variation_delay_ms: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_delay_ms")
    )
    reverb_variation_feedback: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_feedback")
    )
    reverb_variation_tempo_bpm: float = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_tempo_bpm")
    )
    reverb_variation_division: str = dataclasses.field(
        default_factory=_neutral_default("reverb_variation_division")
    )
    midi_release: str = dataclasses.field(default_factory=_neutral_default("midi_release"))

    @classmethod
    def from_pod(cls, pod: SonareRealtimeVoiceChangerConfig) -> RealtimeVoiceChangerConfig:
        """Copy field-for-field out of a ctypes struct."""
        values: dict[str, Any] = {}
        for name, *_ in pod._fields_:
            if name == "struct_version":
                continue
            value = getattr(pod, name)
            if name == "harmony_steps":
                value = tuple(int(step) for step in value)
            elif name in _ENUM_FIELD_NAMES:
                value = _enum_name(name, value)
            values[name] = value
        return cls(**values)

    def to_pod(self) -> SonareRealtimeVoiceChangerConfig:
        """Copy field-for-field into a freshly allocated ctypes struct."""
        out = SonareRealtimeVoiceChangerConfig()
        out.struct_version = _REALTIME_CONFIG_STRUCT_VERSION
        for name, *_ in out._fields_:
            if name == "struct_version":
                continue
            value = getattr(self, name)
            if name == "harmony_steps":
                # An array element bypasses the struct's field guard, so narrow here.
                if isinstance(value, (str, bytes)) or not isinstance(value, Sequence):
                    raise TypeError("harmony_steps must be a sequence of 3 integers")
                if len(value) != _HARMONY_STEP_COUNT:
                    raise SonareValueError(
                        f"harmony_steps must hold {_HARMONY_STEP_COUNT} integers, got {len(value)}"
                    )
                for index, step in enumerate(value):
                    out.harmony_steps[index] = _narrow_int(
                        step, f"harmony_steps[{index}]", _C_INT_MIN, _C_INT_MAX
                    )
            elif name in _ENUM_FIELD_NAMES:
                setattr(out, name, _enum_ordinal(name, value))
            else:
                setattr(out, name, value)
        return out


def _resolve_preset_ordinal(preset: str | int) -> int:
    """Map a preset name to its ordinal, passing an integer ordinal through.

    The name half only. An integer is not checked against anything here -- the
    caller narrows it, and the C entry point owns the ordinal's own range.
    """
    if isinstance(preset, int):
        return preset
    _utf8_arg(preset, "preset")
    try:
        return _VC_PRESET_NAME_TO_ORDINAL[preset]
    except KeyError as exc:
        raise SonareValueError(f"unknown voice character preset: {preset!r}") from exc


def realtime_voice_changer_preset_config(preset: str | int) -> RealtimeVoiceChangerConfig:
    """Return the canonical (normalised) config for a built-in preset.

    Skips the JSON round-trip that ``realtime_voice_changer_preset_json``
    incurs. Accepts either the canonical preset id (``"neutral-monitor"``,
    ...) or the integer ordinal from :data:`SONARE_VC_PRESET_NEUTRAL_MONITOR`
    and friends.

    This is the canonical name shared with the C / Node / WASM ``preset_config``
    surfaces; :func:`realtime_voice_changer_preset_pod` is a deprecated alias.

    Raises:
        SonareValueError: If ``preset`` is neither a known name nor an integer
            ordinal a C ``int`` holds.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_realtime_voice_changer_preset_config"):
        raise _not_supported(
            "loaded libsonare is missing sonare_realtime_voice_changer_preset_config; "
            "rebuild the shared library."
        )
    pod = SonareRealtimeVoiceChangerConfig()
    rc = lib.sonare_realtime_voice_changer_preset_config(
        _to_c_int(_resolve_preset_ordinal(preset), "preset"), ctypes.byref(pod)
    )
    _check(rc)
    return RealtimeVoiceChangerConfig.from_pod(pod)


def realtime_voice_changer_preset_pod(preset: str | int) -> RealtimeVoiceChangerConfig:
    """Deprecated alias for :func:`realtime_voice_changer_preset_config`.

    Retained for backward compatibility; prefer the ``preset_config`` name,
    which matches the other language bindings.
    """
    return realtime_voice_changer_preset_config(preset)


class StreamingRetune:
    """Block-by-block mono pitch shifter (grain overlap-add).

    The streaming counterpart of the offline ``pitch_shift``: state persists
    across :meth:`process_mono` calls, so consecutive blocks of a live stream
    join without a seam. Call :meth:`prepare` before processing.

    A non-finite ``semitones`` / ``mix`` is rejected rather than substituted,
    and a block holding a non-finite sample is rejected rather than zeroed:
    either would otherwise enter the grain history and persist into every
    later block with nothing to tell the caller its input had been altered.
    """

    # Declared on the class so close() reads it even when __init__ never ran.
    _handle: ctypes.c_void_p | None = None

    def __init__(
        self,
        *,
        semitones: float = 0.0,
        mix: float = 1.0,
        grain_size: int = 0,
    ) -> None:
        # Set first so the non-finite rejection below leaves a valid attribute
        # for __del__/close() instead of raising AttributeError over it.
        self._handle = ctypes.c_void_p()
        self._lib = _get_lib()
        handle = self._lib.sonare_streaming_retune_create(
            _to_c_float(semitones, "semitones"),
            _to_c_float(mix, "mix"),
            _to_c_int(grain_size, "grain_size"),
        )
        if not handle:
            raise _last_error(int(ErrorCode.INVALID_PARAMETER), "failed to create StreamingRetune")
        self._handle = ctypes.c_void_p(handle)
        # The grain size the caller last ASKED for. ``config()`` reports the
        # EFFECTIVE grain once prepared, so seeding an omitted ``grain_size``
        # from it froze the 0 "derive from the sample rate" sentinel into a
        # literal and a re-``prepare`` at another rate kept the first rate's
        # grain. The core draws the same distinction internally, but it is not
        # readable across the C ABI.
        self._requested_grain_size = grain_size

    def close(self) -> None:
        if self._handle:
            self._lib.sonare_streaming_retune_destroy(self._handle)
            self._handle = ctypes.c_void_p()

    def __enter__(self) -> StreamingRetune:
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        with contextlib.suppress(Exception):
            self.close()

    def _require_handle(self) -> ctypes.c_void_p:
        return _require_open(self._handle, "StreamingRetune")

    def prepare(self, sample_rate: float, max_block_size: int) -> None:
        """Allocate native state for ``sample_rate`` and resolve the grain size."""
        _check(
            self._lib.sonare_streaming_retune_prepare(
                self._require_handle(),
                _to_c_double(sample_rate, "sample_rate"),
                _to_c_int(max_block_size, "max_block_size"),
            )
        )

    def reset(self) -> None:
        """Clear grain / overlap-add state without changing the config."""
        _check(self._lib.sonare_streaming_retune_reset(self._require_handle()))

    def set_config(
        self,
        *,
        semitones: float | None = None,
        mix: float | None = None,
        grain_size: int | None = None,
    ) -> None:
        """Update the live controls; omitted keywords keep their current value.

        ``grain_size`` is structural: it is remembered and applied by the next
        :meth:`prepare`, and :meth:`config` keeps reporting the effective grain
        until then. An omitted ``grain_size`` keeps the last REQUESTED value,
        including the ``0`` sentinel, so re-preparing at another sample rate
        re-derives the grain instead of freezing the first rate's answer.
        """
        current = self.config()
        # Narrowed rather than coerced: int() takes 0.5 as the 0 this field reads
        # as "derive the grain from the sample rate".
        requested_grain_size = (
            self._requested_grain_size
            if grain_size is None
            else _narrow_int(grain_size, "grain_size", _C_INT_MIN, _C_INT_MAX)
        )
        _check(
            self._lib.sonare_streaming_retune_set_config(
                self._require_handle(),
                _to_c_float(current["semitones"] if semitones is None else semitones, "semitones"),
                _to_c_float(current["mix"] if mix is None else mix, "mix"),
                _to_c_int(requested_grain_size, "requested_grain_size"),
            )
        )
        self._requested_grain_size = requested_grain_size

    def config(self) -> dict[str, float | int]:
        """The applied controls; ``grain_size`` is the effective one."""
        semitones = ctypes.c_float()
        mix = ctypes.c_float()
        grain_size = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_config(
                self._require_handle(),
                ctypes.byref(semitones),
                ctypes.byref(mix),
                ctypes.byref(grain_size),
            )
        )
        return {
            "semitones": float(semitones.value),
            "mix": float(mix.value),
            "grain_size": int(grain_size.value),
        }

    def grain_size(self) -> int:
        """Resolved grain length in samples; 0 before :meth:`prepare`."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_grain_size(self._require_handle(), ctypes.byref(out))
        )
        return int(out.value)

    def latency_samples(self) -> int:
        """Fixed overlap-add latency in samples (one grain); 0 before prepare."""
        out = ctypes.c_int()
        _check(
            self._lib.sonare_streaming_retune_latency_samples(
                self._require_handle(), ctypes.byref(out)
            )
        )
        return int(out.value)

    def process_mono(self, samples: Sequence[float]) -> list[float]:
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
        return cast(list[float], buffer.tolist())
