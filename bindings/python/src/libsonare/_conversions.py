"""Core conversion and utility wrappers for libsonare."""

from __future__ import annotations

from ._runtime import *  # noqa: F403


def hz_to_mel(hz: float) -> float:
    """Convert frequency in Hz to Mel scale."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_mel(ctypes.c_float(hz)))


def mel_to_hz(mel: float) -> float:
    """Convert Mel scale value to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_mel_to_hz(ctypes.c_float(mel)))


def hz_to_midi(hz: float) -> float:
    """Convert frequency in Hz to MIDI note number."""
    lib = _get_lib()
    return float(lib.sonare_hz_to_midi(ctypes.c_float(hz)))


def midi_to_hz(midi: float) -> float:
    """Convert MIDI note number to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_midi_to_hz(ctypes.c_float(midi)))


def hz_to_note(hz: float) -> str:
    """Convert frequency in Hz to note name (e.g. 'A4')."""
    lib = _get_lib()
    result = lib.sonare_hz_to_note(ctypes.c_float(hz))
    return result.decode("utf-8") if result else ""


def note_to_hz(note: str) -> float:
    """Convert note name (e.g. 'A4') to frequency in Hz."""
    lib = _get_lib()
    return float(lib.sonare_note_to_hz(note.encode("utf-8")))


def frames_to_time(frames: int, sr: int = 22050, hop_length: int = 512) -> float:
    """Convert frame count to time in seconds."""
    lib = _get_lib()
    return float(
        lib.sonare_frames_to_time(ctypes.c_int(frames), ctypes.c_int(sr), ctypes.c_int(hop_length))
    )


def time_to_frames(time: float, sr: int = 22050, hop_length: int = 512) -> int:
    """Convert time in seconds to frame count."""
    lib = _get_lib()
    return int(
        lib.sonare_time_to_frames(ctypes.c_float(time), ctypes.c_int(sr), ctypes.c_int(hop_length))
    )


def frames_to_samples(frames: int, hop_length: int = 512, n_fft: int = 0) -> int:
    """Convert frame count to sample index."""
    lib = _get_lib()
    return int(
        lib.sonare_frames_to_samples(
            ctypes.c_int(frames), ctypes.c_int(hop_length), ctypes.c_int(n_fft)
        )
    )


def samples_to_frames(samples: int, hop_length: int = 512, n_fft: int = 0) -> int:
    """Convert sample index to frame count."""
    lib = _get_lib()
    return int(
        lib.sonare_samples_to_frames(
            ctypes.c_int(samples), ctypes.c_int(hop_length), ctypes.c_int(n_fft)
        )
    )


def power_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-10,
    top_db: float = 80.0,
) -> list[float]:
    """Convert power values to dB."""
    return _call_float_transform(
        "sonare_power_to_db",
        values,
        ctypes.c_float(ref),
        ctypes.c_float(amin),
        ctypes.c_float(top_db),
    )


def amplitude_to_db(
    values: Sequence[float] | list[float],
    ref: float = 1.0,
    amin: float = 1e-5,
    top_db: float = 80.0,
) -> list[float]:
    """Convert amplitude values to dB."""
    return _call_float_transform(
        "sonare_amplitude_to_db",
        values,
        ctypes.c_float(ref),
        ctypes.c_float(amin),
        ctypes.c_float(top_db),
    )


def db_to_power(values: Sequence[float] | list[float], ref: float = 1.0) -> list[float]:
    """Convert dB values back to power."""
    return _call_float_transform("sonare_db_to_power", values, ctypes.c_float(ref))


def db_to_amplitude(values: Sequence[float] | list[float], ref: float = 1.0) -> list[float]:
    """Convert dB values back to amplitude."""
    return _call_float_transform("sonare_db_to_amplitude", values, ctypes.c_float(ref))


def preemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> list[float]:
    """Apply librosa-compatible pre-emphasis."""
    return _call_float_transform(
        "sonare_preemphasis",
        samples,
        ctypes.c_float(coef),
        ctypes.c_float(0.0 if zi is None else zi),
        ctypes.c_int(0 if zi is None else 1),
    )


def deemphasis(
    samples: Sequence[float] | list[float],
    coef: float = 0.97,
    zi: float | None = None,
) -> list[float]:
    """Apply inverse pre-emphasis."""
    return _call_float_transform(
        "sonare_deemphasis",
        samples,
        ctypes.c_float(coef),
        ctypes.c_float(0.0 if zi is None else zi),
        ctypes.c_int(0 if zi is None else 1),
    )


def trim_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> tuple[list[float], int, int]:
    """Trim leading/trailing silence and return (audio, start_sample, end_sample)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    start = ctypes.c_int()
    end = ctypes.c_int()
    rc = lib.sonare_trim_silence(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_float(top_db),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(start),
        ctypes.byref(end),
    )
    _check(rc)
    try:
        return (_float_array_result(out, out_length.value), int(start.value), int(end.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def split_silence(
    samples: Sequence[float] | list[float],
    top_db: float = 60.0,
    frame_length: int = 2048,
    hop_length: int = 512,
) -> list[tuple[int, int]]:
    """Return non-silent intervals as (start_sample, end_sample)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_int)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_split_silence(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_float(top_db),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        flat = _int_array_result(out, out_count.value)
        return [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]
    finally:
        if out and out_count.value > 0:
            lib.sonare_free_ints(out)


def frame_signal(
    samples: Sequence[float] | list[float],
    frame_length: int,
    hop_length: int,
) -> tuple[int, list[float]]:
    """Slice a signal into frames. Returns (n_frames, row-major frames)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_frame_signal(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def pad_center(
    values: Sequence[float] | list[float],
    target_size: int,
    pad_value: float = 0.0,
) -> list[float]:
    """Pad an array by centering it within target size."""
    return _call_float_transform(
        "sonare_pad_center", values, ctypes.c_size_t(target_size), ctypes.c_float(pad_value)
    )


def fix_length(
    values: Sequence[float] | list[float],
    target_size: int,
    pad_value: float = 0.0,
) -> list[float]:
    """Crop or pad an array to exact length."""
    return _call_float_transform(
        "sonare_fix_length", values, ctypes.c_size_t(target_size), ctypes.c_float(pad_value)
    )


def fix_frames(
    frames: Sequence[int] | list[int],
    x_min: int = 0,
    x_max: int = -1,
    pad: bool = True,
) -> list[int]:
    """Adjust frame indices to fit within bounds."""
    lib = _get_lib()
    c_array, length = _to_c_int_array(frames)
    out = ctypes.POINTER(ctypes.c_int)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_fix_frames(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(x_min),
        ctypes.c_int(x_max),
        ctypes.c_int(1 if pad else 0),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _int_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_ints(out)


def peak_pick(
    values: Sequence[float] | list[float],
    pre_max: int,
    post_max: int,
    pre_avg: int,
    post_avg: int,
    delta: float,
    wait: int,
) -> list[int]:
    """Pick peaks using librosa.util.peak_pick-compatible parameters."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    out = ctypes.POINTER(ctypes.c_int)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_peak_pick(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(pre_max),
        ctypes.c_int(post_max),
        ctypes.c_int(pre_avg),
        ctypes.c_int(post_avg),
        ctypes.c_float(delta),
        ctypes.c_int(wait),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _int_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_ints(out)


def vector_normalize(
    values: Sequence[float] | list[float],
    norm_type: int = 0,
    threshold: float = 0.0,
) -> list[float]:
    """Normalize a vector. norm_type: 0=inf, 1=L1, 2=L2, 3=power."""
    return _call_float_transform(
        "sonare_vector_normalize",
        values,
        ctypes.c_int(norm_type),
        ctypes.c_float(threshold),
    )


def pcen(
    values: Sequence[float] | list[float],
    n_bins: int,
    n_frames: int,
    sample_rate: int = 22050,
    hop_length: int = 512,
    time_constant: float = 0.4,
    gain: float = 0.98,
    bias: float = 2.0,
    power: float = 0.5,
    eps: float = 1e-6,
) -> list[float]:
    """Apply per-channel energy normalization to a row-major spectrogram."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(values)
    if length != n_bins * n_frames:
        raise ValueError("values length must equal n_bins * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_pcen(
        c_array,
        ctypes.c_int(n_bins),
        ctypes.c_int(n_frames),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(time_constant),
        ctypes.c_float(gain),
        ctypes.c_float(bias),
        ctypes.c_float(power),
        ctypes.c_float(eps),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def tonnetz(chromagram: Sequence[float] | list[float], n_chroma: int, n_frames: int) -> list[float]:
    """Compute Tonnetz from row-major chromagram data."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(chromagram)
    if length != n_chroma * n_frames:
        raise ValueError("chromagram length must equal n_chroma * n_frames")
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_tonnetz(
        c_array,
        ctypes.c_int(n_chroma),
        ctypes.c_int(n_frames),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def tempogram(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
    mode: str = "autocorrelation",
) -> tuple[int, list[float]]:
    """Compute tempogram. Returns (n_frames, row-major matrix)."""
    mode_id = {"autocorrelation": 0, "auto": 0, "ac": 0, "cosine": 1}.get(mode)
    if mode_id is None:
        raise ValueError("mode must be 'autocorrelation' or 'cosine'")
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_tempogram_with_mode(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_int(win_length),
        ctypes.c_int(1 if center else 0),
        ctypes.c_int(1 if norm else 0),
        ctypes.c_int(mode_id),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def cyclic_tempogram(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    bpm_min: float = 60.0,
    n_bins: int = 60,
) -> tuple[int, list[float]]:
    """Compute cyclic tempogram. Returns (n_frames, row-major matrix)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_cyclic_tempogram(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_int(win_length),
        ctypes.c_float(bpm_min),
        ctypes.c_int(n_bins),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def plp(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    tempo_min: float = 30.0,
    tempo_max: float = 300.0,
    win_length: int = 384,
) -> list[float]:
    """Compute predominant local pulse from an onset envelope."""
    return _call_float_transform(
        "sonare_plp",
        onset_envelope,
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_float(tempo_min),
        ctypes.c_float(tempo_max),
        ctypes.c_int(win_length),
    )


def onset_envelope(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
) -> list[float]:
    """Compute the onset strength envelope (librosa.onset.onset_strength).

    Returns one value per frame (half-wave rectified Mel-spectrogram flux).
    """
    return _call_float_transform(
        "sonare_onset_strength",
        samples,
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
    )


def fourier_tempogram(
    onset_envelope: Sequence[float] | list[float],
    sample_rate: int = 22050,
    hop_length: int = 512,
    win_length: int = 384,
    center: bool = True,
    norm: bool = True,
) -> tuple[int, list[float]]:
    """Compute the Fourier (FFT-based) tempogram. Returns (n_frames, matrix).

    The matrix is row-major [n_bins x n_frames] where
    n_bins = len(matrix) // n_frames.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(onset_envelope)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_fourier_tempogram(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        ctypes.c_int(win_length),
        ctypes.c_int(1 if center else 0),
        ctypes.c_int(1 if norm else 0),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def tempogram_ratio(
    tempogram_data: Sequence[float] | list[float],
    win_length: int = 384,
    sample_rate: int = 22050,
    hop_length: int = 512,
    factors: Sequence[float] | list[float] | None = None,
) -> list[float]:
    """Aggregate tempogram magnitudes at integer ratios of a reference tempo.

    When ``factors`` is None the library default factors are used.
    Returns one value per factor.
    """
    if factors is None:
        factors_ptr: ctypes.Array[ctypes.c_float] | None = None
        n_factors = 0
    else:
        factors_ptr, n_factors = _to_c_float_array(factors)
    return _call_float_transform(
        "sonare_tempogram_ratio",
        tempogram_data,
        ctypes.c_int(win_length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(hop_length),
        factors_ptr,
        ctypes.c_size_t(n_factors),
    )


def nnls_chroma(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> tuple[int, list[float]]:
    """Compute NNLS chroma. Returns (n_frames, row-major 12 x n_frames matrix)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    n_frames = ctypes.c_int()
    rc = lib.sonare_nnls_chroma(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out),
        ctypes.byref(out_length),
        ctypes.byref(n_frames),
    )
    _check(rc)
    try:
        return (int(n_frames.value), _float_array_result(out, out_length.value))
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)


def resample(
    samples: Sequence[float] | list[float],
    src_sr: int,
    target_sr: int,
) -> list[float]:
    """Resample audio to a different sample rate.

    Args:
        samples: Audio samples.
        src_sr: Source sample rate in Hz.
        target_sr: Target sample rate in Hz.

    Returns:
        List of resampled audio samples.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = ctypes.POINTER(ctypes.c_float)()
    out_length = ctypes.c_size_t()
    rc = lib.sonare_resample(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(src_sr),
        ctypes.c_int(target_sr),
        ctypes.byref(out),
        ctypes.byref(out_length),
    )
    _check(rc)
    try:
        return _float_array_result(out, out_length.value)
    finally:
        if out and out_length.value > 0:
            lib.sonare_free_floats(out)
