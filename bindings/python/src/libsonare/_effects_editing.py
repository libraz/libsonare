"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import ctypes
import dataclasses
from collections.abc import Sequence

from ._ffi import (
    SONARE_PITCH_TARGET_FIXED_MIDI,
    SONARE_PITCH_TARGET_SCALE,
    SONARE_SPECTRAL_EDIT_MODE_ATTENUATE,
    SONARE_SPECTRAL_EDIT_MODE_GAIN,
    SONARE_SPECTRAL_EDIT_MODE_HEAL,
    SONARE_SPECTRAL_EDIT_MODE_MUTE,
    SonareHpssResult,
    SonarePitchCorrectionConfig,
    SonareSpectralEditConfig,
    SonareSpectralRegionOp,
)
from ._runtime import (
    _call_float_transform,
    _check,
    _float_array_result,
    _get_lib,
    _out_float_array,
    _require_power_of_two,
    _resolve_enum,
    _to_c_float_array,
    _validate_samples,
)
from .types import HpssResult


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
    *,
    validate: bool = True,
) -> list[float]:
    """Extract the harmonic component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of harmonic component samples.
    """
    _validate_samples("harmonic", samples, validate=validate)
    return _call_float_transform("sonare_harmonic", samples, ctypes.c_int(sample_rate))


def percussive(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    validate: bool = True,
) -> list[float]:
    """Extract the percussive component of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of percussive component samples.
    """
    _validate_samples("percussive", samples, validate=validate)
    return _call_float_transform("sonare_percussive", samples, ctypes.c_int(sample_rate))


def time_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    rate: float = 1.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Time-stretch audio without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        rate: Stretch factor (>1 speeds up, <1 slows down).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of time-stretched samples.
    """
    _validate_samples("time_stretch", samples, validate=validate)
    return _call_float_transform(
        "sonare_time_stretch", samples, ctypes.c_int(sample_rate), ctypes.c_float(rate)
    )


def pitch_shift(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    semitones: float = 0.0,
    *,
    validate: bool = True,
) -> list[float]:
    """Shift the pitch of audio.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        semitones: Number of semitones to shift (positive = up, negative = down).
        validate: Reject empty / NaN / Inf input (default True). Pass
            ``validate=False`` to skip the scan on hot paths.

    Returns:
        List of pitch-shifted samples.
    """
    _validate_samples("pitch_shift", samples, validate=validate)
    return _call_float_transform(
        "sonare_pitch_shift", samples, ctypes.c_int(sample_rate), ctypes.c_float(semitones)
    )


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
    return _call_float_transform(
        "sonare_pitch_correct_to_midi",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_float(current_midi),
        ctypes.c_float(target_midi),
    )


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
        target_midi: Desired pitch as a MIDI note number.
        sample_rate: Sample rate in Hz (default 22050).
        hop_length: F0 hop in samples; frame ``i`` covers sample ``i*hop_length``.
        voiced: Optional per-frame voiced flags (non-zero = voiced); ``None``
            treats every frame as voiced.
        voiced_prob: Optional per-frame voicing probability in ``[0, 1]``;
            ``None`` derives it from ``voiced`` (1.0 / 0.0).

    Returns:
        List of pitch-corrected samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_pitch_correct_to_midi_timevarying"):
        raise RuntimeError("libsonare was built without pitch-editor support")
    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob)
        if prob_len != n_frames:
            raise ValueError("voiced_prob must have the same length as f0_hz")
    voiced_array = None
    if voiced is not None:
        voiced_seq = [int(v) for v in voiced]
        if len(voiced_seq) != n_frames:
            raise ValueError("voiced must have the same length as f0_hz")
        voiced_array = (ctypes.c_int32 * n_frames)(*voiced_seq)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_pitch_correct_to_midi_timevarying(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                f0_array,
                prob_array,
                voiced_array,
                ctypes.c_size_t(n_frames),
                ctypes.c_int(hop_length),
                ctypes.c_float(target_midi),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


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
        voiced_prob: Optional per-frame voicing probability in ``[0, 1]``.

    Returns:
        List of pitch-corrected samples.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_pitch_correct_timevarying"):
        raise RuntimeError("libsonare was built without pitch-editor support")

    config = SonarePitchCorrectionConfig()
    _check(lib.sonare_pitch_correction_config_default(ctypes.byref(config)))
    config.target_mode = (
        SONARE_PITCH_TARGET_SCALE if mode == "scale" else SONARE_PITCH_TARGET_FIXED_MIDI
    )
    config.target_midi = float(target_midi)
    config.scale_root = int(scale_root)
    if scale_mode_mask is not None:
        config.scale_mode_mask = int(scale_mode_mask) & 0x0FFF
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
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob)
        if prob_len != n_frames:
            raise ValueError("voiced_prob must have the same length as f0_hz")
    voiced_array = None
    if voiced is not None:
        voiced_seq = [int(v) for v in voiced]
        if len(voiced_seq) != n_frames:
            raise ValueError("voiced must have the same length as f0_hz")
        voiced_array = (ctypes.c_int32 * n_frames)(*voiced_seq)
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_pitch_correct_timevarying(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                f0_array,
                prob_array,
                voiced_array,
                ctypes.c_size_t(n_frames),
                ctypes.c_int(hop_length),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)


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
    return _call_float_transform(
        "sonare_note_stretch",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(onset_sample),
        ctypes.c_int(offset_sample),
        ctypes.c_float(stretch_ratio),
    )


_SPECTRAL_EDIT_MODE_NAMES: dict[str, int] = {
    "gain": SONARE_SPECTRAL_EDIT_MODE_GAIN,
    "attenuate": SONARE_SPECTRAL_EDIT_MODE_ATTENUATE,
    "mute": SONARE_SPECTRAL_EDIT_MODE_MUTE,
    "heal": SONARE_SPECTRAL_EDIT_MODE_HEAL,
}

_SPECTRAL_EDIT_WINDOW_NAMES: dict[str, int] = {
    "hann": 0,
    "hamming": 1,
    "blackman": 2,
    "rectangular": 3,
}


def _coerce_spectral_edit_mode(value: int | str) -> int:
    return _resolve_enum(
        value,
        _SPECTRAL_EDIT_MODE_NAMES,
        "spectral edit mode",
        underscore=True,
        strip=True,
        validate_int=True,
        reject_bool=True,
        quote_value=True,
    )


def _coerce_spectral_edit_window(value: int | str) -> int:
    return _resolve_enum(
        value,
        _SPECTRAL_EDIT_WINDOW_NAMES,
        "spectral edit window",
        underscore=True,
        strip=True,
        validate_int=True,
        reject_bool=True,
        quote_value=True,
    )


@dataclasses.dataclass
class SpectralRegionOp:
    """One time x frequency rectangle edit op for :func:`spectral_edit`.

    Mirrors :class:`SonareSpectralRegionOp`. ``mode`` is one of ``"gain"``,
    ``"attenuate"``, ``"mute"`` or ``"heal"`` (or the matching integer enum
    value from ``SONARE_SPECTRAL_EDIT_MODE_*``).

    An omitted ``end_sample`` (left at the ``-1`` sentinel) spans to the end of
    the signal, matching the Node/WASM facades where ``endSample`` defaults to
    the signal length.
    """

    start_sample: int = 0
    end_sample: int = -1
    low_hz: float = 0.0
    high_hz: float = 0.0
    gain_db: float = 0.0
    mode: str | int = "gain"


def spectral_edit(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    ops: Sequence[SpectralRegionOp],
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    window: str | int = "hann",
    heal_radius_frames: int = 2,
) -> list[float]:
    """Region-based spectral editing (STFT -> per-op bin/frame masking -> iSTFT).

    Stateless mono transform; the output has the same length and sample rate as
    the input. Each entry of ``ops`` is a :class:`SpectralRegionOp` describing a
    time x frequency rectangle that is applied in order. An empty ``ops`` list is
    the identity transform.

    Args:
        samples: Audio samples (mono).
        sample_rate: Sample rate in Hz.
        ops: Sequence of :class:`SpectralRegionOp` region edits, applied in order.
        n_fft: STFT size; must be a power of two (default 2048).
        hop_length: STFT hop; must satisfy ``0 < hop_length <= n_fft / 2``
            (default 512).
        window: Analysis window, one of ``"hann"``, ``"hamming"``,
            ``"blackman"``, ``"rectangular"`` (or the matching integer enum;
            default ``"hann"``).
        heal_radius_frames: Neighbour frames each side used by ``"heal"`` mode
            (default 2).

    Returns:
        List of edited samples.

    Note:
        ``n_fft``/``hop_length``/``heal_radius_frames`` are validated eagerly here
        and raise :class:`ValueError`, the idiomatic Python contract. The Node and
        WASM surfaces accept the same valid inputs but delegate rejection to the
        C++ core, so an invalid value surfaces as a ``SonareError`` there. The
        accepted input range is identical across surfaces.
    """
    _require_power_of_two(n_fft, "n_fft")
    if hop_length <= 0 or hop_length > n_fft // 2:
        raise ValueError("hop_length must satisfy 0 < hop_length <= n_fft / 2")
    if heal_radius_frames < 0:
        raise ValueError("heal_radius_frames must be non-negative")
    window_value = _coerce_spectral_edit_window(window)

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareSpectralEditConfig(
        n_fft=int(n_fft),
        hop_length=int(hop_length),
        window=int(window_value),
        heal_radius_frames=int(heal_radius_frames),
    )

    n_ops = len(ops)
    c_ops = None
    if n_ops > 0:
        c_ops = (SonareSpectralRegionOp * n_ops)()
        for i, op in enumerate(ops):
            # An omitted end_sample (-1 sentinel) spans to the end of the signal,
            # matching the Node/WASM facades; the core clamps to [0, length].
            end_sample = int(op.end_sample) if op.end_sample >= 0 else length
            c_ops[i] = SonareSpectralRegionOp(
                start_sample=int(op.start_sample),
                end_sample=end_sample,
                low_hz=float(op.low_hz),
                high_hz=float(op.high_hz),
                gain_db=float(op.gain_db),
                mode=_coerce_spectral_edit_mode(op.mode),
            )

    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_spectral_edit(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                ctypes.byref(config),
                c_ops,
                ctypes.c_size_t(n_ops),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)
