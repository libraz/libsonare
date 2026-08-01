"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
from collections.abc import Callable, Sequence
from typing import Any, Literal, cast

from ._analysis_detection import _parse_analysis_json
from ._cancellation import CancellationState, make_cancel_trampoline
from ._ffi import (
    SonareAcousticResult,
    SonareAnalysisResult,
    SonareAnalyzeProgressCallback,
    SonareBpmAnalysisResult,
    SonareDynamicsResult,
    SonareMusicAnalyzeOptions,
    SonareRhythmResult,
    SonareTimbreResult,
)
from ._runtime import (
    _check,
    _get_lib,
    _optional_float_array_result,
    _to_c_float_array,
)
from .types import (
    AcousticResult,
    AnalysisResult,
    BpmAnalysisResult,
    BpmCandidate,
    BpmHypothesis,
    DynamicsResult,
    Key,
    Mode,
    PitchClass,
    RhythmResult,
    TimbreFrame,
    TimbreResult,
    TimeSignature,
)


def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    bpm_min: float = 60.0,
    bpm_max: float = 200.0,
    start_bpm: float = 120.0,
    use_triads_only: bool = True,
    use_hpss: bool = True,
    chroma_highpass_hz: float = 80.0,
    use_bass_weighted: bool = True,
    chroma_hop_multiplier: int = 4,
    use_chord_hmm: bool = False,
    use_chord_key_context: bool = False,
    chord_hmm_beam_width: int = 24,
    detect_chord_inversions: bool = False,
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
        if hasattr(lib, "sonare_analyze_json_ex"):
            options = SonareMusicAnalyzeOptions(
                n_fft=n_fft,
                hop_length=hop_length,
                bpm_min=bpm_min,
                bpm_max=bpm_max,
                start_bpm=start_bpm,
                use_triads_only=int(use_triads_only),
                use_hpss=int(use_hpss),
                chroma_highpass_hz=chroma_highpass_hz,
                use_bass_weighted=int(use_bass_weighted),
                chroma_hop_multiplier=chroma_hop_multiplier,
                use_chord_hmm=int(use_chord_hmm),
                use_chord_key_context=int(use_chord_key_context),
                chord_hmm_beam_width=chord_hmm_beam_width,
                detect_chord_inversions=int(detect_chord_inversions),
            )
            rc = lib.sonare_analyze_json_ex(
                c_array,
                ctypes.c_size_t(length),
                ctypes.c_int(sample_rate),
                ctypes.byref(options),
                ctypes.byref(out_json),
            )
        else:
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
        relation_names = ("primary", "half", "double", "other")
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
            bpm_candidates=[
                BpmHypothesis(
                    value=float(out.bpm_candidates[i].value),
                    confidence=float(out.bpm_candidates[i].confidence),
                    relation=cast(
                        Literal["primary", "half", "double", "other"],
                        relation_names[out.bpm_candidates[i].relation]
                        if 0 <= out.bpm_candidates[i].relation < len(relation_names)
                        else "other",
                    ),
                )
                for i in range(out.bpm_candidate_count)
            ],
            time_signature_candidates=[
                TimeSignature(
                    numerator=int(out.time_signature_candidates[i].numerator),
                    denominator=int(out.time_signature_candidates[i].denominator),
                    confidence=float(out.time_signature_candidates[i].confidence),
                )
                for i in range(out.time_signature_candidate_count)
            ],
        )
    finally:
        lib.sonare_free_result(ctypes.byref(out))


def _make_analyze_progress_trampoline(
    on_progress: Callable[[float, str], object], state: CancellationState
) -> Any:
    """Wrap a Python callback for use as a C SonareAnalyzeProgressCallback.

    The returned object MUST be kept alive across the C call to avoid GC
    collecting the underlying ctypes closure.
    """

    def _trampoline(progress: float, stage_cstr: bytes | None, _user_data: int) -> None:
        try:
            stage = stage_cstr.decode("utf-8") if stage_cstr else ""
            state.observe_progress_result(on_progress(float(progress), stage))
        except Exception:  # noqa: BLE001 — never propagate Python exceptions into C
            pass

    return SonareAnalyzeProgressCallback(_trampoline)


def analyze_with_progress(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    on_progress: Callable[[float, str], object] | None = None,
    *,
    cancel: Callable[[], bool] | None = None,
) -> AnalysisResult:
    """Run full audio analysis with optional progress callbacks.

    Calls ``sonare_analyze_json_with_progress_ex``. The ``on_progress`` callable
    is invoked periodically during analysis with a progress fraction ``[0, 1]``
    and a stage name string. Calls the cancellation-capable ABI whenever a
    progress or cancellation callback is supplied; falls back to
    :func:`analyze` if neither progress entry point exists in the loaded library.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).
        on_progress: Optional callable ``(progress: float, stage: str) -> object``
            invoked during analysis. Progress is in ``[0, 1]``. Returning
            exactly ``False`` requests cancellation; ``None`` and every other
            return value continue. If ``None``, no callback is registered.
        cancel: Optional zero-argument callable polled by the native operation.
            A true return value requests cancellation.

    Returns:
        Same rich :class:`libsonare.AnalysisResult` as :func:`analyze`.

    Raises:
        RuntimeError: If analysis fails.
    """
    lib = _get_lib()
    has_progress = hasattr(lib, "sonare_analyze_json_with_progress")
    has_progress_ex = hasattr(lib, "sonare_analyze_json_with_progress_ex")
    if not has_progress and not has_progress_ex:
        if cancel is not None:
            raise RuntimeError("loaded libsonare does not support analysis cancellation")
        return analyze(samples, sample_rate=sample_rate)

    c_array, length = _to_c_float_array(samples)
    out_json = ctypes.c_char_p()
    if has_progress_ex and (on_progress is not None or cancel is not None or not has_progress):
        state = CancellationState(cancel)
        c_cb = (
            _make_analyze_progress_trampoline(on_progress, state)
            if on_progress is not None
            else SonareAnalyzeProgressCallback(0)
        )
        cancel_cb = make_cancel_trampoline(state)
        rc = lib.sonare_analyze_json_with_progress_ex(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            c_cb,
            None,
            ctypes.byref(out_json),
            cancel_cb,
            None,
        )
    else:
        if cancel is not None:
            raise RuntimeError("loaded libsonare does not support analysis cancellation")
        c_cb = (
            _make_analyze_progress_trampoline(on_progress, CancellationState(None))
            if on_progress is not None
            else SonareAnalyzeProgressCallback(0)
        )
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
