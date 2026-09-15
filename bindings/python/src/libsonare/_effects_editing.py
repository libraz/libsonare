"""Audio effect wrappers for libsonare."""

from __future__ import annotations

import ctypes
import dataclasses
from collections.abc import Sequence
from numbers import Integral

import numpy as np

from ._ffi import (
    SONARE_PITCH_TARGET_FIXED_MIDI,
    SONARE_PITCH_TARGET_SCALE,
    SONARE_SPECTRAL_EDIT_MODE_ATTENUATE,
    SONARE_SPECTRAL_EDIT_MODE_GAIN,
    SONARE_SPECTRAL_EDIT_MODE_HEAL,
    SONARE_SPECTRAL_EDIT_MODE_MUTE,
    SonareHpssResult,
    SonareNoteEdit,
    SonareNoteExtractorConfig,
    SonareNoteObject,
    SonareNoteObjectsResult,
    SonareNoteRenderConfig,
    SonarePercussiveEvent,
    SonarePercussiveEventConfig,
    SonarePercussiveEventEdit,
    SonarePercussiveEventsResult,
    SonarePercussiveRenderConfig,
    SonarePitchCorrectionConfig,
    SonarePitchDecompositionResult,
    SonareSpectralEditConfig,
    SonareSpectralRegionOp,
)
from ._runtime import (
    ErrorCode,
    SonareError,
    SonareValueError,
    _call_float_transform,
    _check,
    _float_array_result,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _out_float_array,
    _require_power_of_two,
    _resolve_enum,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_int32,
    _to_c_int_array,
    _to_c_size_t,
    _validate_c_int_field,
    _validate_effect_fft_options,
    _validate_hpss_kernel,
    _validate_samples,
)
from .types import HpssResult

_DEFAULT_EFFECT_N_FFT = 2048
_DEFAULT_EFFECT_HOP_LENGTH = 512

# Both note-object configs are at layout version 1; 0 selects the same layout.
_NOTE_STRUCT_VERSION = 1

# The two percussive-event configs are versioned separately from the note-object
# ones, and are likewise at layout version 1.
_PERCUSSIVE_STRUCT_VERSION = 1


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return SonareError(
        int(ErrorCode.NOT_SUPPORTED),
        f"libsonare does not export {symbol}; install a matching native library",
    )


def hpss(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    kernel_harmonic: int = 31,
    kernel_percussive: int = 31,
    n_fft: int = _DEFAULT_EFFECT_N_FFT,
    hop_length: int = _DEFAULT_EFFECT_HOP_LENGTH,
    hard_mask: bool = False,
    *,
    validate: bool = True,
) -> HpssResult:
    """Perform harmonic-percussive source separation.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        kernel_harmonic: Harmonic median filter kernel size, in STFT frames: a
            positive odd integer at most 524287. The ceiling is 524288 and an
            even kernel is refused, so 524287 is the largest legal value.
        kernel_percussive: Percussive median filter kernel size, in STFT bins,
            under the same rule.
        n_fft: FFT size used for analysis/synthesis; an even integer >= 2
            (default 2048).
        hop_length: Hop size used for analysis/synthesis, in ``(0, n_fft / 2]``
            so frames overlap by at least half a window (default 512).
        hard_mask: Use binary harmonic/percussive masks (default ``False``).
        validate: Reject empty / NaN / Inf input (default ``True``).

    Returns:
        HpssResult with harmonic and percussive components.
    """
    _validate_samples("hpss", samples, validate=validate)
    n_fft, hop_length = _validate_effect_fft_options("hpss", n_fft, hop_length)
    kernel_harmonic = _validate_hpss_kernel("hpss", kernel_harmonic, "kernel_harmonic")
    kernel_percussive = _validate_hpss_kernel("hpss", kernel_percussive, "kernel_percussive")
    if not isinstance(hard_mask, bool):
        raise SonareValueError("hpss: hard_mask must be a bool")

    lib = _get_lib()
    use_soft_mask = 0 if hard_mask else 1
    if not hasattr(lib, "sonare_hpss_ex"):
        if n_fft != _DEFAULT_EFFECT_N_FFT or hop_length != _DEFAULT_EFFECT_HOP_LENGTH or hard_mask:
            raise _unsupported_effect_symbol("sonare_hpss_ex")
        if not hasattr(lib, "sonare_hpss"):
            raise _unsupported_effect_symbol("sonare_hpss")
        return _hpss_legacy(lib, samples, sample_rate, kernel_harmonic, kernel_percussive)

    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss_ex(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_int(hop_length, "hop_length"),
        _to_c_int(use_soft_mask, "use_soft_mask"),
        ctypes.c_int(0),
        ctypes.byref(out),
        None,
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


def _hpss_legacy(
    lib: ctypes.CDLL,
    samples: Sequence[float] | list[float],
    sample_rate: int,
    kernel_harmonic: int,
    kernel_percussive: int,
) -> HpssResult:
    """Call the pre-extended HPSS entry point for legacy-default requests."""
    c_array, length = _to_c_float_array(samples)
    out = SonareHpssResult()
    rc = lib.sonare_hpss(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(kernel_harmonic, "kernel_harmonic"),
        _to_c_int(kernel_percussive, "kernel_percussive"),
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
    return _call_float_transform("sonare_harmonic", samples, _to_c_int(sample_rate, "sample_rate"))


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
    return _call_float_transform(
        "sonare_percussive", samples, _to_c_int(sample_rate, "sample_rate")
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
        voiced_array, voiced_len = _to_c_int_array(voiced)
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
    config.scale_root = int(scale_root)
    if scale_mode_mask is not None:
        config.scale_mode_mask = int(scale_mode_mask)
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
        voiced_array, voiced_len = _to_c_int_array(voiced)
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


@_guard_buffer("samples")
def note_stretch(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int | None = None,
    stretch_ratio: float = 1.0,
) -> list[float]:
    """Time-stretch a single note region without changing pitch.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        onset_sample: Start sample index of the note region.
        offset_sample: End sample index of the note region (defaults to the input length).
        stretch_ratio: Stretch factor for the region (>1 lengthens).

    Returns:
        List of samples with the note region stretched.
    """
    resolved_offset = len(samples) if offset_sample is None else offset_sample
    return _call_float_transform(
        "sonare_note_stretch",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(onset_sample, "onset_sample"),
        _to_c_int(resolved_offset, "resolved_offset"),
        _to_c_float(stretch_ratio, "stretch_ratio"),
    )


@_guard_buffer("samples")
def note_move(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    onset_sample: int = 0,
    offset_sample: int | None = None,
    target_onset_sample: int = 0,
) -> list[float]:
    """Move a note region to a new onset without changing its duration."""
    resolved_offset = len(samples) if offset_sample is None else offset_sample
    return _call_float_transform(
        "sonare_note_move",
        samples,
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(onset_sample, "onset_sample"),
        _to_c_int(resolved_offset, "resolved_offset"),
        _to_c_int(target_onset_sample, "target_onset_sample"),
    )


@dataclasses.dataclass
class NoteEdit:
    """A pending, non-destructive change to one :class:`NoteObject`.

    The default instance is the identity edit, so a note carrying it is left
    untouched by :func:`render_notes` (and a set whose edits are all identity
    reproduces the input exactly). Mutate the fields in place to schedule work.

    Within one note :func:`render_notes` applies these in a fixed order: pitch
    curve, time stretch, pitch shift, formant warp, amplitude envelope, gain.

    Attributes:
        time_offset_samples: Moves the note along the timeline; negative moves
            it earlier.
        pitch_shift_semitones: Transposes the note; pitch only, duration is
            unchanged.
        gain_db: Level change applied over the note's span.
        time_stretch_ratio: ``>1`` lengthens the note, ``<1`` shortens it, with
            pitch preserved.
        muted: ``True`` silences the note's span; the other fields then do not
            apply.
        formant_shift_semitones: Moves the spectral envelope, on top of whatever
            ``pitch_shift_semitones`` already did to it. 0 runs no warp at all,
            so a pitch-only edit is not charged for an LPC analysis-resynthesis
            round it did not ask for; a pitch shift drags the formants with it,
            so holding them still is ``-pitch_shift_semitones`` and the chipmunk
            is the default. Saturates near -10.3 and +8.7 semitones rather than
            being rejected.
        vibrato_depth_change: Scales the vibrato measured over the note, stated
            as a change from it: 0 keeps it, ``-1`` flattens it, ``+1`` doubles
            it. It acts on the note's own pitch curve, so :func:`render_notes`
            rejects a note carrying it unless ``f0_hz`` is passed too.
        drift_change: The same, for the slow drift around the note's centre
            pitch. The two curves are split at ``vibrato_cutoff_hz``.
        amplitude_envelope: ``float32`` linear gain points over the note's span,
            on top of ``gain_db``; empty is no envelope. A set of gain points
            rather than a signal: it is stretched over whatever length the note
            renders at, so it survives a time stretch and need not match the
            note's frame count, and one entry is a constant gain.

    Example:
        >>> edit = libsonare.NoteEdit(gain_db=-3.0)
        >>> edit.amplitude_envelope = np.array([1.0, 0.0], dtype=np.float32)
        >>> edit.vibrato_depth_change = -1.0  # flatten this note's vibrato
    """

    time_offset_samples: int = 0
    pitch_shift_semitones: float = 0.0
    gain_db: float = 0.0
    time_stretch_ratio: float = 1.0
    muted: bool = False
    formant_shift_semitones: float = 0.0
    vibrato_depth_change: float = 0.0
    drift_change: float = 0.0
    # Out of the generated comparison, which an ndarray field makes raise on an
    # ambiguous truth value. __eq__ below puts the curve back in element by
    # element, because an edit carrying an envelope is not the identity edit and
    # must not compare equal to one.
    amplitude_envelope: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )

    def __eq__(self, other: object) -> bool:
        """Compare every field, the envelope element by element."""
        if not isinstance(other, NoteEdit):
            return NotImplemented
        return self._scalars() == other._scalars() and np.array_equal(
            np.asarray(self.amplitude_envelope), np.asarray(other.amplitude_envelope)
        )

    def _scalars(self) -> tuple[object, ...]:
        return (
            self.time_offset_samples,
            self.pitch_shift_semitones,
            self.gain_db,
            self.time_stretch_ratio,
            self.muted,
            self.formant_shift_semitones,
            self.vibrato_depth_change,
            self.drift_change,
        )


@dataclasses.dataclass
class NoteObject:
    """One editable note returned by :func:`extract_notes`.

    Sample bounds are half-open into the source audio; frame bounds are
    half-open into the caller's own F0 track, so the note's pitch curve is
    ``f0_hz[frame_start:frame_end]`` — it is deliberately not repeated here.
    The amplitude curve is measured by the extractor and therefore is: it is one
    RMS value per F0 frame, already sliced to this note.

    :func:`render_notes` reads ``onset_sample``, ``offset_sample`` and ``edit``,
    plus the frame bounds and ``median_hz`` when a curve edit needs them, so a
    host may hand back exactly what :func:`extract_notes` produced, or build a
    bare ``NoteObject(onset_sample=..., offset_sample=...)`` for a span it found
    some other way.

    The span has no default, so a note built by hand states it. The other
    surfaces declare the same two fields mandatory on their own note input and
    reject an omitted one; a default of 0 here would instead have been a
    zero-length span, rendering the note's edit as nothing.

    Attributes:
        onset_sample: First sample of the note.
        offset_sample: One past the last sample of the note.
        frame_start: First F0 frame of the note.
        frame_end: One past the last F0 frame of the note.
        median_hz: Median pitch of the span in Hz.
        median_cents: Median pitch in cents above the extractor's
            ``reference_hz``.
        f0_stability: Pitch steadiness in ``[0, 1]``; 1 is perfectly steady.
        amplitude: ``float32`` RMS curve, one value per frame of the span. Not
            part of ``==``, which compares the span, the metrics and the edit.
        edit: The pending :class:`NoteEdit`, identity on a freshly extracted
            note.
    """

    onset_sample: int
    offset_sample: int
    frame_start: int = 0
    frame_end: int = 0
    median_hz: float = 0.0
    median_cents: float = 0.0
    f0_stability: float = 0.0
    # Out of the comparison: an ndarray field makes the generated __eq__ raise
    # on ambiguous truth. The span already determines this curve.
    amplitude: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )
    edit: NoteEdit = dataclasses.field(default_factory=NoteEdit)


@_guard_buffer("samples", "f0_hz")
def extract_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Extract editable note objects from audio and a caller-supplied F0 track.

    Spans come from the same segmenter :func:`note_segments` uses, so the two
    agree on where the notes are; each note here additionally carries its median
    pitch, two measured quality figures, its own slice of the amplitude curve,
    and an identity :class:`NoteEdit` ready to be filled in. Feed the result
    back to :func:`render_notes` to hear the edits.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, one entry per analysis frame. Every value
            must be finite and non-negative; zero denotes an unvoiced frame.
            :func:`pitch_pyin` emits NaN for an unvoiced frame unless it is
            called with ``fill_na=True``, which returns the zero this expects,
            so a track taken from it needs that argument.
        frame_rate: F0 frames per second; must be finite and positive.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``; pass :func:`pitch_pyin`'s ``voiced_flag`` here.
        voiced_prob: Per-frame voicing probability in ``[0, 1]``, read ONLY when
            ``voiced`` is ``None``. pYIN's ``voiced_prob`` is a frame's voiced
            observation mass and rises with F0 for a fixed frame length, so
            thresholding it drops entire low registers — prefer ``voiced``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5). Ignored when
            ``voiced`` is supplied.

    Returns:
        List of :class:`NoteObject` in time order; empty when the F0 track
        segments into no stable notes.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, or if a buffer is
            empty or non-finite.

    Example:
        ``fill_na=True`` is required, not optional: pYIN's default leaves an
        unvoiced frame as NaN, which this function rejects.

        >>> pitch = libsonare.pitch_pyin(
        ...     samples, sample_rate=sr, hop_length=512, fill_na=True
        ... )
        >>> notes = libsonare.extract_notes(
        ...     samples,
        ...     sr,
        ...     pitch.f0,
        ...     sr / 512,
        ...     voiced=[int(v) for v in pitch.voiced_flag],
        ... )
        >>> notes[0].edit.gain_db = -6.0
        >>> quieter = libsonare.render_notes(samples, sr, notes)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_extract_notes"):
        raise _unsupported_effect_symbol("sonare_extract_notes")

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array, voiced_array = _note_voicing_arrays("extract_notes", n_frames, voiced, voiced_prob)
    config = _note_extractor_config(
        segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
    )
    out = SonareNoteObjectsResult()
    rc = lib.sonare_extract_notes(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        f0_array,
        prob_array,
        voiced_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _notes_from_c(out)
    finally:
        lib.sonare_free_note_objects(ctypes.byref(out))


def _note_voicing_arrays(
    fn_name: str,
    n_frames: int,
    voiced: Sequence[int] | list[int] | np.ndarray | None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None,
) -> tuple[object, object]:
    """Marshal the two per-frame voicing arrays; at least one is required."""
    if voiced is None and voiced_prob is None:
        raise SonareValueError(f"{fn_name}: pass voiced or voiced_prob")
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob, arg_name="voiced_prob")
        if prob_len != n_frames:
            raise SonareValueError(f"{fn_name}: voiced_prob must have f0_hz's length")
    voiced_array = None
    if voiced is not None:
        voiced_array, voiced_len = _to_c_int_array(voiced)
        if voiced_len != n_frames:
            raise SonareValueError(f"{fn_name}: voiced must have f0_hz's length")
    return prob_array, voiced_array


def _note_extractor_config(
    segmentation_threshold_cents: float | None,
    min_note_ms: float | None,
    reference_hz: float | None,
    voiced_threshold: float | None,
) -> SonareNoteExtractorConfig:
    """Build the extractor config; every field takes its library default at 0."""
    return SonareNoteExtractorConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if segmentation_threshold_cents is None else segmentation_threshold_cents,
        0.0 if min_note_ms is None else min_note_ms,
        0.0 if reference_hz is None else reference_hz,
        0.0 if voiced_threshold is None else voiced_threshold,
    )


def _notes_from_c(out: SonareNoteObjectsResult) -> list[NoteObject]:
    """Build the note list from a C result, before it is released.

    The two concatenated pools are copied out once and each note is handed its
    own slice of both, so no caller has to redo the offset arithmetic and no
    returned array is a view into memory the C side is about to free.
    """
    amplitude = _from_c_float_array(out.amplitude, out.amplitude_count)
    envelopes = _from_c_float_array(out.envelopes, out.envelope_count)
    return [_note_from_c(out.notes[i], amplitude, envelopes) for i in range(out.count)]


def _note_from_c(row: SonareNoteObject, amplitude: np.ndarray, envelopes: np.ndarray) -> NoteObject:
    """Build one :class:`NoteObject` from a C row and the two shared pools."""
    start = int(row.amplitude_offset)
    stop = start + int(row.frame_end) - int(row.frame_start)
    envelope_start = int(row.edit.envelope_offset)
    envelope_stop = envelope_start + int(row.edit.envelope_count)
    return NoteObject(
        onset_sample=int(row.onset_sample),
        offset_sample=int(row.offset_sample),
        frame_start=int(row.frame_start),
        frame_end=int(row.frame_end),
        median_hz=float(row.median_hz),
        median_cents=float(row.median_cents),
        f0_stability=float(row.f0_stability),
        amplitude=amplitude[start:stop].copy(),
        edit=NoteEdit(
            time_offset_samples=int(row.edit.time_offset_samples),
            pitch_shift_semitones=float(row.edit.pitch_shift_semitones),
            gain_db=float(row.edit.gain_db),
            time_stretch_ratio=float(row.edit.time_stretch_ratio),
            muted=bool(row.edit.muted),
            formant_shift_semitones=float(row.edit.formant_shift_semitones),
            vibrato_depth_change=float(row.edit.vibrato_depth_change),
            drift_change=float(row.edit.drift_change),
            amplitude_envelope=envelopes[envelope_start:envelope_stop].copy(),
        ),
    )


def _notes_to_c(fn_name: str, notes: Sequence[NoteObject]) -> tuple[object, int, object, int]:
    """Marshal a note list into the C note array plus its shared envelope pool.

    The C ABI carries every note's envelope in one array the edits index into,
    which is an ownership arrangement rather than something a caller should have
    to build; each :class:`NoteEdit` owns its own curve, so the pool is packed
    here and the offsets are derived.
    """
    note_count = len(notes)
    if note_count == 0:
        return None, 0, None, 0

    c_notes = (SonareNoteObject * note_count)()
    curves: list[np.ndarray] = []
    envelope_offset = 0
    for i, note in enumerate(notes):
        curve = np.asarray(note.edit.amplitude_envelope, dtype=np.float32)
        if curve.ndim != 1:
            raise SonareValueError(
                f"{fn_name}: notes[{i}].edit.amplitude_envelope must be one-dimensional"
            )
        curves.append(curve)
        # The metrics and the amplitude offset are not marshalled: nothing on
        # the far side reads them back.
        c_notes[i].onset_sample = int(note.onset_sample)
        c_notes[i].offset_sample = int(note.offset_sample)
        c_notes[i].frame_start = int(note.frame_start)
        c_notes[i].frame_end = int(note.frame_end)
        c_notes[i].median_hz = float(note.median_hz)
        c_notes[i].edit = SonareNoteEdit(
            # Narrowed rather than coerced: int(0.5) is 0, which is this field's
            # identity, so a sub-sample shift would render unmoved and report
            # success.
            time_offset_samples=_validate_c_int_field(
                fn_name, note.edit.time_offset_samples, f"notes[{i}].edit.time_offset_samples"
            ),
            envelope_offset=envelope_offset,
            envelope_count=int(curve.size),
            pitch_shift_semitones=float(note.edit.pitch_shift_semitones),
            gain_db=float(note.edit.gain_db),
            time_stretch_ratio=float(note.edit.time_stretch_ratio),
            formant_shift_semitones=float(note.edit.formant_shift_semitones),
            vibrato_depth_change=float(note.edit.vibrato_depth_change),
            drift_change=float(note.edit.drift_change),
            muted=1 if note.edit.muted else 0,
        )
        envelope_offset += int(curve.size)

    if envelope_offset == 0:
        return c_notes, note_count, None, 0
    pool, pool_count = _to_c_float_array(np.concatenate(curves))
    return c_notes, note_count, pool, pool_count


@_guard_buffer("samples")
def render_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    notes: Sequence[NoteObject],
    *,
    f0_hz: Sequence[float] | list[float] | np.ndarray | None = None,
    frame_rate: float | None = None,
    fade_ms: float | None = None,
    vibrato_cutoff_hz: float | None = None,
) -> np.ndarray:
    """Render edited note objects over their source audio.

    Each note's ``onset_sample``, ``offset_sample`` and ``edit`` are read, plus
    its frame bounds and ``median_hz`` when a curve edit needs them, so the list
    :func:`extract_notes` returned can be passed straight back. A note whose
    edit is the identity is not resynthesized, so a set whose edits are all
    identity reproduces the input exactly.

    Overlap is checked on the *source* spans only. Where an edit's
    ``time_offset_samples`` lands a note is not, and a note lengthened past its
    own span writes into its neighbours' samples, so two moved or stretched
    notes may be written over each other.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        notes: Notes to render; an empty sequence returns the input unchanged.
        f0_hz: The F0 track the notes were extracted from. Required only by
            ``NoteEdit.vibrato_depth_change`` and ``NoteEdit.drift_change``,
            which act on the note's own pitch curve -- that curve is this array
            sliced by ``[frame_start, frame_end)``, which the caller already
            holds, so it is not carried on the notes. Every other edit ignores
            it.
        frame_rate: F0 frames per second; required when ``f0_hz`` is given and
            ignored otherwise.
        fade_ms: Equal-power cross-fade at each edited note's edges; ``None``
            keeps the library default (5 ms). A hard cut is deliberately not
            selectable, because the seam it leaves behind is a click.
        vibrato_cutoff_hz: Boundary between the drift and the vibrato that the
            two curve edits act on; ``None`` keeps the default (3 Hz). Pass what
            :func:`decompose_note_pitch` was called with -- a host that draws
            the vibrato at one cutoff and edits it at another edits a curve it
            never showed anyone.

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or if ``f0_hz``
            was given without ``frame_rate``.
        SonareError: If the C call rejects the request (e.g. a note span that
            overlaps another's, or a curve edit with no ``f0_hz``).

    Example:
        >>> notes[0].edit.vibrato_depth_change = -1.0
        >>> flattened = libsonare.render_notes(
        ...     audio, sr, notes, f0_hz=pitch.f0, frame_rate=sr / 512
        ... )
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_render_notes"):
        raise _unsupported_effect_symbol("sonare_render_notes")

    c_array, length = _to_c_float_array(samples)
    c_notes, note_count, envelopes, envelope_count = _notes_to_c("render_notes", notes)

    f0_array = None
    n_frames = 0
    # Unread when there is no track, which is how the C ABI spells "no frames".
    c_frame_rate = 0.0
    if f0_hz is not None:
        if frame_rate is None:
            raise SonareValueError("render_notes: pass frame_rate with f0_hz")
        f0_array, n_frames = _to_c_float_array(
            _validate_samples("render_notes", f0_hz, arg_name="f0_hz")
        )
        c_frame_rate = float(frame_rate)

    config = SonareNoteRenderConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if fade_ms is None else fade_ms,
        0.0 if vibrato_cutoff_hz is None else vibrato_cutoff_hz,
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_render_notes(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                c_notes,
                _to_c_size_t(note_count, "note_count"),
                envelopes,
                _to_c_size_t(envelope_count, "envelope_count"),
                f0_array,
                _to_c_size_t(n_frames, "n_frames"),
                _to_c_float(c_frame_rate, "frame_rate"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


@dataclasses.dataclass
class PitchDecomposition:
    """One note's pitch curve split into a centre, a slow drift and a vibrato.

    ``drift_cents[i] + vibrato_cents[i]`` is the note's own pitch at frame ``i``,
    in cents above ``centre_hz``, to within float rounding, so the three parts
    reconstruct the curve. The drift filter is zero phase, so neither curve is
    shifted in time against the audio.

    Frames whose F0 is unusable carry no measurement, so the curve is held at
    the nearest usable neighbour across them. Both curves therefore have an
    entry everywhere; a host marking the held ones reads them off its own
    ``f0_hz``, which is exact.

    Attributes:
        centre_hz: The note's steady pitch in Hz. 0 when the note carries no
            usable pitch, and then both curves are empty.
        drift_cents: ``float32`` slow deviation from ``centre_hz``, in cents.
            Not part of ``==``, which an ndarray field would make raise on an
            ambiguous truth value.
        vibrato_cents: ``float32`` fast deviation, over the same frames. Out of
            ``==`` for the same reason.
    """

    centre_hz: float = 0.0
    drift_cents: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )
    vibrato_cents: np.ndarray = dataclasses.field(
        default_factory=lambda: np.empty(0, dtype=np.float32), compare=False
    )


@_guard_buffer("f0_hz")
def decompose_note_pitch(
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    median_hz: float,
    *,
    vibrato_cutoff_hz: float | None = None,
) -> PitchDecomposition:
    """Split one note's pitch curve into a centre, a drift and a vibrato.

    This is what a host draws when it shows a note's pitch, and what
    ``NoteEdit.vibrato_depth_change`` and ``NoteEdit.drift_change`` then scale.
    Hand :func:`render_notes` the same ``vibrato_cutoff_hz``, or the curve being
    edited is not the curve that was drawn.

    The note's own F0 curve is not returned by :func:`extract_notes`, because it
    is the caller's own track sliced by the note's frame bounds; pass that slice
    here together with the note's ``median_hz``.

    Args:
        f0_hz: The note's slice of the F0 track, one entry per frame. Every
            value must be finite and non-negative; zero denotes an unvoiced
            frame.
        frame_rate: F0 frames per second; must be finite and positive.
        median_hz: The note's ``median_hz``; must be finite and non-negative. A
            note with no pitch is spelled 0, so a negative value is a caller bug
            rather than a second way of saying that.
        vibrato_cutoff_hz: Boundary between the two curves; ``None`` keeps the
            default (3 Hz).

    Returns:
        :class:`PitchDecomposition`. A note with no usable pitch is reported as
        a zero ``centre_hz`` and two empty curves rather than as an error.

    Raises:
        SonareValueError: If ``f0_hz`` is empty or non-finite.
        SonareError: If the C call rejects the request.

    Example:
        >>> note = notes[0]
        >>> curve = libsonare.decompose_note_pitch(
        ...     f0_hz[note.frame_start : note.frame_end], sr / 512, note.median_hz
        ... )
        >>> curve.vibrato_cents.max()  # doctest: +SKIP
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_decompose_note_pitch"):
        raise _unsupported_effect_symbol("sonare_decompose_note_pitch")

    f0_array, n_frames = _to_c_float_array(f0_hz)
    out = SonarePitchDecompositionResult()
    rc = lib.sonare_decompose_note_pitch(
        f0_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        _to_c_float(median_hz, "median_hz"),
        _to_c_float(0.0 if vibrato_cutoff_hz is None else vibrato_cutoff_hz, "vibrato_cutoff_hz"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return PitchDecomposition(
            centre_hz=float(out.centre_hz),
            drift_cents=_from_c_float_array(out.drift_cents, out.count),
            vibrato_cents=_from_c_float_array(out.vibrato_cents, out.count),
        )
    finally:
        lib.sonare_free_pitch_decomposition(ctypes.byref(out))


def _note_set_index(fn_name: str, arg_name: str, value: int) -> int:
    """Reject a negative note index before c_size_t wraps it into a huge one."""
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise SonareValueError(f"{fn_name}: {arg_name} must be an integer")
    if int(value) < 0:
        raise SonareValueError(f"{fn_name}: {arg_name} must be non-negative")
    return int(value)


def _note_set_edit(
    fn_name: str,
    symbol: str,
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    voiced: Sequence[int] | list[int] | np.ndarray | None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None,
    config: SonareNoteExtractorConfig,
    cut: tuple[object, ...],
) -> list[NoteObject]:
    """Shared body of :func:`split_note` and :func:`merge_notes`.

    The two differ only in ``cut``, the pair of arguments naming where to cut or
    what to join; everything else -- the track, the note set and its envelope
    pool, the extractor config -- is marshalled identically.
    """
    lib = _get_lib()
    if not hasattr(lib, symbol):
        raise _unsupported_effect_symbol(symbol)

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz, arg_name="f0_hz")
    prob_array, voiced_array = _note_voicing_arrays(fn_name, n_frames, voiced, voiced_prob)
    c_notes, note_count, envelopes, envelope_count = _notes_to_c(fn_name, notes)

    out = SonareNoteObjectsResult()
    rc = getattr(lib, symbol)(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        f0_array,
        prob_array,
        voiced_array,
        _to_c_size_t(n_frames, "n_frames"),
        _to_c_float(frame_rate, "frame_rate"),
        ctypes.byref(config),
        c_notes,
        _to_c_size_t(note_count, "note_count"),
        envelopes,
        _to_c_size_t(envelope_count, "envelope_count"),
        *cut,
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return _notes_from_c(out)
    finally:
        lib.sonare_free_note_objects(ctypes.byref(out))


@_guard_buffer("samples", "f0_hz")
def split_note(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    index: int,
    frame: int,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Split one note in two at a track frame, returning the whole new note set.

    Both halves are re-derived from the audio and the track the way
    :func:`extract_notes` derives its own, rather than by patching the fields of
    the note they replace. Both inherit the source note's edit, and its
    amplitude envelope is cut at the same proportion so each half keeps its own
    part of it -- which means a note whose edit is the identity still renders
    bit for bit after being split. A one-entry envelope is a constant over the
    span, so both halves get that same entry.

    Every note in the set, not just the two halves, has its spans, curves,
    medians and stability re-derived from ``samples`` and the track, because a
    :class:`NoteObject` carries no curves for this call to copy through. The
    frame bounds are therefore what a note is identified by here, and the audio
    and track arguments must be the ones the set was extracted from, or the
    whole set is re-measured against something else.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, the track the set was extracted from.
        frame_rate: F0 frames per second; must be finite and positive.
        notes: The current note set. Each note's ``[frame_start, frame_end)``
            must be non-empty and inside the track.
        index: Note to split.
        frame: Track frame to cut at, strictly inside that note's own span.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``.
        voiced_prob: Per-frame voicing probability, read ONLY when ``voiced`` is
            ``None``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5).

    Returns:
        The whole new list of :class:`NoteObject` in time order, one longer than
        ``notes``.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, if ``index`` is
            negative, or if a buffer is empty or non-finite.
        SonareError: If the C call rejects the request (e.g. a frame on or
            outside the note's own boundaries).

    Example:
        >>> notes = libsonare.split_note(audio, sr, f0_hz, sr / 512, notes, 1, 18,
        ...                              voiced=voiced)
    """
    return _note_set_edit(
        "split_note",
        "sonare_split_note",
        samples,
        sample_rate,
        f0_hz,
        frame_rate,
        notes,
        voiced,
        voiced_prob,
        _note_extractor_config(
            segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
        ),
        (
            ctypes.c_size_t(_note_set_index("split_note", "index", index)),
            _to_c_int32(int(frame), "frame"),
        ),
    )


@_guard_buffer("samples", "f0_hz")
def merge_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    f0_hz: Sequence[float] | list[float] | np.ndarray,
    frame_rate: float,
    notes: Sequence[NoteObject],
    first: int,
    last: int,
    *,
    voiced: Sequence[int] | list[int] | np.ndarray | None = None,
    voiced_prob: Sequence[float] | list[float] | np.ndarray | None = None,
    segmentation_threshold_cents: float | None = None,
    min_note_ms: float | None = None,
    reference_hz: float | None = None,
    voiced_threshold: float | None = None,
) -> list[NoteObject]:
    """Join a run of notes into one, returning the whole new note set.

    The result spans from ``notes[first]``'s onset to ``notes[last]``'s offset,
    including whatever the segmenter cut out between them, and its measured
    fields are derived over that whole span -- the pitch and amplitude of an
    unvoiced gap live in the track and the audio, not in either neighbour.

    It takes ``notes[first]``'s edit, envelope included. Notes carrying
    different edits have no single correct answer here, so the rule is stated
    rather than guessed at; set the edit afterwards if it matters.

    Every note in the set is re-derived from ``samples`` and the track, exactly
    as :func:`split_note` describes.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        f0_hz: Per-frame F0 in Hz, the track the set was extracted from.
        frame_rate: F0 frames per second; must be finite and positive.
        notes: The current note set. Each note's ``[frame_start, frame_end)``
            must be non-empty and inside the track.
        first: Index of the first note to join; must be ``< last``.
        last: Index of the last note to join, inclusive.
        voiced: Per-frame voiced flags (non-zero = voiced). Preferred over
            ``voiced_prob``.
        voiced_prob: Per-frame voicing probability, read ONLY when ``voiced`` is
            ``None``.
        segmentation_threshold_cents: Pitch jump that starts a new note;
            ``None`` keeps the library default (50 cents).
        min_note_ms: Shortest note kept; ``None`` keeps the default (30 ms).
        reference_hz: Reference for ``median_cents``; ``None`` keeps the default
            (A4 = 440 Hz).
        voiced_threshold: Value of ``voiced_prob`` at or above which a frame
            counts as voiced; ``None`` keeps the default (0.5).

    Returns:
        The whole new list of :class:`NoteObject` in time order, ``last - first``
        shorter than ``notes``.

    Raises:
        SonareValueError: If neither ``voiced`` nor ``voiced_prob`` is given, if
            either has a different length than ``f0_hz``, if ``first`` or
            ``last`` is negative, or if a buffer is empty or non-finite.
        SonareError: If the C call rejects the request (e.g. a run that does not
            ascend, or one that runs past the end of the set).

    Example:
        >>> notes = libsonare.merge_notes(audio, sr, f0_hz, sr / 512, notes, 0, 1,
        ...                               voiced=voiced)
    """
    return _note_set_edit(
        "merge_notes",
        "sonare_merge_notes",
        samples,
        sample_rate,
        f0_hz,
        frame_rate,
        notes,
        voiced,
        voiced_prob,
        _note_extractor_config(
            segmentation_threshold_cents, min_note_ms, reference_hz, voiced_threshold
        ),
        (
            ctypes.c_size_t(_note_set_index("merge_notes", "first", first)),
            ctypes.c_size_t(_note_set_index("merge_notes", "last", last)),
        ),
    )


@dataclasses.dataclass
class PercussiveEventEdit:
    """A pending, non-destructive change to one :class:`PercussiveEvent`.

    The default instance is the identity edit, so an event carrying it is left
    untouched by :func:`render_percussive_events` (and a set whose edits are all
    identity reproduces the input bit for bit). Mutate the fields in place to
    schedule work.

    A struck sound has no steady pitch to edit, so the axes are time and
    amplitude and there is deliberately nothing else here -- this is not a
    :class:`NoteEdit` with fields removed, and the two models never refer to each
    other.

    Attributes:
        time_offset_samples: Moves the hit along the timeline; negative moves it
            earlier. A shift that pushes the signal past either end of the audio
            is truncated there rather than wrapped.
        gain_db: Level change applied to the hit. It scales the percussive
            component of the span, which is the signal ``peak_amplitude`` is
            measured on, not the source.
        muted: ``True`` silences the hit; the other fields then do not apply.
            What is silenced is the percussive component alone, so whatever was
            sustaining under the hit keeps sounding.

    Example:
        >>> events[0].edit = libsonare.PercussiveEventEdit(gain_db=-6.0)
        >>> events[1].edit.time_offset_samples = -441  # 20 ms earlier at 22050 Hz
    """

    time_offset_samples: int = 0
    gain_db: float = 0.0
    muted: bool = False


@dataclasses.dataclass
class PercussiveEvent:
    """One editable percussive event returned by :func:`extract_percussive_events`.

    Sample bounds are half-open into the source audio. The three measured figures
    are taken on the percussive component of the span rather than on the span
    itself, because that component is the signal an edit acts on.

    It carries no pitch and is never associated with a :class:`NoteObject`: the
    two models come from separate calls and do not refer to each other.

    :func:`render_percussive_events` reads ``onset_sample``, ``offset_sample``
    and ``edit`` only, so a host may hand back exactly what
    :func:`extract_percussive_events` produced, or build a bare
    ``PercussiveEvent(onset_sample=..., offset_sample=...)`` for a span it found
    some other way.

    The span has no default, so an event built by hand states it. The other
    surfaces declare the same two fields mandatory on their own event input and
    reject an omitted one; a default of 0 here would instead have been a
    zero-length span, rendering the event's edit as nothing.

    Attributes:
        onset_sample: First sample of the event, backtracked to the start of the
            transient rather than left where peak-picking landed.
        offset_sample: One past the last sample. The next onset closes a span;
            where none follows, ``max_event_ms`` does.
        strength: Detector strength at the onset, on the onset envelope's own
            scale. It orders events against each other and carries no absolute
            meaning.
        peak_amplitude: Peak absolute sample of the percussive component over the
            span, linear. This is the signal ``edit.gain_db`` scales.
        percussive_ratio: Share of the span's energy the separation assigned to
            percussion, in ``[0, 1]``; 0 when the span is silent. It describes the
            *span*, not the onset that opened it: an isolated hit sits near 1, but
            a real hit under a loud sustain sits near 0, because the sustain owns
            the span's energy. So it is not a test for whether a hit is there.
        edit: The pending :class:`PercussiveEventEdit`, identity on a freshly
            extracted event.
    """

    onset_sample: int
    offset_sample: int
    strength: float = 0.0
    peak_amplitude: float = 0.0
    percussive_ratio: float = 0.0
    edit: PercussiveEventEdit = dataclasses.field(default_factory=PercussiveEventEdit)


def _percussive_separation(
    fn_name: str,
    n_fft: int | None,
    hop_length: int | None,
    hpss_kernel_harmonic: int | None,
    hpss_kernel_percussive: int | None,
) -> dict[str, int]:
    """The separation fields both percussive configs carry; 0 keeps each default.

    Extraction measures events against this separation and rendering has to
    repeat it, so it is one set of fields both sides take rather than a framing
    each of them restates -- and one place the signed-32-bit narrowing is
    checked, since 0 is the default here and a value that wraps to it would
    separate on the default while reporting success.
    """
    fields = {
        "n_fft": n_fft,
        "hop_length": hop_length,
        "hpss_kernel_harmonic": hpss_kernel_harmonic,
        "hpss_kernel_percussive": hpss_kernel_percussive,
    }
    return {
        name: 0 if value is None else _validate_c_int_field(fn_name, value, name)
        for name, value in fields.items()
    }


def _percussive_event_from_c(row: SonarePercussiveEvent) -> PercussiveEvent:
    """Build one :class:`PercussiveEvent` from a C row, before it is released."""
    return PercussiveEvent(
        onset_sample=int(row.onset_sample),
        offset_sample=int(row.offset_sample),
        strength=float(row.strength),
        peak_amplitude=float(row.peak_amplitude),
        percussive_ratio=float(row.percussive_ratio),
        edit=PercussiveEventEdit(
            time_offset_samples=int(row.edit.time_offset_samples),
            gain_db=float(row.edit.gain_db),
            muted=bool(row.edit.muted),
        ),
    )


def _percussive_events_to_c(events: Sequence[PercussiveEvent]) -> tuple[object, int]:
    """Marshal an event list into the C event array; an empty set is NULL and 0."""
    count = len(events)
    if count == 0:
        return None, 0

    c_events = (SonarePercussiveEvent * count)()
    for i, event in enumerate(events):
        # The three measured figures are not marshalled: rendering reads the span
        # and the edit, so nothing on the far side reads them back.
        c_events[i].onset_sample = int(event.onset_sample)
        c_events[i].offset_sample = int(event.offset_sample)
        c_events[i].edit = SonarePercussiveEventEdit(
            # Narrowed rather than coerced: int(0.5) is 0, which is this field's
            # identity, so a sub-sample shift would render unmoved and report
            # success.
            time_offset_samples=_validate_c_int_field(
                "render_percussive_events",
                event.edit.time_offset_samples,
                f"events[{i}].edit.time_offset_samples",
            ),
            gain_db=float(event.edit.gain_db),
            muted=1 if event.edit.muted else 0,
        )
    return c_events, count


@_guard_buffer("samples")
def extract_percussive_events(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    onset_wait: int | None = None,
    onset_delta: float | None = None,
    max_event_ms: float | None = None,
    min_percussive_ratio: float | None = None,
) -> list[PercussiveEvent]:
    """Extract editable percussive events from audio.

    Unlike :func:`extract_notes` this needs nothing but the audio: onsets are
    detected on the percussive component rather than on the source, so a harmonic
    attack is attenuated before the detector sees it instead of being filtered
    out afterwards. Each onset opens a span that the next one closes, and every
    returned event carries the identity :class:`PercussiveEventEdit`. Feed the
    result back to :func:`render_percussive_events` to hear the edits.

    Each onset is backtracked to the transient's start, which is not optional and
    is why there is no knob for it: peak-picking lands after the attack, and a
    span that opened there would report the next hit's peak and leave its own
    attack behind when muted.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        n_fft: FFT size the separation and the onset detector share; ``None``
            keeps the library default (2048). The pair must overlap-add --
            ``n_fft`` even and at least 2, ``hop_length`` no more than half of it
            -- because the separation inverts an STFT.
        hop_length: Hop the same two share; ``None`` keeps the default (512).
            They cannot be set apart: an event measured on one framing and lifted
            out on another is not the same signal.
        hpss_kernel_harmonic: Median filter length the separation runs along
            time; ``None`` keeps the default (31). A longer harmonic kernel calls
            more of a sustained sound harmonic.
        hpss_kernel_percussive: The same, along frequency; ``None`` keeps 31.
        onset_wait: Minimum frames between consecutive onsets; ``None`` keeps the
            default (1). Must be a whole number: 0 is the default's own spelling,
            so a fractional wait is refused rather than resolved onto it.
        onset_delta: Offset added to the detector's adaptive threshold; raising
            it finds fewer, stronger hits and lowering it finds more. ``None``
            keeps the default (0.06), so exactly 0 is the one value not
            selectable here -- a negative one is accepted and puts the threshold
            below the default, which is the direction a caller reaching for 0
            wanted anyway.

            It is in the units of :attr:`PercussiveEvent.strength`, the onset
            envelope's own scale, which is **not** normalised: three isolated
            hits measured 37 to 51 on one fixture, where the 0.06 default selects
            nothing at all. Read a useful value off the strengths a default
            extraction returns rather than guessing one -- a number chosen
            assuming the scale is around 1 looks like a knob that does nothing.
        max_event_ms: Caps a span that no onset follows; ``None`` keeps the
            default (500). It binds at the end of a phrase and at the end of the
            track, and nowhere else. A span never runs past the end of the audio
            whatever this says.
        min_percussive_ratio: Drops an event whose ``percussive_ratio`` falls
            below this; must be in ``[0, 1]``. ``None`` keeps the default, which
            is 0 -- and 0 is also the meaningful "keep everything", so unlike
            every other argument here passing it explicitly is not distinct from
            passing ``None``. Raising it is useful on material that is mostly
            drums and wrong on a dense mix, where it also drops real hits sitting
            over a loud sustain. Spans are fixed before it drops anything, so
            raising it selects events without lengthening the survivors.

    Returns:
        List of :class:`PercussiveEvent` in time order; empty when nothing was
        detected, which is reported rather than raised.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or a framing,
            kernel size or ``onset_wait`` is not a whole number fitting in a
            signed 32-bit integer.
        SonareError: If the C call rejects the request (a framing that breaks
            overlap-add, a negative ``onset_wait``, a negative or non-finite
            ``max_event_ms``, or a ``min_percussive_ratio`` outside ``[0, 1]``).
            0 is not rejected for any of these: it is the C ABI's spelling of the
            default.

    Example:
        >>> events = libsonare.extract_percussive_events(audio, sr)
        >>> events[0].edit.muted = True
        >>> without_the_first_hit = libsonare.render_percussive_events(audio, sr, events)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_extract_percussive_events"):
        raise _unsupported_effect_symbol("sonare_extract_percussive_events")

    c_array, length = _to_c_float_array(samples)
    config = SonarePercussiveEventConfig(
        struct_version=_PERCUSSIVE_STRUCT_VERSION,
        **_percussive_separation(
            "extract_percussive_events",
            n_fft,
            hop_length,
            hpss_kernel_harmonic,
            hpss_kernel_percussive,
        ),
        # Narrowed rather than coerced: int(0.5) is the 0 this field reads as
        # "keep the default", so a fractional wait would run at the default and
        # report success.
        onset_wait=(
            0
            if onset_wait is None
            else _validate_c_int_field("extract_percussive_events", onset_wait, "onset_wait")
        ),
        onset_delta=0.0 if onset_delta is None else float(onset_delta),
        max_event_ms=0.0 if max_event_ms is None else float(max_event_ms),
        # 0 is this field's own meaning as well as its default, so it is passed
        # through rather than read as "leave the default".
        min_percussive_ratio=(0.0 if min_percussive_ratio is None else float(min_percussive_ratio)),
    )
    out = SonarePercussiveEventsResult()
    rc = lib.sonare_extract_percussive_events(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return [_percussive_event_from_c(out.events[i]) for i in range(out.count)]
    finally:
        lib.sonare_free_percussive_events(ctypes.byref(out))


@_guard_buffer("samples")
def render_percussive_events(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    events: Sequence[PercussiveEvent],
    *,
    n_fft: int | None = None,
    hop_length: int | None = None,
    hpss_kernel_harmonic: int | None = None,
    hpss_kernel_percussive: int | None = None,
    fade_ms: float | None = None,
) -> np.ndarray:
    """Render edited percussive events over their source audio.

    Per event the lifted signal is the percussive component over
    ``[onset_sample, offset_sample)`` under the tail fade. It is subtracted where
    it sits and, unless the event is muted, added back at the shifted position
    scaled by the gain. Only that signal moves, so muting a hit leaves the
    harmonic content under it sounding and moving one does not drag its
    neighbours' sustain along.

    Each event's span and ``edit`` are read; ``strength``, ``peak_amplitude`` and
    ``percussive_ratio`` are ignored, so the list
    :func:`extract_percussive_events` returned can be passed straight back. A set
    whose edits are all identity reproduces the input bit for bit and runs no
    separation at all.

    Overlap is checked on the *source* spans only. Where an edit's
    ``time_offset_samples`` lands an event is not, so two moved events may be
    written over each other. A shift that pushes the signal past either end is
    truncated there rather than wrapped.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        events: Events to render; an empty sequence returns the input unchanged.
            Every span must be non-empty and inside ``samples``, and no two may
            overlap -- which holds for an extracted set as produced.
        n_fft: The separation the events were measured against. Pass back what
            :func:`extract_percussive_events` was called with, or a different
            separation lifts a different signal out of the span than the one the
            events describe. ``None`` keeps the library default (2048).
        hop_length: The same; ``None`` keeps the default (512).
        hpss_kernel_harmonic: The same; ``None`` keeps the default (31).
        hpss_kernel_percussive: The same; ``None`` keeps the default (31).
        fade_ms: Fade-out at the tail of each lifted span; ``None`` keeps the
            library default (5 ms). There is deliberately no matching fade-in: a
            span opens in front of its transient, where the percussive component
            is near-silent, so cutting square there costs nothing and keeps a
            muted hit's attack from surviving inside a fade. A zero-length fade
            is unreachable here rather than rejected -- 0 selects the default --
            and a hard cut is not a thing to want anyway, because what the fade
            shapes is the signal being subtracted, so squaring it off leaves a
            step.

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite, or a framing or
            kernel size does not fit in a signed 32-bit integer.
        SonareError: If the C call rejects the request (an empty, reversed or
            out-of-range span, overlapping source spans, a non-finite gain, a
            framing that breaks overlap-add, or a negative or non-finite
            ``fade_ms`` -- 0 is the default rather than a rejected value). The
            framing and every span are validated even when every edit is the
            identity and no separation runs, so an unusable request is an error
            on every set rather than on the ones that reach the separation.

    Example:
        >>> events[1].edit.time_offset_samples = 441  # 20 ms later at 22050 Hz
        >>> events[2].edit.gain_db = -3.0
        >>> rendered = libsonare.render_percussive_events(audio, sr, events)
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_render_percussive_events"):
        raise _unsupported_effect_symbol("sonare_render_percussive_events")

    c_array, length = _to_c_float_array(samples)
    c_events, count = _percussive_events_to_c(events)
    config = SonarePercussiveRenderConfig(
        struct_version=_PERCUSSIVE_STRUCT_VERSION,
        **_percussive_separation(
            "render_percussive_events",
            n_fft,
            hop_length,
            hpss_kernel_harmonic,
            hpss_kernel_percussive,
        ),
        fade_ms=0.0 if fade_ms is None else float(fade_ms),
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_render_percussive_events(
                c_array,
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                c_events,
                _to_c_size_t(count, "count"),
                ctypes.byref(config),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _from_c_float_array(out, out_length.value)


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
    "rect": 3,
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


@_guard_buffer("samples")
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
        n_fft: STFT size; a power of two in ``[2, 262144]`` (default 2048).
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
        The shape rules -- ``n_fft`` a power of two, ``hop_length`` within
        ``(0, n_fft / 2]``, ``heal_radius_frames`` non-negative -- are validated
        eagerly here and raise :class:`SonareValueError`, which is both a
        ``ValueError`` (the idiomatic Python contract) and a ``SonareError``. The
        upper bound on ``n_fft`` is a value rule and is checked by the C++ core
        instead, so exceeding it raises a plain :class:`SonareError`; both carry
        ``ErrorCode.INVALID_PARAMETER``, so only ``except ValueError`` tells them
        apart. The Node and WASM surfaces accept the same valid inputs but
        delegate every rejection to the core. The accepted input range is
        identical across surfaces.
    """
    # Narrowed ahead of the range checks below, and for a reason of its own: 0 is
    # the C sentinel for "keep the default" on all three, and int(0.5) is that 0,
    # so a fractional count would run at the default and report success.
    n_fft = _validate_c_int_field("spectral_edit", n_fft, "n_fft")
    hop_length = _validate_c_int_field("spectral_edit", hop_length, "hop_length")
    heal_radius_frames = _validate_c_int_field(
        "spectral_edit", heal_radius_frames, "heal_radius_frames"
    )
    # spectral_edit is deliberately stricter than the shared even-size rule:
    # src/effects/spectral_edit.cpp requires a power of two, so checking it here
    # reports the same rejection eagerly and by name instead of as a generic
    # invalid-parameter return from the core.
    _require_power_of_two(n_fft, "n_fft")
    if hop_length <= 0 or hop_length > n_fft // 2:
        raise SonareValueError("hop_length must satisfy 0 < hop_length <= n_fft / 2")
    if heal_radius_frames < 0:
        raise SonareValueError("heal_radius_frames must be non-negative")
    window_value = _coerce_spectral_edit_window(window)

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    config = SonareSpectralEditConfig(
        n_fft=n_fft,
        hop_length=hop_length,
        window=int(window_value),
        heal_radius_frames=heal_radius_frames,
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
                _to_c_size_t(length, "length"),
                _to_c_int(sample_rate, "sample_rate"),
                ctypes.byref(config),
                c_ops,
                _to_c_size_t(n_ops, "n_ops"),
                ctypes.byref(out),
                ctypes.byref(out_length),
            )
        )
        return _float_array_result(out, out_length.value)
