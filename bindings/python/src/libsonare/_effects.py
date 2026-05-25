"""Audio effect wrappers for libsonare."""

from __future__ import annotations

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


def normalize(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    target_db: float = -3.0,
) -> list[float]:
    """Normalize audio to a target dB level.

    Args:
        samples: Audio samples.
        sample_rate: Sample rate in Hz (default 22050).
        target_db: Target peak level in dB (default -3.0).

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
