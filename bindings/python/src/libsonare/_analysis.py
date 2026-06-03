"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Callable, Sequence
from typing import Any

from ._ffi import (
    SonareAcousticResult,
    SonareAnalysisResult,
    SonareAnalyzeProgressCallback,
    SonareBpmAnalysisResult,
    SonareChordAnalysisResult,
    SonareChordDetectionOptions,
    SonareDynamicsResult,
    SonareKey,
    SonareKeyCandidate,
    SonareMelodyResult,
    SonareRhythmResult,
    SonareSectionResult,
    SonareStringArray,
    SonareTimbreResult,
)
from ._runtime import (
    _check,
    _get_lib,
    _mode_values,
    _optional_float_array_result,
    _profile_value,
    _to_c_float_array,
    _to_c_int_array,
)
from .types import (
    AcousticResult,
    AnalysisDynamics,
    AnalysisMelody,
    AnalysisResult,
    AnalysisRhythm,
    AnalysisTimbre,
    BpmAnalysisResult,
    BpmCandidate,
    Chord,
    ChordAnalysisResult,
    DynamicsResult,
    Key,
    KeyCandidate,
    KeyProfile,
    MelodyPoint,
    MelodyResult,
    Mode,
    PitchClass,
    RhythmResult,
    Section,
    SectionResult,
    SectionType,
    TimbreFrame,
    TimbreResult,
    TimeSignature,
)


def detect_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> float:
    """Detect the BPM (tempo) of audio samples.

    One-shot wrapper for raw sample input. When you load from a file or call
    multiple analyses on the same signal, prefer :class:`libsonare.Audio` and
    :meth:`Audio.detect_bpm` to avoid re-copying samples across the FFI.

    Args:
        samples: Mono audio samples (1D, nominally ``[-1.0, 1.0]``). Accepts
            ``list[float]``, ``tuple[float, ...]``, ``array.array``, or a numpy
            1D array of dtype ``float32``.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        Detected BPM as a ``float``. For confidence, use :func:`analyze`.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_bpm = ctypes.c_float()
    rc = lib.sonare_detect_bpm(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out_bpm)
    )
    _check(rc)
    return float(out_bpm.value)


def detect_key(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> Key:
    """Detect the musical key of audio samples.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).
        n_fft: FFT size used for chroma analysis.
        hop_length: Hop length used for chroma analysis.
        use_hpss: Use harmonic-percussive separation before chroma analysis.
        loudness_weighted: Weight chroma frames by RMS loudness.
        high_pass_hz: Optional high-pass cutoff before chroma analysis.
        profile: Optional key-profile family.
        genre_hint: Optional genre hint (``"auto"``, ``"edm"``, ``"pop"``,
            ``"classical"``, or ``"jazz"``).

    Returns:
        :class:`libsonare.Key` with ``root`` (:class:`PitchClass`), ``mode``
        (:class:`Mode`), and ``confidence`` (``float`` in ``[0, 1]``).

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values) if mode_values else (None, 0)
    out_key = SonareKey()
    rc = lib.sonare_detect_key_with_extended_options(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        ctypes.c_float(high_pass_hz),
        mode_array,
        ctypes.c_size_t(mode_count),
        ctypes.c_int32(_profile_value(profile)),
        genre_hint.encode("utf-8") if genre_hint else None,
        ctypes.byref(out_key),
    )
    _check(rc)
    return Key(
        root=PitchClass(out_key.root),
        mode=Mode(out_key.mode),
        confidence=float(out_key.confidence),
    )


def detect_key_candidates(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 4096,
    hop_length: int = 512,
    use_hpss: bool = False,
    loudness_weighted: bool = False,
    high_pass_hz: float = 0.0,
    modes: Sequence[Mode | str] | str | None = None,
    profile: KeyProfile | str | None = None,
    genre_hint: str | None = None,
) -> list[KeyCandidate]:
    """Return ranked musical key candidates for ambiguous material."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    mode_values = _mode_values(modes)
    mode_array, mode_count = _to_c_int_array(mode_values) if mode_values else (None, 0)
    out_candidates = ctypes.POINTER(SonareKeyCandidate)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_key_candidates_with_extended_options(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(1 if use_hpss else 0),
        ctypes.c_int(1 if loudness_weighted else 0),
        ctypes.c_float(high_pass_hz),
        mode_array,
        ctypes.c_size_t(mode_count),
        ctypes.c_int32(_profile_value(profile)),
        genre_hint.encode("utf-8") if genre_hint else None,
        ctypes.byref(out_candidates),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        return [
            KeyCandidate(
                key=Key(
                    root=PitchClass(out_candidates[i].key.root),
                    mode=Mode(out_candidates[i].key.mode),
                    confidence=float(out_candidates[i].key.confidence),
                ),
                correlation=float(out_candidates[i].correlation),
            )
            for i in range(out_count.value)
        ]
    finally:
        if out_candidates and out_count.value > 0:
            lib.sonare_free_key_candidates(out_candidates)


def detect_beats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect beat positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of beat times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_beats(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]
    finally:
        if out_times and out_count.value > 0:
            lib.sonare_free_floats(out_times)


def detect_downbeats(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect downbeat positions in audio samples."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_downbeats(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]
    finally:
        if out_times and out_count.value > 0:
            lib.sonare_free_floats(out_times)


def detect_onsets(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> list[float]:
    """Detect onset positions in audio samples.

    Args:
        samples: Audio samples as a list/sequence of floats.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        List of onset times in seconds.

    Raises:
        RuntimeError: If detection fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out_times = ctypes.POINTER(ctypes.c_float)()
    out_count = ctypes.c_size_t()
    rc = lib.sonare_detect_onsets(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.byref(out_times),
        ctypes.byref(out_count),
    )
    _check(rc)
    try:
        count = out_count.value
        return [float(out_times[i]) for i in range(count)]
    finally:
        if out_times and out_count.value > 0:
            lib.sonare_free_floats(out_times)


# camelCase → snake_case quality name table (mirrors detect_chords).
_QUALITY_NAMES: dict[int, str] = {
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


def _parse_analysis_json(data: dict) -> AnalysisResult:
    """Parse the camelCase JSON dict returned by ``sonare_analyze_json`` into
    an :class:`AnalysisResult`.

    This is an internal helper shared by :func:`analyze` and
    :func:`analyze_with_progress`.
    """
    # Key
    key_d = data.get("key", {})
    key = Key(
        root=PitchClass(int(key_d.get("root", 0))),
        mode=Mode(int(key_d.get("mode", 0))),
        confidence=float(key_d.get("confidence", 0.0)),
    )

    # Time signature (top-level)
    ts_d = data.get("timeSignature", {})
    time_signature = TimeSignature(
        numerator=int(ts_d.get("numerator", 4)),
        denominator=int(ts_d.get("denominator", 4)),
        confidence=float(ts_d.get("confidence", 0.0)),
    )

    # Beats (list of {time, strength})
    beats_raw = data.get("beats", [])
    beat_times = [float(b.get("time", 0.0)) for b in beats_raw]
    beat_strengths = [float(b.get("strength", 0.0)) for b in beats_raw]

    # Chords
    chord_quality_str: dict[str, str] = {
        "major": "major",
        "minor": "minor",
        "diminished": "diminished",
        "augmented": "augmented",
        "dominant7": "dominant7",
        "major7": "major7",
        "minor7": "minor7",
        "sus2": "sus2",
        "sus4": "sus4",
        "unknown": "unknown",
        "add9": "add9",
        "minorAdd9": "minorAdd9",
        "dim7": "dim7",
        "halfDim7": "halfDim7",
        "major9": "major9",
        "dominant9": "dominant9",
        "sus2Add4": "sus2Add4",
    }
    chords: list[Chord] = []
    for c in data.get("chords", []):
        q_raw = c.get("quality", 9)
        # quality field may be int (enum value) or str (name from JSON)
        if isinstance(q_raw, int):
            quality_str = _QUALITY_NAMES.get(q_raw, "unknown")
        else:
            quality_str = chord_quality_str.get(str(q_raw), "unknown")
        chords.append(
            Chord(
                root=PitchClass(int(c.get("root", 0))),
                bass=PitchClass(int(c.get("bass", c.get("root", 0)))),
                quality=quality_str,
                start=float(c.get("start", 0.0)),
                end=float(c.get("end", 0.0)),
                confidence=float(c.get("confidence", 0.0)),
            )
        )

    # Sections
    sections: list[Section] = []
    for s in data.get("sections", []):
        sections.append(
            Section(
                type=SectionType(int(s.get("type", 7))),
                start=float(s.get("start", 0.0)),
                end=float(s.get("end", 0.0)),
                energy_level=float(s.get("energyLevel", 0.0)),
                confidence=float(s.get("confidence", 0.0)),
            )
        )

    # Timbre
    timbre_d = data.get("timbre", {})
    timbre = (
        AnalysisTimbre(
            brightness=float(timbre_d.get("brightness", 0.0)),
            warmth=float(timbre_d.get("warmth", 0.0)),
            density=float(timbre_d.get("density", 0.0)),
            roughness=float(timbre_d.get("roughness", 0.0)),
            complexity=float(timbre_d.get("complexity", 0.0)),
        )
        if timbre_d
        else None
    )

    # Dynamics
    dyn_d = data.get("dynamics", {})
    dynamics = (
        AnalysisDynamics(
            dynamic_range_db=float(dyn_d.get("dynamicRangeDb", 0.0)),
            peak_db=float(dyn_d.get("peakDb", 0.0)),
            rms_db=float(dyn_d.get("rmsDb", 0.0)),
            crest_factor=float(dyn_d.get("crestFactor", 0.0)),
            loudness_range_db=float(dyn_d.get("loudnessRangeDb", 0.0)),
            is_compressed=bool(dyn_d.get("isCompressed", False)),
        )
        if dyn_d
        else None
    )

    # Rhythm
    rhy_d = data.get("rhythm", {})
    if rhy_d:
        rts_d = rhy_d.get("timeSignature", ts_d)
        rhythm = AnalysisRhythm(
            time_signature=TimeSignature(
                numerator=int(rts_d.get("numerator", 4)),
                denominator=int(rts_d.get("denominator", 4)),
                confidence=float(rts_d.get("confidence", 0.0)),
            ),
            syncopation=float(rhy_d.get("syncopation", 0.0)),
            groove_type=str(rhy_d.get("grooveType", "straight")),
            pattern_regularity=float(rhy_d.get("patternRegularity", 0.0)),
            tempo_stability=float(rhy_d.get("tempoStability", 0.0)),
        )
    else:
        rhythm = None

    # Melody
    mel_d = data.get("melody", {})
    if mel_d:
        pitches = [
            MelodyPoint(
                time=float(p.get("time", 0.0)),
                frequency=float(p.get("frequency", 0.0)),
                confidence=float(p.get("confidence", 0.0)),
            )
            for p in mel_d.get("pitches", [])
        ]
        melody = AnalysisMelody(
            pitch_range_octaves=float(mel_d.get("pitchRangeOctaves", 0.0)),
            pitch_stability=float(mel_d.get("pitchStability", 0.0)),
            mean_frequency=float(mel_d.get("meanFrequency", 0.0)),
            vibrato_rate=float(mel_d.get("vibratoRate", 0.0)),
            pitches=pitches,
        )
    else:
        melody = None

    return AnalysisResult(
        bpm=float(data.get("bpm", 0.0)),
        bpm_confidence=float(data.get("bpmConfidence", 0.0)),
        key=key,
        time_signature=time_signature,
        beat_times=beat_times,
        beat_strengths=beat_strengths,
        chords=chords,
        sections=sections,
        timbre=timbre,
        dynamics=dynamics,
        rhythm=rhythm,
        melody=melody,
        form=str(data.get("form", "")),
    )


def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    Calls ``sonare_analyze_json`` when available to return the complete
    analysis result; falls back to ``sonare_analyze`` for older builds.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).

    Returns:
        :class:`libsonare.AnalysisResult` with all fields:

        - ``bpm`` (``float``) and ``bpm_confidence`` (``float`` in ``[0, 1]``)
        - ``key`` (:class:`Key`), ``time_signature`` (:class:`TimeSignature`)
        - ``beat_times`` (``list[float]`` in seconds)
        - ``beat_strengths`` (``list[float]``) — per-beat strength values
        - ``chords`` (``list[Chord]``) — detected chord segments
        - ``sections`` (``list[Section]``) — detected structural sections
        - ``timbre`` (:class:`AnalysisTimbre`) — spectral character summary
        - ``dynamics`` (:class:`AnalysisDynamics`) — loudness/dynamics summary
        - ``rhythm`` (:class:`AnalysisRhythm`) — groove/syncopation summary
        - ``melody`` (:class:`AnalysisMelody`) — melody contour and statistics
        - ``form`` (``str``) — overall form classification

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)

    if hasattr(lib, "sonare_analyze_json"):
        out_json = ctypes.c_char_p()
        rc = lib.sonare_analyze_json(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.byref(out_json),
        )
        _check(rc)
        try:
            raw = out_json.value
            data = json.loads(raw.decode("utf-8") if raw else "{}")
        finally:
            if out_json.value and hasattr(lib, "sonare_free_string"):
                lib.sonare_free_string(out_json)
        return _parse_analysis_json(data)

    # Fallback for builds that only have the older flat struct API.
    out = SonareAnalysisResult()
    rc = lib.sonare_analyze(
        c_array, ctypes.c_size_t(length), ctypes.c_int(sample_rate), ctypes.byref(out)
    )
    _check(rc)
    try:
        beat_times = [float(out.beat_times[i]) for i in range(out.beat_count)]
        return AnalysisResult(
            bpm=float(out.bpm),
            bpm_confidence=float(out.bpm_confidence),
            key=Key(
                root=PitchClass(out.key.root),
                mode=Mode(out.key.mode),
                confidence=float(out.key.confidence),
            ),
            time_signature=TimeSignature(
                numerator=int(out.time_signature.numerator),
                denominator=int(out.time_signature.denominator),
                confidence=float(out.time_signature.confidence),
            ),
            beat_times=beat_times,
        )
    finally:
        lib.sonare_free_result(ctypes.byref(out))


def _make_analyze_progress_trampoline(
    on_progress: Callable[[float, str], None],
) -> Any:
    """Wrap a Python callback for use as a C SonareAnalyzeProgressCallback.

    The returned object MUST be kept alive across the C call to avoid GC
    collecting the underlying ctypes closure.
    """

    def _trampoline(progress: float, stage_cstr: bytes | None, _user_data: int) -> None:
        try:
            stage = stage_cstr.decode("utf-8") if stage_cstr else ""
            on_progress(float(progress), stage)
        except Exception:  # noqa: BLE001 — never propagate Python exceptions into C
            pass

    return SonareAnalyzeProgressCallback(_trampoline)


def analyze_with_progress(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    on_progress: Callable[[float, str], None] | None = None,
) -> AnalysisResult:
    """Run full audio analysis with optional progress callbacks.

    Calls ``sonare_analyze_json_with_progress``. The ``on_progress`` callable
    is invoked periodically during analysis with a progress fraction ``[0, 1]``
    and a stage name string. Falls back to :func:`analyze` if the extended
    symbol is absent in the loaded library.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).
        on_progress: Optional callable ``(progress: float, stage: str) -> None``
            invoked during analysis. Progress is in ``[0, 1]``. If ``None``,
            no callback is registered.

    Returns:
        Same rich :class:`libsonare.AnalysisResult` as :func:`analyze`.

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    if not hasattr(lib, "sonare_analyze_json_with_progress"):
        return analyze(samples, sample_rate=sample_rate)

    c_array, length = _to_c_float_array(samples)

    if on_progress is not None:
        # The trampoline must stay alive across the C call so the ctypes
        # closure is not garbage-collected mid-analysis.
        c_cb = _make_analyze_progress_trampoline(on_progress)
    else:
        c_cb = SonareAnalyzeProgressCallback(0)  # null callback

    out_json = ctypes.c_char_p()
    rc = lib.sonare_analyze_json_with_progress(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        c_cb,
        None,
        ctypes.byref(out_json),
    )
    _check(rc)
    try:
        raw = out_json.value
        data = json.loads(raw.decode("utf-8") if raw else "{}")
    finally:
        if out_json.value and hasattr(lib, "sonare_free_string"):
            lib.sonare_free_string(out_json)
    return _parse_analysis_json(data)


def analyze_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    bpm_min: float = 30.0,
    bpm_max: float = 300.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
    max_candidates: int = 5,
) -> BpmAnalysisResult:
    """Analyze BPM with confidence, candidates, autocorrelation, and tempogram.

    Note:
        ``bpm_min`` defaults to 30.0 here (lower than :func:`analyze_rhythm`'s
        60.0). This wider search range lets the full BPM analyzer surface
        half-tempo candidates and very slow material in its candidate list;
        :func:`analyze_rhythm` keeps the narrower 60.0 floor for a single,
        more stable tempo estimate. This difference is intentional.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareBpmAnalysisResult()
    rc = lib.sonare_analyze_bpm(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(bpm_min),
        ctypes.c_float(bpm_max),
        ctypes.c_float(start_bpm),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(max_candidates),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return BpmAnalysisResult(
            bpm=float(out.bpm),
            confidence=float(out.confidence),
            candidates=[
                BpmCandidate(
                    bpm=float(out.candidates[i].bpm),
                    confidence=float(out.candidates[i].confidence),
                )
                for i in range(out.candidate_count)
            ],
            autocorrelation=[
                float(out.autocorrelation[i]) for i in range(out.autocorrelation_count)
            ],
            tempogram=[float(out.tempogram[i]) for i in range(out.tempogram_count)],
        )
    finally:
        lib.sonare_free_bpm_analysis_result(ctypes.byref(out))


def analyze_impulse_response(
    samples: Sequence[float] | list[float],
    sample_rate: int = 48000,
    n_octave_bands: int = 6,
) -> AcousticResult:
    """Analyze RT60, EDT, and clarity metrics from an impulse response."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAcousticResult()
    rc = lib.sonare_analyze_impulse_response(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_octave_bands),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = out.band_count
        return AcousticResult(
            rt60=float(out.rt60),
            edt=float(out.edt),
            c50=float(out.c50),
            c80=float(out.c80),
            d50=float(out.d50),
            rt60_bands=[float(out.rt60_bands[i]) for i in range(count)],
            edt_bands=[float(out.edt_bands[i]) for i in range(count)],
            c50_bands=_optional_float_array_result(out.c50_bands, count),
            c80_bands=_optional_float_array_result(out.c80_bands, count),
            confidence=float(out.confidence),
            is_blind=bool(out.is_blind),
        )
    finally:
        lib.sonare_free_acoustic_result(ctypes.byref(out))


def detect_acoustic(
    samples: Sequence[float] | list[float],
    sample_rate: int = 48000,
    n_octave_bands: int = 6,
    n_third_octave_subbands: int = 24,
    min_decay_db: float = 30.0,
    noise_floor_margin_db: float = 10.0,
) -> AcousticResult:
    """Estimate blind RT60/EDT acoustic parameters from ordinary audio."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAcousticResult()
    rc = lib.sonare_detect_acoustic(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_octave_bands),
        ctypes.c_int(n_third_octave_subbands),
        ctypes.c_float(min_decay_db),
        ctypes.c_float(noise_floor_margin_db),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = out.band_count
        return AcousticResult(
            rt60=float(out.rt60),
            edt=float(out.edt),
            c50=float(out.c50),
            c80=float(out.c80),
            d50=float(out.d50),
            rt60_bands=[float(out.rt60_bands[i]) for i in range(count)],
            edt_bands=[float(out.edt_bands[i]) for i in range(count)],
            c50_bands=_optional_float_array_result(out.c50_bands, count),
            c80_bands=_optional_float_array_result(out.c80_bands, count),
            confidence=float(out.confidence),
            is_blind=bool(out.is_blind),
        )
    finally:
        lib.sonare_free_acoustic_result(ctypes.byref(out))


def analyze_rhythm(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    bpm_min: float = 60.0,
    bpm_max: float = 200.0,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> RhythmResult:
    """Analyze rhythm primitives without generating a summary report.

    Note:
        ``bpm_min`` defaults to 60.0 here (higher than :func:`analyze_bpm`'s
        30.0). The narrower search range biases the single tempo estimate
        toward the musically common 60-200 BPM band and avoids half-tempo
        lock; :func:`analyze_bpm` uses the lower 30.0 floor so its candidate
        list can include slow/half-tempo options. This difference is
        intentional.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareRhythmResult()
    rc = lib.sonare_analyze_rhythm(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(bpm_min),
        ctypes.c_float(bpm_max),
        ctypes.c_float(start_bpm),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.byref(out),
    )
    _check(rc)
    groove_names = {0: "straight", 1: "shuffle", 2: "swing"}
    try:
        return RhythmResult(
            bpm=float(out.bpm),
            time_signature=TimeSignature(
                numerator=int(out.time_signature.numerator),
                denominator=int(out.time_signature.denominator),
                confidence=float(out.time_signature.confidence),
            ),
            groove_type=groove_names.get(int(out.groove_type), "straight"),
            syncopation=float(out.syncopation),
            pattern_regularity=float(out.pattern_regularity),
            tempo_stability=float(out.tempo_stability),
            beat_intervals=[float(out.beat_intervals[i]) for i in range(out.beat_interval_count)],
        )
    finally:
        lib.sonare_free_rhythm_result(ctypes.byref(out))


def analyze_dynamics(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    window_sec: float = 0.4,
    hop_length: int = 512,
    compression_threshold: float = 6.0,
) -> DynamicsResult:
    """Analyze dynamics and loudness primitives."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareDynamicsResult()
    rc = lib.sonare_analyze_dynamics(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_float(window_sec),
        ctypes.c_int(hop_length),
        ctypes.c_float(compression_threshold),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return DynamicsResult(
            dynamic_range_db=float(out.dynamic_range_db),
            peak_db=float(out.peak_db),
            rms_db=float(out.rms_db),
            crest_factor=float(out.crest_factor),
            loudness_range_db=float(out.loudness_range_db),
            is_compressed=bool(out.is_compressed),
            loudness_times=[float(out.loudness_times[i]) for i in range(out.loudness_count)],
            loudness_rms_db=[float(out.loudness_rms_db[i]) for i in range(out.loudness_count)],
        )
    finally:
        lib.sonare_free_dynamics_result(ctypes.byref(out))


def analyze_timbre(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 13,
    window_sec: float = 0.5,
) -> TimbreResult:
    """Analyze timbre and spectral-shape primitives."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareTimbreResult()
    rc = lib.sonare_analyze_timbre(
        c_array,
        ctypes.c_size_t(length),
        ctypes.c_int(sample_rate),
        ctypes.c_int(n_fft),
        ctypes.c_int(hop_length),
        ctypes.c_int(n_mels),
        ctypes.c_int(n_mfcc),
        ctypes.c_float(window_sec),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return TimbreResult(
            brightness=float(out.brightness),
            warmth=float(out.warmth),
            density=float(out.density),
            roughness=float(out.roughness),
            complexity=float(out.complexity),
            spectral_centroid=[
                float(out.spectral_centroid[i]) for i in range(out.spectral_centroid_count)
            ],
            spectral_flatness=[
                float(out.spectral_flatness[i]) for i in range(out.spectral_flatness_count)
            ],
            spectral_rolloff=[
                float(out.spectral_rolloff[i]) for i in range(out.spectral_rolloff_count)
            ],
            timbre_over_time=[
                TimbreFrame(
                    brightness=float(out.timbre_over_time[i].brightness),
                    warmth=float(out.timbre_over_time[i].warmth),
                    density=float(out.timbre_over_time[i].density),
                    roughness=float(out.timbre_over_time[i].roughness),
                    complexity=float(out.timbre_over_time[i].complexity),
                )
                for i in range(out.timbre_over_time_count)
            ],
        )
    finally:
        lib.sonare_free_timbre_result(ctypes.byref(out))


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
    """Detect chord segments without generating a summary report."""
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
