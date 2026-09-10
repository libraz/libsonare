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
    SonarePitchCorrectionConfig,
    SonareSpectralEditConfig,
    SonareSpectralRegionOp,
)
from ._runtime import (
    _C_INT_MAX,
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
    _to_c_float_array,
    _to_c_int_array,
    _validate_effect_fft_options,
    _validate_samples,
)
from .types import HpssResult

_DEFAULT_EFFECT_N_FFT = 2048
_DEFAULT_EFFECT_HOP_LENGTH = 512

# Both note-object configs are at layout version 1; 0 selects the same layout.
_NOTE_STRUCT_VERSION = 1


def _unsupported_effect_symbol(symbol: str) -> SonareError:
    return SonareError(
        int(ErrorCode.NOT_SUPPORTED),
        f"libsonare does not export {symbol}; install a matching native library",
    )


def _validate_hpss_kernel(fn_name: str, value: int, arg_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, Integral):
        raise SonareValueError(f"{fn_name}: {arg_name} must be an integer")
    value = int(value)
    if value <= 0 or value > _C_INT_MAX or value % 2 == 0:
        raise SonareValueError(
            f"{fn_name}: {arg_name} must be a positive odd signed 32-bit integer"
        )
    return value


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
        kernel_harmonic: Harmonic median filter kernel size (positive odd integer).
        kernel_percussive: Percussive median filter kernel size (positive odd integer).
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
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(kernel_harmonic),
        ctypes.c_int(kernel_percussive),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(use_soft_mask),
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
            "sonare_time_stretch", samples, ctypes.c_int(sample_rate), ctypes.c_float(rate)
        )
    return _call_float_transform(
        "sonare_time_stretch_ex",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_float(rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
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
            "sonare_pitch_shift", samples, ctypes.c_int(sample_rate), ctypes.c_float(semitones)
        )
    return _call_float_transform(
        "sonare_pitch_shift_ex",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_float(semitones),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
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
        ctypes.c_int(sample_rate),
        ctypes.c_float(current_midi),
        ctypes.c_float(target_midi),
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
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob)
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
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob)
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
        ctypes.c_int(sample_rate),
        ctypes.c_int(onset_sample),
        ctypes.c_int(resolved_offset),
        ctypes.c_float(stretch_ratio),
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
        ctypes.c_int(sample_rate),
        ctypes.c_int(onset_sample),
        ctypes.c_int(resolved_offset),
        ctypes.c_int(target_onset_sample),
    )


@dataclasses.dataclass
class NoteEdit:
    """A pending, non-destructive change to one :class:`NoteObject`.

    The default instance is the identity edit, so a note carrying it is left
    untouched by :func:`render_notes` (and a set whose edits are all identity
    reproduces the input exactly). Mutate the fields in place to schedule work:

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
    """

    time_offset_samples: int = 0
    pitch_shift_semitones: float = 0.0
    gain_db: float = 0.0
    time_stretch_ratio: float = 1.0
    muted: bool = False


@dataclasses.dataclass
class NoteObject:
    """One editable note returned by :func:`extract_notes`.

    Sample bounds are half-open into the source audio; frame bounds are
    half-open into the caller's own F0 track, so the note's pitch curve is
    ``f0_hz[frame_start:frame_end]`` — it is deliberately not repeated here.
    The amplitude curve is measured by the extractor and therefore is: it is one
    RMS value per F0 frame, already sliced to this note.

    :func:`render_notes` reads only ``onset_sample``, ``offset_sample`` and
    ``edit``, so a host may hand back exactly what :func:`extract_notes`
    produced, or build a bare ``NoteObject(onset_sample=..., offset_sample=...)``
    for a span it found some other way.

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

    onset_sample: int = 0
    offset_sample: int = 0
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
        >>> pitch = libsonare.pitch_pyin(samples, sample_rate=sr, hop_length=512)
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
    if voiced is None and voiced_prob is None:
        raise SonareValueError("extract_notes: pass voiced or voiced_prob")

    c_array, length = _to_c_float_array(samples)
    f0_array, n_frames = _to_c_float_array(f0_hz)
    prob_array = None
    if voiced_prob is not None:
        prob_array, prob_len = _to_c_float_array(voiced_prob)
        if prob_len != n_frames:
            raise SonareValueError("extract_notes: voiced_prob must have f0_hz's length")
    voiced_array = None
    if voiced is not None:
        voiced_array, voiced_len = _to_c_int_array(voiced)
        if voiced_len != n_frames:
            raise SonareValueError("extract_notes: voiced must have f0_hz's length")

    config = SonareNoteExtractorConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if segmentation_threshold_cents is None else segmentation_threshold_cents,
        0.0 if min_note_ms is None else min_note_ms,
        0.0 if reference_hz is None else reference_hz,
        0.0 if voiced_threshold is None else voiced_threshold,
    )
    out = SonareNoteObjectsResult()
    rc = lib.sonare_extract_notes(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        f0_array,
        prob_array,
        voiced_array,
        ctypes.c_size_t(n_frames),
        ctypes.c_float(frame_rate),
        ctypes.byref(config),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        # Copy the concatenated RMS curve out once, then hand every note its own
        # slice so no caller has to redo the amplitude-offset arithmetic.
        amplitude = _from_c_float_array(out.amplitude, out.amplitude_count)
        return [_note_from_c(out.notes[i], amplitude) for i in range(out.count)]
    finally:
        lib.sonare_free_note_objects(ctypes.byref(out))


def _note_from_c(row: SonareNoteObject, amplitude: np.ndarray) -> NoteObject:
    """Build one :class:`NoteObject` from a C row and the shared RMS curve."""
    start = int(row.amplitude_offset)
    stop = start + int(row.frame_end) - int(row.frame_start)
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
        ),
    )


@_guard_buffer("samples")
def render_notes(
    samples: Sequence[float] | list[float] | np.ndarray,
    sample_rate: int,
    notes: Sequence[NoteObject],
    *,
    fade_ms: float | None = None,
) -> np.ndarray:
    """Render edited note objects over their source audio.

    Only each note's ``onset_sample``, ``offset_sample`` and ``edit`` are read,
    so the list :func:`extract_notes` returned can be passed straight back. A
    note whose edit is the identity is not resynthesized, so a set whose edits
    are all identity reproduces the input exactly.

    Overlap is checked on the *source* spans only. Where an edit's
    ``time_offset_samples`` lands a note is not, and a note lengthened past its
    own span writes into its neighbours' samples, so two moved or stretched
    notes may be written over each other.

    Args:
        samples: Source audio (any sequence convertible to float32).
        sample_rate: Sample rate in Hz.
        notes: Notes to render; an empty sequence returns the input unchanged.
        fade_ms: Equal-power cross-fade at each edited note's edges; ``None``
            keeps the library default (5 ms). A hard cut is deliberately not
            selectable, because the seam it leaves behind is a click.

    Returns:
        ``numpy.ndarray`` of ``float32`` with the same length as the input.

    Raises:
        SonareValueError: If ``samples`` is empty or non-finite.
        SonareError: If the C call rejects the request (e.g. a note span that
            overlaps another's).
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_render_notes"):
        raise _unsupported_effect_symbol("sonare_render_notes")

    c_array, length = _to_c_float_array(samples)
    note_count = len(notes)
    c_notes = None
    if note_count > 0:
        c_notes = (SonareNoteObject * note_count)()
        for i, note in enumerate(notes):
            # Only the span and the edit are read; the frame bounds, medians and
            # metrics stay zeroed rather than being marshalled for nothing.
            c_notes[i].onset_sample = int(note.onset_sample)
            c_notes[i].offset_sample = int(note.offset_sample)
            c_notes[i].edit = SonareNoteEdit(
                int(note.edit.time_offset_samples),
                float(note.edit.pitch_shift_semitones),
                float(note.edit.gain_db),
                float(note.edit.time_stretch_ratio),
                1 if note.edit.muted else 0,
            )

    config = SonareNoteRenderConfig(
        _NOTE_STRUCT_VERSION,
        0.0 if fade_ms is None else fade_ms,
    )
    with _out_float_array(lib) as (out, out_length):
        _check(
            lib.sonare_render_notes(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                c_notes,
                ctypes.c_size_t(note_count),
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
        and raise :class:`SonareValueError`, which is both a ``ValueError`` (the
        idiomatic Python contract) and a ``SonareError``. The Node and WASM
        surfaces accept the same valid inputs but delegate rejection to the C++
        core, so an invalid value surfaces there only once it reaches the core.
        The accepted input range is identical across surfaces.
    """
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
