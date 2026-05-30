"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import contextlib
import dataclasses
import json
from collections.abc import Mapping

import numpy as np

from ._runtime import *  # noqa: F403


def hpss(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
) -> HpssResult:
    """Perform harmonic-percussive source separation.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Harmonic median filter kernel size.
        kernel_percussive: Percussive median filter kernel size.

    Returns:
        HpssResult with harmonic and percussive components.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(kernel_harmonic),
        ctypes.c_int(kernel_percussive),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        n = out.length
        return HpssResult(
            harmonic=[float(out.harmonic[i]) for i in range(n)],
            percussive=[float(out.percussive[i]) for i in range(n)],
            length=int(n),
            sample_rate=int(out.sample_rate),
        )
    finally:
        lib.sonare_free_hpss_result(ctypes.byref(out))


def harmonic(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Extract the harmonic component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of harmonic component samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_harmonic(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def percussive(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Extract the percussive component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of percussive component samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_percussive(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def time_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    rate: float = 1.0,
) -> list[float]:
    """Time-stretch audio without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        rate: Stretch factor (>1 speeds up, <1 slows down).

    Returns:
        List of time-stretched samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_time_stretch(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def pitch_shift(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    semitones: float = 0.0,
) -> list[float]:
    """Shift the pitch of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        semitones: Number of semitones to shift (positive = up, negative = down).

    Returns:
        List of pitch-shifted samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_pitch_shift(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(semitones),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def pitch_correct_to_midi(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    current_midi: float = 69.0,
    target_midi: float = 69.0,
) -> list[float]:
    """Pitch-correct audio from a current MIDI note to a target MIDI note.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        current_midi: Detected/current pitch as a MIDI note number.
        target_midi: Desired pitch as a MIDI note number.

    Returns:
        List of pitch-corrected samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_pitch_correct_to_midi(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(current_midi),
        ctypes.c_float(target_midi),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


def note_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int = 0,
    stretch_ratio: float = 1.0,
) -> list[float]:
    """Time-stretch a single note region without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        onset_sample: Start sample index of the note region.
        offset_sample: End sample index of the note region.
        stretch_ratio: Stretch factor for the region (>1 lengthens).

    Returns:
        List of samples with the note region stretched.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_note_stretch(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(onset_sample),
        ctypes.c_int(offset_sample),
        ctypes.c_float(stretch_ratio),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


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
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_voice_change(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(pitch_semitones),
        ctypes.c_float(formant_factor),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


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
        out = ctypes.c_void_p()
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
            c_in = (ctypes.c_float * length).from_buffer(in_block)
            c_out = (ctypes.c_float * length).from_buffer(out_block)
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
            c_in = (ctypes.c_float * (block_frames * ch)).from_buffer(in_block)
            c_out = (ctypes.c_float * (block_frames * ch)).from_buffer(out_block)
            rc = self._lib.sonare_realtime_voice_changer_process_interleaved(
                self._handle,
                c_in,
                c_out,
                ctypes.c_size_t(block_frames),
                ctypes.c_int(ch),
            )
            _check(rc)
        return out_buf


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

    Returns a ``numpy.ndarray`` of dtype ``float32`` (changed from
    ``list[float]`` in the prior implementation). ``len(result)`` still
    equals ``len(samples)`` so existing length-based assertions keep
    working.
    """
    if channels < 1 or channels > 2:
        raise ValueError("channels must be 1 or 2")
    _validate_samples("voice_change_realtime", samples, validate=True)
    with RealtimeVoiceChanger(
        sample_rate, preset, max_block_size=128, channels=channels
    ) as changer:
        if channels == 1:
            return changer.process_mono(samples)
        return changer.process_interleaved(samples, channels=channels)


def realtime_voice_changer_preset_names() -> list[str]:
    raw = _get_lib().sonare_realtime_voice_changer_preset_names()
    if not raw:
        return []
    # The C API returns a newline-separated list (matching every other
    # *_names API in libsonare); split on '\n' and drop empty entries.
    return [s for s in raw.decode("utf-8").split("\n") if s.strip()]


def realtime_voice_changer_preset_json(name: str) -> str:
    lib = _get_lib()
    out = ctypes.c_void_p()
    rc = lib.sonare_realtime_voice_changer_preset_json(name.encode("utf-8"), ctypes.byref(out))
    _check(rc)
    try:
        return ctypes.string_at(out).decode("utf-8")
    finally:
        if out:
            lib.sonare_free_string(out)


def validate_realtime_voice_changer_preset_json(json_text: str) -> dict[str, object]:
    lib = _get_lib()
    normalized = ctypes.c_void_p()
    error = ctypes.c_void_p()
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


@dataclasses.dataclass
class RealtimeVoiceChangerConfig:
    """Flat mirror of :class:`SonareRealtimeVoiceChangerConfig` (34 fields).

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

    @classmethod
    def from_pod(cls, pod: SonareRealtimeVoiceChangerConfig) -> RealtimeVoiceChangerConfig:
        """Copy field-for-field out of a ctypes struct."""
        return cls(**{name: getattr(pod, name) for name, _ in pod._fields_})

    def to_pod(self) -> SonareRealtimeVoiceChangerConfig:
        """Copy field-for-field into a freshly allocated ctypes struct."""
        out = SonareRealtimeVoiceChangerConfig()
        for name, _ in out._fields_:
            setattr(out, name, getattr(self, name))
        return out


def _resolve_preset_ordinal(preset: str | int) -> int:
    if isinstance(preset, int):
        return preset
    try:
        return _VC_PRESET_NAME_TO_ORDINAL[preset]
    except KeyError as exc:
        raise ValueError(f"unknown voice character preset: {preset!r}") from exc


def realtime_voice_changer_preset_pod(preset: str | int) -> RealtimeVoiceChangerConfig:
    """Return the canonical (normalised) POD config for a built-in preset.

    Skips the JSON round-trip that ``realtime_voice_changer_preset_json``
    incurs. Accepts either the canonical preset id (``"neutral-monitor"``,
    ...) or the integer ordinal from :data:`SONARE_VC_PRESET_NEUTRAL_MONITOR`
    and friends.
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


def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = 0.0,
) -> list[float]:
    """Normalize audio to a target dB level.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Target peak level in dB (default 0.0 = full scale).

    Returns:
        List of normalized samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_normalize(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(target_db),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


_DENOISE_MODE_NAMES = {
    "logmmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "log_mmse": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "lsa": SONARE_DENOISE_MODE_LOG_MMSE,  # noqa: F405
    "mmsestsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "mmse_stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "stsa": SONARE_DENOISE_MODE_MMSE_STSA,  # noqa: F405
    "spectralsubtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "spectral_subtraction": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
    "ss": SONARE_DENOISE_MODE_SPECTRAL_SUBTRACTION,  # noqa: F405
}

_DENOISE_ESTIMATOR_NAMES = {
    "quantile": SONARE_DENOISE_NOISE_ESTIMATOR_QUANTILE,  # noqa: F405
    "mcra": SONARE_DENOISE_NOISE_ESTIMATOR_MCRA,  # noqa: F405
    "imcra": SONARE_DENOISE_NOISE_ESTIMATOR_IMCRA,  # noqa: F405
}


def _coerce_denoise_mode(value: int | str) -> int:
    if isinstance(value, int):
        if value not in _DENOISE_MODE_NAMES.values():
            raise ValueError(f"unknown denoise mode: {value!r}")
        return value
    key = value.strip().lower().replace("-", "_")
    if key not in _DENOISE_MODE_NAMES:
        raise ValueError(f"unknown denoise mode: {value!r}")
    return _DENOISE_MODE_NAMES[key]


def _coerce_denoise_estimator(value: int | str) -> int:
    if isinstance(value, int):
        if value not in _DENOISE_ESTIMATOR_NAMES.values():
            raise ValueError(f"unknown denoise noise estimator: {value!r}")
        return value
    key = value.strip().lower().replace("-", "_")
    if key not in _DENOISE_ESTIMATOR_NAMES:
        raise ValueError(f"unknown denoise noise estimator: {value!r}")
    return _DENOISE_ESTIMATOR_NAMES[key]


def mastering_repair_declick(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.8,
    neighbor_ratio: float = 4.0,
    max_click_samples: int = 8,
    lpc_order: int = 20,
    residual_ratio: float = 8.0,
) -> np.ndarray:
    """Offline LPC-based declicker.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold: Amplitude threshold vs LPC prediction (default 0.8).
        neighbor_ratio: Ratio vs neighbour amplitude (default 4.0).
        max_click_samples: Maximum click run length in samples (default 8).
        lpc_order: LPC order used for prediction (default 20).
        residual_ratio: Residual / signal threshold (default 8.0).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.
    """
    if max_click_samples <= 0:
        raise ValueError("max_click_samples must be positive")
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDeclickConfig(  # noqa: F405
        threshold=float(threshold),
        neighbor_ratio=float(neighbor_ratio),
        max_click_samples=int(max_click_samples),
        lpc_order=int(lpc_order),
        residual_ratio=float(residual_ratio),
    )
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mastering_repair_declick(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def mastering_repair_denoise_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    mode: int | str = "logMmse",
    noise_estimator: int | str = "quantile",
    n_fft: int = 1024,
    hop_length: int = 256,
    dd_alpha: float = 0.98,
    gain_floor: float = 0.05,
    over_subtraction: float = 2.0,
    spectral_floor: float = 0.05,
    noise_estimation_quantile: float = 0.1,
    speech_presence_gain: bool = True,
    gain_smoothing: bool = True,
) -> np.ndarray:
    """Offline STFT-domain classical denoiser.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        mode: ``"logMmse"`` (default), ``"mmseStsa"``, or ``"spectralSubtraction"``;
              an integer in ``SONARE_DENOISE_MODE_*`` is also accepted.
        noise_estimator: ``"quantile"`` (default), ``"mcra"``, or ``"imcra"``.
        n_fft: STFT size, must be a positive power of two (default 1024).
        hop_length: Hop size in samples (default 256).
        dd_alpha: Decision-directed a priori SNR smoothing (default 0.98).
        gain_floor: Minimum per-bin gain, linear (default 0.05).
        over_subtraction: Berouti alpha; SpectralSubtraction only (default 2.0).
        spectral_floor: Berouti beta; SpectralSubtraction only (default 0.05).
        noise_estimation_quantile: Fraction of frames assumed noise-only (default 0.1).
        speech_presence_gain: Apply speech-presence probability gating (default True).
        gain_smoothing: Smooth gains across time (default True).

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        ValueError: If ``mode`` / ``noise_estimator`` cannot be resolved.
        RuntimeError: If the C call rejects the request (e.g. non-power-of-two
        ``n_fft`` or non-positive ``hop_length``).
    """
    if n_fft <= 0 or (n_fft & (n_fft - 1)) != 0:
        raise ValueError("n_fft must be a positive power of two")
    if hop_length <= 0:
        raise ValueError("hop_length must be positive")

    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    config = SonareDenoiseClassicalConfig(  # noqa: F405
        mode=_coerce_denoise_mode(mode),
        noise_estimator=_coerce_denoise_estimator(noise_estimator),
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        dd_alpha=float(dd_alpha),
        gain_floor=float(gain_floor),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        noise_estimation_quantile=float(noise_estimation_quantile),
        speech_presence_gain=1 if speech_presence_gain else 0,
        gain_smoothing=1 if gain_smoothing else 0,
    )
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_mastering_repair_denoise_classical(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


_DECRACKLE_MODE_NAMES = {
    "median": SONARE_DECRACKLE_MODE_MEDIAN,  # noqa: F405
    "waveletshrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet_shrinkage": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
    "wavelet": SONARE_DECRACKLE_MODE_WAVELET_SHRINKAGE,  # noqa: F405
}

_TRIM_SILENCE_MODE_NAMES = {
    "peak": SONARE_TRIM_SILENCE_MODE_PEAK,  # noqa: F405
    "lufsgated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs_gated": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
    "lufs": SONARE_TRIM_SILENCE_MODE_LUFS_GATED,  # noqa: F405
}


def _coerce_decrackle_mode(value: int | str) -> int:
    if isinstance(value, int):
        if value not in _DECRACKLE_MODE_NAMES.values():
            raise ValueError(f"unknown decrackle mode: {value!r}")
        return value
    key = value.strip().lower().replace("-", "_")
    if key not in _DECRACKLE_MODE_NAMES:
        raise ValueError(f"unknown decrackle mode: {value!r}")
    return _DECRACKLE_MODE_NAMES[key]


def _coerce_trim_silence_mode(value: int | str) -> int:
    if isinstance(value, int):
        if value not in _TRIM_SILENCE_MODE_NAMES.values():
            raise ValueError(f"unknown trim_silence mode: {value!r}")
        return value
    key = value.strip().lower().replace("-", "_")
    if key not in _TRIM_SILENCE_MODE_NAMES:
        raise ValueError(f"unknown trim_silence mode: {value!r}")
    return _TRIM_SILENCE_MODE_NAMES[key]


def _run_repair(
    lib_fn,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    config,
) -> np.ndarray:
    lib = _get_lib()
    in_buf = _as_float32_buffer(samples)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib_fn(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def mastering_repair_declip(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    clip_threshold: float = 0.98,
    lpc_order: int = 36,
    iterations: int = 2,
    lpc_blend: float = 0.65,
) -> np.ndarray:
    """Offline LPC-based declipper."""
    config = SonareDeclipConfig(  # noqa: F405
        clip_threshold=float(clip_threshold),
        lpc_order=int(lpc_order),
        iterations=int(iterations),
        lpc_blend=float(lpc_blend),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_declip, samples, sample_rate, config)


def mastering_repair_decrackle(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.4,
    mode: int | str = "median",
    levels: int = 4,
) -> np.ndarray:
    """Offline crackle suppressor (median or wavelet-shrinkage)."""
    config = SonareDecrackleConfig(  # noqa: F405
        threshold=float(threshold),
        mode=_coerce_decrackle_mode(mode),
        levels=int(levels),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_decrackle, samples, sample_rate, config)


def mastering_repair_dehum(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    fundamental_hz: float = 50.0,
    harmonics: int = 4,
    q: float = 20.0,
    adaptive: bool = False,
    search_range_hz: float = 2.0,
    adaptation: float = 0.25,
    frame_size: int = 2048,
    pll_bandwidth: float = 0.01,
) -> np.ndarray:
    """Offline mains-hum remover."""
    config = SonareDehumConfig(  # noqa: F405
        fundamental_hz=float(fundamental_hz),
        harmonics=int(harmonics),
        q=float(q),
        adaptive=1 if adaptive else 0,
        search_range_hz=float(search_range_hz),
        adaptation=float(adaptation),
        frame_size=int(frame_size),
        pll_bandwidth=float(pll_bandwidth),
    )
    return _run_repair(_get_lib().sonare_mastering_repair_dehum, samples, sample_rate, config)


def mastering_repair_dereverb_classical(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.05,
    attenuation: float = 0.5,
    n_fft: int = 1024,
    hop_length: int = 256,
    t60_sec: float = 0.4,
    late_delay_ms: float = 50.0,
    over_subtraction: float = 1.0,
    spectral_floor: float = 0.08,
    wpe_enabled: bool = False,
    wpe_iterations: int = 2,
    wpe_taps: int = 3,
    wpe_strength: float = 0.7,
) -> np.ndarray:
    """Offline classical dereverberator (spectral subtraction + optional WPE)."""
    if n_fft <= 0 or (n_fft & (n_fft - 1)) != 0:
        raise ValueError("n_fft must be a positive power of two")
    if hop_length <= 0 or hop_length > n_fft:
        raise ValueError("hop_length must be in (0, n_fft]")
    config = SonareDereverbClassicalConfig(  # noqa: F405
        threshold=float(threshold),
        attenuation=float(attenuation),
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        t60_sec=float(t60_sec),
        late_delay_ms=float(late_delay_ms),
        over_subtraction=float(over_subtraction),
        spectral_floor=float(spectral_floor),
        wpe_enabled=1 if wpe_enabled else 0,
        wpe_iterations=int(wpe_iterations),
        wpe_taps=int(wpe_taps),
        wpe_strength=float(wpe_strength),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_dereverb_classical, samples, sample_rate, config
    )


def mastering_repair_trim_silence(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold: float = 0.001,
    padding_samples: int = 0,
    mode: int | str = "peak",
    gate_lufs: float = -60.0,
    window_ms: float = 400.0,
) -> np.ndarray:
    """Offline silence trimmer (peak threshold or LUFS-gated)."""
    if padding_samples < 0:
        raise ValueError("padding_samples must be non-negative")
    config = SonareTrimSilenceConfig(  # noqa: F405
        threshold=float(threshold),
        padding_samples=int(padding_samples),
        mode=_coerce_trim_silence_mode(mode),
        gate_lufs=float(gate_lufs),
        window_ms=float(window_ms),
    )
    return _run_repair(
        _get_lib().sonare_mastering_repair_trim_silence, samples, sample_rate, config
    )


def trim(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    threshold_db: float = -60.0,
) -> list[float]:
    """Trim silence from the beginning and end of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Silence threshold in dB (default -60.0).

    Returns:
        List of trimmed samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_trim(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(threshold_db),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    result = [float(out[i]) for i in range(out_length.value)]
    if out and out_length.value > 0:
        lib.sonare_free_floats(out)
    return result


# ---------------------------------------------------------------------------
# Offline mastering dynamics processors
# ---------------------------------------------------------------------------


_COMPRESSOR_DETECTOR_NAMES: dict[str, int] = {
    "peak": SONARE_COMPRESSOR_DETECTOR_PEAK,
    "rms": SONARE_COMPRESSOR_DETECTOR_RMS,
    "log_rms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
    "logrms": SONARE_COMPRESSOR_DETECTOR_LOG_RMS,
}


def _coerce_compressor_detector(value: int | str) -> int:
    if isinstance(value, int):
        if value not in _COMPRESSOR_DETECTOR_NAMES.values():
            raise ValueError(f"unknown compressor detector: {value!r}")
        return value
    key = value.strip().lower().replace("-", "_")
    if key not in _COMPRESSOR_DETECTOR_NAMES:
        raise ValueError(f"unknown compressor detector: {value!r}")
    return _COMPRESSOR_DETECTOR_NAMES[key]


def _run_dynamics(
    lib_fn,
    samples,
    sample_rate: int,
    config,
    *,
    fn_name: str = "mastering_dynamics",
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Invoke a dynamics ``(samples, sr, &config, &out, &out_length, &out_latency)``
    C call and return ``(output ndarray, latency_samples)``.
    """
    lib = _get_lib()
    in_buf = _validate_samples(fn_name, samples, validate=validate)
    length = int(in_buf.shape[0])
    c_array = in_buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    latency = ctypes.c_int()
    rc = lib_fn(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(config),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(latency),
    )
    _check(rc)
    try:
        return _from_c_float_array(out, out_length.value), int(latency.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def mastering_dynamics_compressor(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -18.0,
    ratio: float = 2.0,
    attack_ms: float = 10.0,
    release_ms: float = 100.0,
    knee_db: float = 0.0,
    makeup_gain_db: float = 0.0,
    auto_makeup: bool = False,
    detector: int | str = "rms",
    sidechain_hpf_enabled: bool = False,
    sidechain_hpf_hz: float = 100.0,
    pdr_time_ms: float = 0.0,
    pdr_release_scale: float = 1.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline feed-forward compressor.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Compression threshold in dB (default -18).
        ratio: Compression ratio (clamped to >= 1; default 2.0).
        attack_ms: Attack time in milliseconds (default 10).
        release_ms: Release time in milliseconds (default 100).
        knee_db: Soft-knee width in dB (default 0).
        makeup_gain_db: Static makeup gain in dB (default 0).
        auto_makeup: Whether to enable automatic makeup gain (default False).
        detector: Detector mode, either an alias (``"peak"``, ``"rms"``,
            ``"log_rms"``) or an integer from
            :data:`SONARE_COMPRESSOR_DETECTOR_PEAK` and friends (default ``"rms"``).
        sidechain_hpf_enabled: Whether to high-pass-filter the sidechain
            (default False).
        sidechain_hpf_hz: Sidechain HPF cutoff in Hz (default 100).
        pdr_time_ms: Program-dependent release time in ms (default 0).
        pdr_release_scale: PDR release multiplier (default 1.0).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``. The output is a
        ``numpy.ndarray`` of ``float32`` with ``shape == samples.shape``.
    """
    config = SonareCompressorConfig(
        threshold_db=float(threshold_db),
        ratio=float(ratio),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        knee_db=float(knee_db),
        makeup_gain_db=float(makeup_gain_db),
        auto_makeup=1 if auto_makeup else 0,
        detector=_coerce_compressor_detector(detector),
        sidechain_hpf_enabled=1 if sidechain_hpf_enabled else 0,
        sidechain_hpf_hz=float(sidechain_hpf_hz),
        pdr_time_ms=float(pdr_time_ms),
        pdr_release_scale=float(pdr_release_scale),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_compressor,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_compressor",
        validate=validate,
    )


def mastering_dynamics_gate(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    threshold_db: float = -50.0,
    attack_ms: float = 2.0,
    release_ms: float = 80.0,
    range_db: float = -80.0,
    hold_ms: float = 0.0,
    close_threshold_db: float = -50.0,
    key_hpf_hz: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline noise gate.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        threshold_db: Open-state threshold in dB (default -50).
        attack_ms: Attack time in milliseconds (default 2).
        release_ms: Release time in milliseconds (default 80).
        range_db: Closed-state attenuation in dB (default -80).
        hold_ms: Minimum open time in milliseconds (default 0).
        close_threshold_db: Hysteresis close-threshold in dB; clamped to
            ``<= threshold_db`` (default -50).
        key_hpf_hz: Sidechain HPF cutoff in Hz (default 0 = disabled).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareGateConfig(
        threshold_db=float(threshold_db),
        attack_ms=float(attack_ms),
        release_ms=float(release_ms),
        range_db=float(range_db),
        hold_ms=float(hold_ms),
        close_threshold_db=float(close_threshold_db),
        key_hpf_hz=float(key_hpf_hz),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_gate,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_gate",
        validate=validate,
    )


def mastering_dynamics_transient_shaper(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int = 22050,
    *,
    attack_gain_db: float = 3.0,
    sustain_gain_db: float = 0.0,
    fast_attack_ms: float = 0.0,
    fast_release_ms: float = 20.0,
    slow_attack_ms: float = 15.0,
    slow_release_ms: float = 200.0,
    sensitivity: float = 1.0,
    max_gain_db: float = 12.0,
    gain_smoothing_ms: float = 0.0,
    lookahead_ms: float = 0.0,
    validate: bool = True,
) -> tuple[np.ndarray, int]:
    """Apply the offline envelope-difference transient shaper.

    Args:
        samples: Mono input buffer (any sequence convertible to float32).
        sample_rate: Sample rate in Hz (default 22050).
        attack_gain_db: Attack-band gain in dB (default +3).
        sustain_gain_db: Sustain-band gain in dB (default 0).
        fast_attack_ms: Fast-envelope attack time in ms (default 0).
        fast_release_ms: Fast-envelope release time in ms (default 20).
        slow_attack_ms: Slow-envelope attack time in ms (default 15).
        slow_release_ms: Slow-envelope release time in ms (default 200).
        sensitivity: Envelope difference sensitivity (clamped >= 0; default 1).
        max_gain_db: Safety clamp on applied gain in dB (default 12).
        gain_smoothing_ms: Gain-signal smoothing time in ms
            (default 0 = disabled).
        lookahead_ms: Lookahead time in ms (default 0 = disabled).

    Returns:
        Tuple of ``(output ndarray, latency_samples)``.
    """
    config = SonareTransientShaperConfig(
        attack_gain_db=float(attack_gain_db),
        sustain_gain_db=float(sustain_gain_db),
        fast_attack_ms=float(fast_attack_ms),
        fast_release_ms=float(fast_release_ms),
        slow_attack_ms=float(slow_attack_ms),
        slow_release_ms=float(slow_release_ms),
        sensitivity=float(sensitivity),
        max_gain_db=float(max_gain_db),
        gain_smoothing_ms=float(gain_smoothing_ms),
        lookahead_ms=float(lookahead_ms),
    )
    return _run_dynamics(
        _get_lib().sonare_mastering_dynamics_transient_shaper,
        samples,
        sample_rate,
        config,
        fn_name="mastering_dynamics_transient_shaper",
        validate=validate,
    )
