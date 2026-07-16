"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import json
from collections.abc import Mapping, Sequence
from typing import cast

import numpy as np

from ._ffi import (
    SONARE_OK,
    SONARE_VC_PRESET_BRIGHT_IDOL,
    SONARE_VC_PRESET_DARK_VILLAIN,
    SONARE_VC_PRESET_DEEP_NARRATOR,
    SONARE_VC_PRESET_NEUTRAL_MONITOR,
    SONARE_VC_PRESET_ROBOT_MASCOT,
    SONARE_VC_PRESET_SOFT_WHISPER,
    SonareRealtimeVoiceChangerConfig,
)
from ._runtime import (
    _as_float32_buffer,
    _check,
    _float_array_result,
    _from_c_float_array,
    _get_lib,
    _out_float_array,
    _to_c_float_array,
    _validate_samples,
)


def voice_change(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    pitch_semitones: float = 0.0,
    formant_factor: float = 1.0,
) -> list[float]:
    """Apply a voice-change effect with independent pitch and formant control.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        pitch_semitones: Pitch shift in semitones (positive = up).
        formant_factor: Formant scaling factor (1.0 = unchanged).

    Returns:
        List of voice-changed samples.
    """
    _validate_samples("voice_change", samples, validate=True)
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_voice_change(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                ctypes.c_float(pitch_semitones),
                ctypes.c_float(formant_factor),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


def _voice_config_to_json(preset: str | Mapping[str, object]) -> bytes:
    if isinstance(preset, str):
        return preset.encode("utf-8")
    return json.dumps(preset, separators=(",", ":")).encode("utf-8")


class RealtimeVoiceChanger:
    """Streaming realtime voice changer backed by the libsonare C API."""

    def __init__(
        self,
        sample_rate: int,
        preset: str | Mapping[str, object] = "neutral-monitor",
        *,
        max_block_size: int = 128,
        channels: int = 1,
    ) -> None:
        self._lib = _get_lib()
        self._handle = ctypes.c_void_p()
        self._max_block_size = int(max_block_size)
        self._channels = int(channels)
        rc = self._lib.sonare_realtime_voice_changer_create_json(
            _voice_config_to_json(preset),
            ctypes.c_int(sample_rate),
            ctypes.c_int(max_block_size),
            ctypes.c_int(channels),
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

    def reset(self) -> None:
        rc = self._lib.sonare_realtime_voice_changer_reset(self._handle)
        _check(rc)

    def set_config(self, preset: str | Mapping[str, object]) -> None:
        rc = self._lib.sonare_realtime_voice_changer_set_config_json(
            self._handle, _voice_config_to_json(preset)
        )
        _check(rc)

    def latency_samples(self) -> int:
        out = ctypes.c_int()
        rc = self._lib.sonare_realtime_voice_changer_latency_samples(
            self._handle, ctypes.byref(out)
        )
        _check(rc)
        return int(out.value)

    def config_json(self) -> str:
        """Return the live (normalized) configuration as a JSON document.

        Useful for syncing UI state or roundtripping the post-normalize
        configuration into a different language binding.
        """
        out = ctypes.c_char_p()
        rc = self._lib.sonare_realtime_voice_changer_config_json(self._handle, ctypes.byref(out))
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
            raise RuntimeError(
                "loaded libsonare is missing sonare_realtime_voice_changer_get_config; "
                "rebuild the shared library."
            )
        pod = SonareRealtimeVoiceChangerConfig()
        rc = self._lib.sonare_realtime_voice_changer_get_config(self._handle, ctypes.byref(pod))
        _check(rc)
        return RealtimeVoiceChangerConfig.from_pod(pod)

    def set_config_pod(self, config: RealtimeVoiceChangerConfig) -> None:
        """Realtime-safe configuration update via the flat POD path.

        Equivalent to :meth:`set_config` but skips the JSON round-trip. The C
        side still clamps out-of-range values rather than rejecting them.
        """
        if not hasattr(self._lib, "sonare_realtime_voice_changer_set_config"):
            raise RuntimeError(
                "loaded libsonare is missing sonare_realtime_voice_changer_set_config; "
                "rebuild the shared library."
            )
        pod = config.to_pod()
        rc = self._lib.sonare_realtime_voice_changer_set_config(self._handle, ctypes.byref(pod))
        _check(rc)

    def process_mono(self, samples: Sequence[float] | list[float] | np.ndarray) -> np.ndarray:
        """Process a mono buffer block-by-block and return the result as ndarray.

        Returns a ``numpy.ndarray`` of dtype ``float32`` (changed from
        ``list[float]`` in the prior implementation). The realtime path is
        zero-copy: each ``max_block_size`` block aliases the input/output
        numpy buffers as ``c_float*`` via ``ctypes.from_buffer``, so the
        per-block Python overhead is independent of block size.
        """
        in_buf = _as_float32_buffer(samples)
        total = int(in_buf.shape[0])
        out_buf = np.empty(total, dtype=np.float32)
        step = self._max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            in_block = in_buf[pos : pos + length]
            out_block = out_buf[pos : pos + length]
            # `from_buffer` requires C-contiguous memory; slices of a
            # contiguous 1-D float32 array are guaranteed contiguous.
            c_in = (ctypes.c_float * length).from_buffer(in_block)  # type: ignore[arg-type]
            c_out = (ctypes.c_float * length).from_buffer(out_block)  # type: ignore[arg-type]
            rc = self._lib.sonare_realtime_voice_changer_process_mono(
                self._handle, c_in, c_out, ctypes.c_size_t(length)
            )
            _check(rc)
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
        ch = self._channels if channels is None else int(channels)
        in_buf = _as_float32_buffer(samples)
        total_samples = int(in_buf.shape[0])
        if ch <= 0 or total_samples % ch != 0:
            raise ValueError("interleaved samples length must be divisible by channels")
        frames = total_samples // ch
        out_buf = np.empty(total_samples, dtype=np.float32)
        step = self._max_block_size
        for frame in range(0, frames, step):
            block_frames = min(step, frames - frame)
            start = frame * ch
            end = start + block_frames * ch
            in_block = in_buf[start:end]
            out_block = out_buf[start:end]
            c_in = (ctypes.c_float * (block_frames * ch)).from_buffer(
                in_block  # type: ignore[arg-type]
            )
            c_out = (ctypes.c_float * (block_frames * ch)).from_buffer(
                out_block  # type: ignore[arg-type]
            )
            rc = self._lib.sonare_realtime_voice_changer_process_interleaved(
                self._handle,
                c_in,
                c_out,
                ctypes.c_size_t(block_frames),
                ctypes.c_int(ch),
            )
            _check(rc)
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
        left_buf = _as_float32_buffer(left)
        right_buf = _as_float32_buffer(right)
        total = int(left_buf.shape[0])
        if total != int(right_buf.shape[0]):
            raise ValueError("left and right channels must have equal length")
        # Copy into fresh contiguous output buffers; the C call mutates in place.
        out_left = np.array(left_buf, dtype=np.float32, copy=True)
        out_right = np.array(right_buf, dtype=np.float32, copy=True)
        step = self._max_block_size
        for pos in range(0, total, step):
            length = min(step, total - pos)
            l_block = out_left[pos : pos + length]
            r_block = out_right[pos : pos + length]
            c_left = (ctypes.c_float * length).from_buffer(l_block)  # type: ignore[arg-type]
            c_right = (ctypes.c_float * length).from_buffer(r_block)  # type: ignore[arg-type]
            rc = self._lib.sonare_realtime_voice_changer_process_planar_stereo(
                self._handle, c_left, c_right, ctypes.c_size_t(length)
            )
            _check(rc)
        return out_left, out_right


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
        raise ValueError("channels must be 1 or 2")
    in_buf = _validate_samples("voice_change_realtime", samples, validate=True)
    if channels == 2 and int(in_buf.shape[0]) % 2 != 0:
        raise ValueError("voice_change_realtime: interleaved stereo input length must be even")
    lib = _get_lib()
    c_array, length = _to_c_float_array(in_buf)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_voice_change_realtime(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                _voice_config_to_json(preset),
                ctypes.c_int(channels),
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
    rc = lib.sonare_realtime_voice_changer_preset_json(name.encode("utf-8"), ctypes.byref(out))
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
        json_text.encode("utf-8"), ctypes.byref(normalized), ctypes.byref(error)
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
    raw = _get_lib().sonare_voice_character_preset_id(ctypes.c_int(preset))
    if not raw:
        return None
    return cast(str, raw.decode("utf-8"))


@dataclasses.dataclass
class RealtimeVoiceChangerConfig:
    """Flat mirror of :class:`SonareRealtimeVoiceChangerConfig` (36 fields).

    Field order matches the C POD struct in ``sonare_c.h``. Values are
    normalised on the C side after :func:`RealtimeVoiceChanger.set_config_pod`,
    so out-of-range entries are clamped rather than rejected.
    """

    input_gain_db: float = 0.0
    output_gain_db: float = 0.0
    wet_mix: float = 1.0
    retune_semitones: float = 0.0
    retune_mix: float = 0.0
    retune_grain_size: int = 1024
    formant_factor: float = 1.0
    formant_amount: float = 0.0
    formant_body: float = 0.0
    formant_brightness: float = 0.0
    formant_nasal: float = 0.0
    eq_highpass_hz: float = 0.0
    eq_body_db: float = 0.0
    eq_presence_db: float = 0.0
    eq_air_db: float = 0.0
    gate_threshold_db: float = -60.0
    gate_attack_ms: float = 5.0
    gate_release_ms: float = 50.0
    gate_range_db: float = 0.0
    compressor_threshold_db: float = 0.0
    compressor_ratio: float = 1.0
    compressor_attack_ms: float = 10.0
    compressor_release_ms: float = 100.0
    compressor_makeup_gain_db: float = 0.0
    deesser_frequency_hz: float = 6000.0
    deesser_threshold_db: float = 0.0
    deesser_ratio: float = 1.0
    deesser_range_db: float = 0.0
    reverb_mix: float = 0.0
    reverb_time_ms: float = 0.0
    reverb_damping: float = 0.0
    reverb_seed: int = 0
    limiter_ceiling_db: float = 0.0
    limiter_release_ms: float = 50.0
    # Appended in ABI version 2 (kept at the END to match the C POD layout).
    limiter_enable_isp_limiter: int = 1
    limiter_isp_ceiling_dbtp: float = -1.0

    @classmethod
    def from_pod(cls, pod: SonareRealtimeVoiceChangerConfig) -> RealtimeVoiceChangerConfig:
        """Copy field-for-field out of a ctypes struct."""
        return cls(**{name: getattr(pod, name) for name, *_ in pod._fields_})

    def to_pod(self) -> SonareRealtimeVoiceChangerConfig:
        """Copy field-for-field into a freshly allocated ctypes struct."""
        out = SonareRealtimeVoiceChangerConfig()
        for name, *_ in out._fields_:
            setattr(out, name, getattr(self, name))
        return out


def _resolve_preset_ordinal(preset: str | int) -> int:
    if isinstance(preset, int):
        return preset
    try:
        return _VC_PRESET_NAME_TO_ORDINAL[preset]
    except KeyError as exc:
        raise ValueError(f"unknown voice character preset: {preset!r}") from exc


def realtime_voice_changer_preset_config(preset: str | int) -> RealtimeVoiceChangerConfig:
    """Return the canonical (normalised) config for a built-in preset.

    Skips the JSON round-trip that ``realtime_voice_changer_preset_json``
    incurs. Accepts either the canonical preset id (``"neutral-monitor"``,
    ...) or the integer ordinal from :data:`SONARE_VC_PRESET_NEUTRAL_MONITOR`
    and friends.

    This is the canonical name shared with the C / Node / WASM ``preset_config``
    surfaces; :func:`realtime_voice_changer_preset_pod` is a deprecated alias.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_realtime_voice_changer_preset_config"):
        raise RuntimeError(
            "loaded libsonare is missing sonare_realtime_voice_changer_preset_config; "
            "rebuild the shared library."
        )
    pod = SonareRealtimeVoiceChangerConfig()
    rc = lib.sonare_realtime_voice_changer_preset_config(
        ctypes.c_int(_resolve_preset_ordinal(preset)), ctypes.byref(pod)
    )
    _check(rc)
    return RealtimeVoiceChangerConfig.from_pod(pod)


def realtime_voice_changer_preset_pod(preset: str | int) -> RealtimeVoiceChangerConfig:
    """Deprecated alias for :func:`realtime_voice_changer_preset_config`.

    Retained for backward compatibility; prefer the ``preset_config`` name,
    which matches the other language bindings.
    """
    return realtime_voice_changer_preset_config(preset)
