"""Time-scale, pitch-shift and pitch-correction wrappers."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SONARE_PITCH_TARGET_FIXED_MIDI,
    SONARE_PITCH_TARGET_SCALE,
    SonarePitchCorrectionConfig,
)
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _DEFAULT_EFFECT_HOP_LENGTH,
    _DEFAULT_EFFECT_N_FFT,
    _UINT32_MAX,
    SonareValueError,
    _call_float_transform,
    _check,
    _float_array_result,
    _get_lib,
    _guard_buffer,
    _narrow_int,
    _out_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int_array,
    _to_c_size_t,
    _unsupported_effect_symbol,
    _validate_effect_fft_options,
    _validate_samples,
)


def time_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    rate: float = 1.0,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    *,
    validate: bool = True,
) -> list[float]:
    """Time-stretch audio without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        rate: Stretch factor (>1 speeds up, <1 slows down).
        n_fft: FFT size used for analysis/synthesis; an even integer >= 2
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window (default 512).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of time-stretched samples.
    """
    _validate_samples("time_stretch", samples, validate=validate)
    n_fft, hop_length = _validate_effect_fft_options("time_stretch", n_fft, hop_length)
    lib = _get_lib()
    if not hasattr(lib, "sonare_time_stretch_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != _DEFAULT_EFFECT_HOP_LENGTH:
            raise _unsupported_effect_symbol("sonare_time_stretch_ex")
        if not hasattr(lib, "sonare_time_stretch"):
            raise _unsupported_effect_symbol("sonare_time_stretch")
        return _call_float_transform(
            "sonare_time_stretch",
            samples,
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_float(rate, "rate"),
        )
    return _call_float_transform(
        "sonare_time_stretch_ex",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(rate, "rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


def pitch_shift(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    semitones: float = 0.0,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    *,
    validate: bool = True,
) -> list[float]:
    """Shift the pitch of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        semitones: Number of semitones to shift (positive = up, negative = down).
        n_fft: FFT size used for analysis/synthesis; an even integer >= 2
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window (default 512).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of pitch-shifted samples.
    """
    _validate_samples("pitch_shift", samples, validate=validate)
    n_fft, hop_length = _validate_effect_fft_options("pitch_shift", n_fft, hop_length)
    lib = _get_lib()
    if not hasattr(lib, "sonare_pitch_shift_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != _DEFAULT_EFFECT_HOP_LENGTH:
            raise _unsupported_effect_symbol("sonare_pitch_shift_ex")
        if not hasattr(lib, "sonare_pitch_shift"):
            raise _unsupported_effect_symbol("sonare_pitch_shift")
        return _call_float_transform(
            "sonare_pitch_shift",
            samples,
            _to_c_int(sample_rate, "sample_rate"),
            _to_c_float(semitones, "semitones"),
        )
    return _call_float_transform(
        "sonare_pitch_shift_ex",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(semitones, "semitones"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
    )


@_guard_buffer("samples")
def pitch_correct_to_midi(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    current_midi: float = 69.0,
    target_midi: float = 69.0,
) -> list[float]:
    """Pitch-correct audio from a current MIDI note to a target MIDI note.

    Applies one constant, immediate transpose with no retune glide and preserves
    the input buffer length. The whole interval is applied however large it is:
    both endpoints are validated to [0, 127], so a two-octave move such as
    C3 -> C5 transposes by the full 24 semitones. Use
    :func:`pitch_correct_to_midi_timevarying` for a caller-supplied pitch
    contour.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        current_midi: Detected/current pitch as a MIDI note number.
        target_midi: Desired pitch as a MIDI note number.

    Returns:
        List of pitch-corrected samples.
    """
    return _call_float_transform(
        "sonare_pitch_correct_to_midi",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(current_midi, "current_midi"),
        _to_c_float(target_midi, "target_midi"),
    )


@_guard_buffer("samples")
def pitch_correct_to_midi_timevarying(
    samples: Sequence[float] | list[float],
    f0_hz: Sequence[float] | list[float],
    target_midi: float,
    sample_rate: int = 22050,
    hop_length: int = 512,
    voiced: Sequence[int] | list[int] | None = None,
    voiced_prob: Sequence[float] | list[float] | None = None,
) -> list[float]:
    """Contour-following ("time-varying") pitch correction toward a MIDI target.

    Unlike :func:`pitch_correct_to_midi` (a single constant transpose), this
    follows the caller-supplied per-frame ``f0_hz`` contour and retunes every
    voiced frame toward ``target_midi``, so vibrato/drift in the source is
    tracked rather than flattened.

    Args:
        samples: Audio samples.
        f0_hz: Per-frame measured F0 in Hz (one entry per analysis frame).
            Unvoiced frames may use ``NaN`` when the corresponding ``voiced``
            flag is zero, matching pYIN's default output.
        target_midi: Desired pitch as a MIDI note number.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: F0 hop in samples; frame ``i`` covers sample ``i*hop_length``.
        voiced: Optional per-frame voiced flags (non-zero = voiced); ``None``
            treats every frame as voiced.
        voiced_prob: Optional per-frame voicing probability in ``[0, 1]``. Used
            ONLY to derive voicing when ``voiced`` is ``None`` (>= 0.5 is
            voiced); ignored entirely when ``voiced`` is supplied. It never
            scales the correction amount, so passing :func:`pitch_pyin`'s
            ``voiced_prob`` (a frequency-dependent observation mass, not a
            confidence) gives the same result as omitting it.

    Returns:
        List of pitch-corrected samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_pitch_correct_to_midi_timevarying"):
        raise RuntimeError("libsonare was built without pitch-editor support")
    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise SonareValueError("voiced_prob must have the same length as f0_hz")
    voiced_array = None
    if voiced is not None:
        # Bulk-marshalled through NumPy like every other buffer on this path;
        # `(c_int32 * n)(*seq)` unpacks each frame through Python varargs, which
        # is the one per-element hop left in an otherwise vectorised call.
        voiced_array, voiced_len = _to_c_int_array(voiced, "voiced")
        if voiced_len != n_frames:
            raise SonareValueError("voiced must have the same length as f0_hz")
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_pitch_correct_to_midi_timevarying(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                f0_array,
                prob_array,
                voiced_array,
                _to_c_size_t(n_frames, "n_frames"),
                _to_c_int(hop_length, "hop_length"),
                _to_c_float(target_midi, "target_midi"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


@_guard_buffer("samples")
def pitch_correct_timevarying(
    samples: Sequence[float] | list[float],
    f0_hz: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    *,
    mode: str = "midi",
    target_midi: float = 69.0,
    scale_root: int = 0,
    scale_mode_mask: int | None = None,
    reference_midi: float | None = None,
    retune_amount: float | None = None,
    max_correction_semitones: float | None = None,
    retune_speed_ms: float | None = None,
    vibrato_threshold_cents: float | None = None,
    voiced: Sequence[int] | list[int] | None = None,
    voiced_prob: Sequence[float] | list[float] | None = None,
) -> list[float]:
    """Contour-following pitch correction toward a fixed MIDI note OR a scale.

    Generalises :func:`pitch_correct_to_midi_timevarying`: the same
    caller-supplied per-frame ``f0_hz`` contour drives correction, but ``mode``
    selects between a fixed-MIDI target (``"midi"``) and scale quantisation
    (``"scale"``), and the retune knobs shape natural-vs-robotic correction.

    Args:
        samples: Audio samples.
        f0_hz: Per-frame measured F0 in Hz (one entry per analysis frame).
            Unvoiced frames may use ``NaN`` when the corresponding ``voiced``
            flag is zero, matching pYIN's default output.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: F0 hop in samples; frame ``i`` covers sample ``i*hop_length``.
        mode: ``"midi"`` retunes toward ``target_midi``; ``"scale"`` snaps to the key.
        target_midi: Fixed target note when ``mode == "midi"`` (in ``[0, 127]``).
        scale_root: Scale root pitch class (0=C .. 11=B) when ``mode == "scale"``.
        scale_mode_mask: 12-bit degree mask; ``None`` keeps the library default (C major).
        reference_midi: Reference MIDI anchoring the scale grid; ``None`` keeps the default.
        retune_amount: Correction strength in ``[0, 1]``; ``None`` keeps the default (1.0).
        max_correction_semitones: Per-frame correction clamp; ``None`` keeps the default.
        retune_speed_ms: Retune IIR time constant (ms); ``None`` keeps the default.
        vibrato_threshold_cents: Vibrato-preserve threshold; ``None`` keeps the default.
        voiced: Optional per-frame voiced flags (non-zero = voiced).
        voiced_prob: Optional per-frame voicing probability in ``[0, 1]``. Used
            only to derive voicing when ``voiced`` is ``None``; never a weight
            on the correction amount.

    Returns:
        List of pitch-corrected samples.
    """
    if mode not in ("midi", "scale"):
        raise SonareValueError("mode must be 'midi' or 'scale'")
    lib = _get_lib()
    if not hasattr(lib, "sonare_pitch_correct_timevarying"):
        raise RuntimeError("libsonare was built without pitch-editor support")

    config = SonarePitchCorrectionConfig()
    _check(lib.sonare_pitch_correction_config_default(ctypes.byref(config)))
    config.target_mode = (
        SONARE_PITCH_TARGET_SCALE if mode == "scale" else SONARE_PITCH_TARGET_FIXED_MIDI
    )
    config.target_midi = float(target_midi)
    # Narrowed rather than coerced: int() takes 3.7 as 3, a root and a degree set
    # the caller never asked for.
    config.scale_root = _narrow_int(
        scale_root, "pitch_correct_timevarying: scale_root", _C_INT_MIN, _C_INT_MAX
    )
    if scale_mode_mask is not None:
        config.scale_mode_mask = _narrow_int(
            scale_mode_mask, "pitch_correct_timevarying: scale_mode_mask", 0, _UINT32_MAX
        )
    if reference_midi is not None:
        config.scale_reference_midi = float(reference_midi)
    if retune_amount is not None:
        config.retune_amount = float(retune_amount)
    if max_correction_semitones is not None:
        config.max_correction_semitones = float(max_correction_semitones)
    if retune_speed_ms is not None:
        config.retune_speed_ms = float(retune_speed_ms)
    if vibrato_threshold_cents is not None:
        config.vibrato_threshold_cents = float(vibrato_threshold_cents)

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise SonareValueError("voiced_prob must have the same length as f0_hz")
    voiced_array = None
    if voiced is not None:
        # Bulk-marshalled through NumPy like every other buffer on this path;
        # `(c_int32 * n)(*seq)` unpacks each frame through Python varargs, which
        # is the one per-element hop left in an otherwise vectorised call.
        voiced_array, voiced_len = _to_c_int_array(voiced, "voiced")
        if voiced_len != n_frames:
            raise SonareValueError("voiced must have the same length as f0_hz")
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
            )
        )
        return _float_array_result(out, out_length.value)
