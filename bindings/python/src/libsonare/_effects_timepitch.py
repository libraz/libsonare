"""Time-scale, pitch-shift and pitch-correction wrappers."""

from __future__ import annotations

import ctypes
from collections.abc import Mapping, Sequence
from typing import cast

import numpy as np

from ._analysis_music import _key_pair
from ._errors import _entry, _refuse
from ._features_metering import scale_mask_for_mode
from ._ffi import (
    SONARE_PITCH_TARGET_FIXED_MIDI,
    SONARE_PITCH_TARGET_SCALE,
    SonareKey,
    SonarePitchCorrectionConfig,
)
from ._narrowing import _narrow_double
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _DEFAULT_EFFECT_N_FFT,
    _PITCH_TARGET_MODES,
    _UINT32_MAX,
    _call_float_array_transform,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _key_mode_token,
    _mode_value,
    _narrow_int,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _token_arg,
    _validate_effect_hop,
    _validate_samples,
    _validate_stft_n_fft,
)
from ._types_analysis import AutoTuneResult, Key
from ._types_enums import KeyMode, PitchClass, PitchTargetMode


@_entry("time_stretch")
def time_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    speed_ratio: float,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int | None = None,
) -> np.ndarray:
    """Time-stretch audio without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        speed_ratio: Playback speed: ``>1`` is faster and shorter, ``<1`` slower
            and longer; required. The opposite sense of :func:`note_stretch`'s
            ``stretch_ratio``, a duration ratio.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 4
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window. Default: ``n_fft / 4``,
            derived by the library.

    Returns:
        ``float32`` array of time-stretched samples.
    """
    _validate_samples(samples)
    n_fft = _validate_stft_n_fft(n_fft)
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    return _call_float_array_transform(
        "sonare_time_stretch_ex",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(speed_ratio, "speed_ratio"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


@_entry("pitch_shift")
def pitch_shift(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    pitch_semitones: float,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int | None = None,
) -> np.ndarray:
    """Shift the pitch of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        pitch_semitones: Semitones to shift (positive = up, negative = down); required.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 4
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window. Default: ``n_fft / 4``,
            derived by the library.

    Returns:
        ``float32`` array of pitch-shifted samples.
    """
    _validate_samples(samples)
    n_fft = _validate_stft_n_fft(n_fft)
    hop_length = 0 if hop_length is None else _validate_effect_hop(hop_length)
    return _call_float_array_transform(
        "sonare_pitch_shift_ex",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(pitch_semitones, "pitch_semitones"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


@_entry("pitch_correct_to_midi")
@_guard_buffer("samples")
def pitch_correct_to_midi(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    current_midi: float = 69.0,
    target_midi: float = 69.0,
) -> np.ndarray:
    """Pitch-correct audio from a current MIDI note to a target MIDI note.

    Applies one constant, immediate transpose with no retune glide and preserves
    the input buffer length. The whole interval is applied however large it is:
    both endpoints are validated to [0, 127], so a two-octave move such as
    C3 -> C5 transposes by the full 24 semitones. Use
    :func:`pitch_correct_timevarying` for a caller-supplied pitch contour.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz.
        current_midi: Detected/current pitch as a MIDI note number.
        target_midi: Desired pitch as a MIDI note number.

    Returns:
        ``float32`` array of pitch-corrected samples.
    """
    return _call_float_array_transform(
        "sonare_pitch_correct_to_midi",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(current_midi, "current_midi"),
        _to_c_float(target_midi, "target_midi"),
    )


@_entry("pitch_correct_timevarying")
@_guard_buffer("samples")
def pitch_correct_timevarying(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    f0_hz: Sequence[float] | list[float],
    hop_length: int = 512,
    *,
    target_mode: PitchTargetMode = "midi",
    target_midi: float = 69.0,
    scale_root: int = 0,
    scale_mode_mask: int | KeyMode | None = None,
    scale_reference_midi: float | None = None,
    retune_amount: float | None = None,
    max_correction_semitones: float | None = None,
    retune_speed_ms: float | None = None,
    vibrato_threshold_cents: float | None = None,
    voiced: Sequence[int] | list[int] | None = None,
    voiced_prob: Sequence[float] | list[float] | None = None,
) -> np.ndarray:
    """Contour-following pitch correction toward a fixed MIDI note OR a scale.

    Follows the caller-supplied per-frame ``f0_hz`` contour, so vibrato and
    drift in the source are tracked rather than flattened (unlike the constant
    transpose of :func:`pitch_correct_to_midi`). ``target_mode`` selects a
    fixed-MIDI target (``"midi"``) or scale quantisation (``"scale"``), and the
    retune knobs shape natural-vs-robotic correction.

    Args:
        samples: Audio samples.
        f0_hz: Per-frame measured F0 in Hz (one entry per analysis frame).
            Unvoiced frames may use ``NaN`` when the corresponding ``voiced``
            flag is zero, matching pYIN's default output.
        sample_rate: Sample rate in Hz.
        hop_length: F0 hop in samples; frame ``i`` covers sample ``i*hop_length``.
        target_mode: ``"midi"`` retunes toward ``target_midi``; ``"scale"`` snaps
            to the scale.
        target_midi: Fixed target note when ``target_mode == "midi"`` (in ``[0, 127]``).
        scale_root: Scale root pitch class (0=C .. 11=B) when ``target_mode == "scale"``.
        scale_mode_mask: 12-bit degree mask, or a mode token
            (``"major"``, ``"dorian"``, ...) resolved by :func:`scale_mask_for_mode`;
            ``None`` keeps the library default (C major).
        scale_reference_midi: Reference MIDI anchoring the scale grid; ``None`` keeps
            the default.
        retune_amount: Correction strength in ``[0, 1]``; ``None`` keeps the default (1.0).
        max_correction_semitones: Per-frame correction clamp; ``None`` keeps the default.
        retune_speed_ms: Retune IIR time constant (ms); ``None`` keeps the default.
        vibrato_threshold_cents: Vibrato-preserve threshold; ``None`` keeps the default.
        voiced: Optional per-frame voiced flags (non-zero = voiced).
        voiced_prob: Optional per-frame voicing probability in ``[0, 1]``. Used
            only to derive voicing when ``voiced`` is ``None``; never a weight
            on the correction amount.

    Returns:
        ``float32`` array of pitch-corrected samples.
    """
    target = _token_arg(target_mode, _PITCH_TARGET_MODES, "target_mode")
    lib = _get_lib()

    config = SonarePitchCorrectionConfig()
    _check(lib.sonare_pitch_correction_config_default(ctypes.byref(config)))
    config.target_mode = (
        SONARE_PITCH_TARGET_SCALE if target == "scale" else SONARE_PITCH_TARGET_FIXED_MIDI
    )
    config.target_midi = _narrow_double(target_midi, "target_midi")
    # Narrowed rather than coerced: int() takes 3.7 as 3, a root and a degree set
    # the caller never asked for.
    config.scale_root = _narrow_int(scale_root, "scale_root", _C_INT_MIN, _C_INT_MAX)
    if isinstance(scale_mode_mask, str):
        config.scale_mode_mask = scale_mask_for_mode(0, scale_mode_mask)
    elif scale_mode_mask is not None:
        config.scale_mode_mask = _narrow_int(scale_mode_mask, "scale_mode_mask", 0, _UINT32_MAX)
    if scale_reference_midi is not None:
        config.scale_reference_midi = _narrow_double(scale_reference_midi, "scale_reference_midi")
    if retune_amount is not None:
        config.retune_amount = _narrow_double(retune_amount, "retune_amount")
    if max_correction_semitones is not None:
        config.max_correction_semitones = _narrow_double(
            max_correction_semitones, "max_correction_semitones"
        )
    if retune_speed_ms is not None:
        config.retune_speed_ms = _narrow_double(retune_speed_ms, "retune_speed_ms")
    if vibrato_threshold_cents is not None:
        config.vibrato_threshold_cents = _narrow_double(
            vibrato_threshold_cents, "vibrato_threshold_cents"
        )

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    voiced_array = None
    if voiced is not None:
        # Bulk-marshalled through NumPy like every other buffer on this path;
        # `(c_int32 * n)(*seq)` unpacks each frame through Python varargs, which
        # is the one per-element hop left in an otherwise vectorised call.
        voiced_array, voiced_len = _to_c_int_array(voiced, "voiced")
        if voiced_len != n_frames:
            raise _refuse("voiced must have the same length as f0_hz")
    prob_array = None
    if voiced is None and voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise _refuse("voiced_prob must have the same length as f0_hz")
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_pitch_correct_timevarying(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                f0_array,
                prob_array,
                voiced_array,
                _to_c_size_t(n_frames, "n_frames"),
                _to_c_int(hop_length, "hop_length"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            ),
            flattened="config",
        )
        return _from_c_float_array(out, out_length.value)


def _auto_tune_key(key: object) -> SonareKey | None:
    """Resolves ``key`` to the C key, or ``None`` for ``"detect"``."""
    if isinstance(key, str):
        if key != "detect":
            raise _refuse("key must be 'detect' or a key with a root and a mode")
        return None
    root, mode = _key_pair(key)
    return SonareKey(
        int(PitchClass(cast(int, root))), _mode_value(cast(KeyMode, mode), "key.mode"), 1.0
    )


@_entry("auto_tune")
@_guard_buffer("samples")
def auto_tune(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    key: str | Key | Mapping[str, object] | tuple[PitchClass, KeyMode] = "detect",
    *,
    retune_amount: float | None = None,
    retune_speed_ms: float | None = None,
    vibrato_threshold_cents: float | None = None,
    max_correction_semitones: float | None = None,
    scale_reference_midi: float | None = None,
) -> AutoTuneResult:
    """Snap the voiced pitch of a monophonic recording to a scale, offline.

    Chains key detection (when ``key`` is ``"detect"``), :func:`scale_mask_for_mode`,
    pYIN pitch tracking (2048-sample frames, 512-sample hop) and the time-varying
    scale corrector, so the knobs are :func:`pitch_correct_timevarying`'s. The output has
    the input's length; unvoiced stretches pass through unchanged.

    Args:
        samples: Mono audio samples.
        sample_rate: Sample rate in Hz.
        key: ``"detect"`` finds the key with :func:`detect_key`'s defaults; or name
            it with a :class:`Key` (e.g. :func:`detect_key`'s result), a
            ``{"root": ..., "mode": ...}`` mapping or a ``(root, mode)`` pair, the
            mode a :data:`KeyMode` token.
        retune_amount: Correction strength in ``[0, 1]``; ``None`` keeps the default (1.0).
        retune_speed_ms: Retune IIR time constant (ms); ``None`` keeps the default.
        vibrato_threshold_cents: Vibrato-preserve threshold; ``None`` keeps the default.
        max_correction_semitones: Per-frame correction clamp; ``None`` keeps the default.
        scale_reference_midi: Reference MIDI anchoring the scale grid; ``None`` keeps
            the default.

    Returns:
        :class:`AutoTuneResult` with the corrected ``samples`` and the ``key`` used.
    """
    lib = _get_lib()

    named = _auto_tune_key(key)
    config = SonarePitchCorrectionConfig()
    _check(lib.sonare_pitch_correction_config_default(ctypes.byref(config)))
    if retune_amount is not None:
        config.retune_amount = _narrow_double(retune_amount, "retune_amount")
    if scale_reference_midi is not None:
        config.scale_reference_midi = _narrow_double(scale_reference_midi, "scale_reference_midi")
    if max_correction_semitones is not None:
        config.max_correction_semitones = _narrow_double(
            max_correction_semitones, "max_correction_semitones"
        )
    if retune_speed_ms is not None:
        config.retune_speed_ms = _narrow_double(retune_speed_ms, "retune_speed_ms")
    if vibrato_threshold_cents is not None:
        config.vibrato_threshold_cents = _narrow_double(
            vibrato_threshold_cents, "vibrato_threshold_cents"
        )

    c_array, length = _to_c_float_array(samples)
    used = SonareKey()
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_auto_tune(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(named) if named is not None else None,
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
                ctypes.byref(used),
            ),
            flattened="config",
        )
        return AutoTuneResult(
            samples=_from_c_float_array(out, out_length.value),
            key=Key(PitchClass(used.root), _key_mode_token(used.mode), float(used.confidence)),
        )
