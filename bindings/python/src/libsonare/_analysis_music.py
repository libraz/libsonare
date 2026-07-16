"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
from collections.abc import Sequence

from ._ffi import (
    SonareChordAnalysisResult,
    SonareChordDetectionOptions,
    SonareMelodyResult,
    SonareSectionResult,
    SonareStringArray,
)
from ._runtime import (
    _check,
    _get_lib,
    _to_c_float_array,
)
from .types import (
    Chord,
    ChordAnalysisResult,
    MelodyPoint,
    MelodyResult,
    Mode,
    PitchClass,
    Section,
    SectionResult,
    SectionType,
)


def detect_chords(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    min_duration: float = 0.3,
    smoothing_window: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = False,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = False,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    key_root: PitchClass = PitchClass.C,
    key_mode: Mode = Mode.MAJOR,
    detect_inversions: bool = False,
    chroma_method: str = "stft",
) -> ChordAnalysisResult:
    """Detect a continuous chord/N.C. timeline.

    ``threshold`` is a final-template correlation cutoff in ``[0, 1]``;
    rejected intervals use quality ``"unknown"`` and ``Chord.name == "N.C."``.
    """
    chroma_method_value = {"stft": 0, "nnls": 1}.get(chroma_method.lower())
    if chroma_method_value is None:
        raise ValueError("chroma_method must be 'stft' or 'nnls'")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareChordAnalysisResult()
    options = SonareChordDetectionOptions(
        min_duration,
        smoothing_window,
        threshold,
        1 if use_triads_only else 0,
        n_fft,
        hop_length,
        1 if use_beat_sync else 0,
        1 if use_hmm else 0,
        hmm_beam_width,
        1 if use_key_context else 0,
        int(key_root),
        int(key_mode),
        1 if detect_inversions else 0,
        chroma_method_value,
    )
    rc = lib.sonare_detect_chords_ex(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc)
    quality_names = {
        0: "major",
        1: "minor",
        2: "diminished",
        3: "augmented",
        4: "dominant7",
        5: "major7",
        6: "minor7",
        7: "sus2",
        8: "sus4",
        9: "unknown",
        10: "add9",
        11: "minorAdd9",
        12: "dim7",
        13: "halfDim7",
        14: "major9",
        15: "dominant9",
        16: "sus2Add4",
    }
    try:
        return ChordAnalysisResult(
            chords=[
                Chord(
                    root=PitchClass(out.chords[i].root),
                    quality=quality_names.get(int(out.chords[i].quality), "unknown"),
                    start=float(out.chords[i].start),
                    end=float(out.chords[i].end),
                    confidence=float(out.chords[i].confidence),
                    bass=PitchClass(out.chords[i].bass),
                )
                for i in range(out.chord_count)
            ]
        )
    finally:
        lib.sonare_free_chord_analysis_result(ctypes.byref(out))


def chord_functional_analysis(
    samples: Sequence[float] | list[float],
    key_root: PitchClass,
    key_mode: Mode = Mode.MAJOR,
    sample_rate: int = 22050,
    min_duration: float = 0.3,
    smoothing_window: float = 2.0,
    threshold: float = 0.5,
    use_triads_only: bool = False,
    n_fft: int = 2048,
    hop_length: int = 512,
    use_beat_sync: bool = True,
    use_hmm: bool = False,
    hmm_beam_width: int = 24,
    use_key_context: bool = False,
    detect_inversions: bool = False,
    chroma_method: str = "stft",
) -> list[str]:
    """Label detected chords with Roman numerals relative to a key.

    Detects chords with the same algorithm as :func:`detect_chords`, then
    returns one Roman-numeral label (e.g. ``"I"``, ``"IV"``, ``"V"``, ``"vi"``)
    per detected chord, in chord order.
    """
    chroma_method_value = {"stft": 0, "nnls": 1}.get(chroma_method.lower())
    if chroma_method_value is None:
        raise ValueError("chroma_method must be 'stft' or 'nnls'")
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareStringArray()
    options = SonareChordDetectionOptions(
        min_duration,
        smoothing_window,
        threshold,
        1 if use_triads_only else 0,
        n_fft,
        hop_length,
        1 if use_beat_sync else 0,
        1 if use_hmm else 0,
        hmm_beam_width,
        1 if use_key_context else 0,
        int(key_root),
        int(key_mode),
        1 if detect_inversions else 0,
        chroma_method_value,
    )
    rc = lib.sonare_chord_functional_analysis(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(options),
        ctypes.c_int32(int(key_root)),
        ctypes.c_int32(int(key_mode)),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return [out.items[i].decode("utf-8") for i in range(out.count)]
    finally:
        lib.sonare_free_string_array(ctypes.byref(out))


def analyze_sections(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    min_section_sec: float = 4.0,
) -> SectionResult:
    """Detect song-structure sections (intro/verse/chorus/...).

    Args:
        samples: Mono audio samples (1D float).
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT window size used for the structural features.
        hop_length: Hop length in samples.
        min_section_sec: Minimum section duration in seconds.

    Returns:
        A :class:`SectionResult` with a list of detected :class:`Section`.

    Note:
        The Python binding deliberately returns a :class:`SectionResult`
        wrapper object (with a ``.sections`` list of :class:`Section`),
        whereas the WASM/Node bindings return a flat array of section
        records. This Pythonic shape is intentional; access the sections via
        ``result.sections`` rather than indexing the result directly.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_analyze_sections"):
        raise RuntimeError("libsonare was built without section-analysis support")
    c_array, length = _to_c_float_array(samples)
    out = SonareSectionResult()
    rc = lib.sonare_analyze_sections(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_float(min_section_sec),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return SectionResult(
            sections=[
                Section(
                    type=SectionType(int(out.sections[i].type)),
                    start=float(out.sections[i].start),
                    end=float(out.sections[i].end),
                    energy_level=float(out.sections[i].energy_level),
                    confidence=float(out.sections[i].confidence),
                )
                for i in range(out.section_count)
            ]
        )
    finally:
        lib.sonare_free_section_result(ctypes.byref(out))


def analyze_melody(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    fmin: float = 65.0,
    fmax: float = 2093.0,
    frame_length: int = 2048,
    hop_length: int = 256,
    threshold: float = 0.1,
    use_pyin: bool = False,
    center: bool = True,
) -> MelodyResult:
    """Extract the melody contour from monophonic audio.

    Args:
        samples: Mono audio samples (1D float).
        sample_rate: Sample rate in Hz (default 22050).
        fmin: Minimum detectable frequency in Hz.
        fmax: Maximum detectable frequency in Hz.
        frame_length: Analysis frame length in samples.
        hop_length: Hop length in samples.
        threshold: YIN/pYIN absolute threshold.
        use_pyin: When ``True``, use pYIN (probabilistic YIN with Viterbi
            smoothing) instead of plain YIN. Requires
            ``sonare_analyze_melody_ex`` in the loaded library; falls back to
            plain YIN on older builds.
        center: When ``True`` (default), apply librosa-style center padding
            so the first frame is centred on sample 0. Requires
            ``sonare_analyze_melody_ex``; older builds ignore this flag.

    Returns:
        A :class:`MelodyResult` with the contour points and summary stats.
    """
    lib = _get_lib()

    # Prefer the extended function when use_pyin or center flags are needed,
    # or when it is the only melody entry point available.
    if hasattr(lib, "sonare_analyze_melody_ex"):
        if not hasattr(lib, "sonare_analyze_melody"):
            pass  # fall through to _ex path below
        c_array, length = _to_c_float_array(samples)
        out = SonareMelodyResult()
        rc = lib.sonare_analyze_melody_ex(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.c_float(fmin),
            ctypes.c_float(fmax),
            ctypes.c_int(frame_length),
            ctypes.c_int(hop_length),
            ctypes.c_float(threshold),
            ctypes.c_int(1 if use_pyin else 0),
            ctypes.c_int(1 if center else 0),
            ctypes.byref(out),
        )
        _check(rc)
        try:
            return MelodyResult(
                points=[
                    MelodyPoint(
                        time=float(out.points[i].time),
                        frequency=float(out.points[i].frequency),
                        confidence=float(out.points[i].confidence),
                    )
                    for i in range(out.point_count)
                ],
                pitch_range_octaves=float(out.pitch_range_octaves),
                pitch_stability=float(out.pitch_stability),
                mean_frequency=float(out.mean_frequency),
                vibrato_rate=float(out.vibrato_rate),
            )
        finally:
            lib.sonare_free_melody_result(ctypes.byref(out))

    if not hasattr(lib, "sonare_analyze_melody"):
        raise RuntimeError("libsonare was built without melody-analysis support")
    c_array, length = _to_c_float_array(samples)
    out = SonareMelodyResult()
    rc = lib.sonare_analyze_melody(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(fmin),
        ctypes.c_float(fmax),
        ctypes.c_int(frame_length),
        ctypes.c_int(hop_length),
        ctypes.c_float(threshold),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return MelodyResult(
            points=[
                MelodyPoint(
                    time=float(out.points[i].time),
                    frequency=float(out.points[i].frequency),
                    confidence=float(out.points[i].confidence),
                )
                for i in range(out.point_count)
            ],
            pitch_range_octaves=float(out.pitch_range_octaves),
            pitch_stability=float(out.pitch_stability),
            mean_frequency=float(out.mean_frequency),
            vibrato_rate=float(out.vibrato_rate),
        )
    finally:
        lib.sonare_free_melody_result(ctypes.byref(out))


def version() -> str:
    """Return the libsonare version string."""
    lib = _get_lib()
    v = lib.sonare_version()
    return v.decode("utf-8") if v else ""


def abi_version() -> int:
    """Return the aggregate libsonare C ABI version.

    Folds every subsystem ABI macro into a single 32-bit value (see
    ``sonare_c.h``), letting a prebuilt binding detect an incompatible native
    library. Returns 0 when the loaded library predates this symbol.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_abi_version"):
        return 0
    return int(lib.sonare_abi_version())


def engine_abi_version() -> int:
    """Return the realtime engine ABI version used for binding compatibility checks."""
    return int(_get_lib().sonare_engine_abi_version())


def voice_changer_abi_version() -> int:
    """Return the realtime voice-changer ABI version.

    Bindings that rely on the flat POD ``SonareRealtimeVoiceChangerConfig``
    layout (Rust FFI, raw C consumers) should call this at attach time and
    compare against the compile-time constant exported by the host library.
    JSON-based callers (this binding) do not need to gate on this; it is
    exposed for parity with the C/Node/WASM surfaces.
    """
    return int(_get_lib().sonare_voice_changer_abi_version())


def has_ffmpeg_support() -> bool:
    """Return whether the loaded libsonare was compiled with FFmpeg support.

    When ``True``, :meth:`Audio.from_file` / :meth:`Audio.from_memory` can
    decode M4A, AAC, FLAC, OGG, Opus and any other container/codec supported
    by the linked FFmpeg. When ``False``, only WAV and MP3 are supported
    and unsupported formats raise an actionable :class:`RuntimeError`.
    """
    lib = _get_lib()
    return bool(lib.sonare_has_ffmpeg_support())
