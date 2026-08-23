"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
import math
import operator
from collections.abc import Callable, Sequence
from typing import Any, Literal, cast

from ._analysis_detection import _parse_analysis_json
from ._cancellation import CancellationState, make_cancel_trampoline
from ._ffi import (
    SONARE_MAX_METER_CANDIDATE_NUMERATORS,
    SonareAcousticResult,
    SonareAnalysisResult,
    SonareAnalyzeProgressCallback,
    SonareBpmAnalysisResult,
    SonareDynamicsResult,
    SonareMeterOptions,
    SonareMusicAnalyzeOptions,
    SonareRhythmResult,
    SonareTimbreResult,
)
from ._runtime import (
    ErrorCode,
    SonareError,
    SonareValueError,
    _check,
    _get_lib,
    _guard_buffer,
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
    MeterEstimate,
    Mode,
    PitchClass,
    RhythmResult,
    TimbreFrame,
    TimbreResult,
    TimeSignature,
)

# Meter numerators the estimator scores when the caller passes none. Mirrors
# MusicAnalyzerConfig::meter_candidate_numerators, so a default-argument
# analyze() reproduces the native default rather than a Python-side guess.
_DEFAULT_METER_CANDIDATE_NUMERATORS = (3, 4, 6)


def _unsupported_feature_symbol(symbol: str) -> SonareError:
    return SonareError(
        int(ErrorCode.NOT_SUPPORTED),
        f"libsonare does not export {symbol}; install a matching native library",
    )


@_guard_buffer("samples")
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
    adaptive_tempo: bool = False,
    tempo_update_interval_beats: int = 8,
    compute_tempo_curve: bool = False,
    meter_candidate_numerators: Sequence[int] | None = None,
    meter_denominator: int = 4,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    Calls ``sonare_analyze_json`` when available to return the complete
    analysis result; falls back to ``sonare_analyze`` for older builds.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz (default 22050).
        adaptive_tempo: Track a locally updated tempo prior through beat
            tracking instead of holding one global tempo.
        tempo_update_interval_beats: Local tempo context length in beats, used
            only when ``adaptive_tempo`` is true. Must be positive.
        compute_tempo_curve: Decode a per-beat local tempo curve into
            ``beat_local_bpm``. Off by default because it is an extra output
            rather than a better analysis — nothing else in the result changes.
            The curve describes the beat grid it was decoded from, and beat
            tracking holds a fixed tempo prior unless ``adaptive_tempo`` is also
            set, so measuring a tempo that moves needs both.
        meter_candidate_numerators: Meter numerators the estimator scores.
            ``None`` selects the native default ``(3, 4, 6)``. At most 16
            entries, each in ``[2, 32]``. Widening the set does not force a
            wider meter — the default reproduces the historical result.
        meter_denominator: Beat unit reported for the detected meter; a power
            of two in ``[1, 32]``. The estimator still reports 8 on its own
            when it resolves a compound meter.

    Returns:
        :class:`libsonare.AnalysisResult` with all fields:

        - ``bpm`` (``float``) and ``bpm_confidence`` (``float`` in ``[0, 1]``)
        - ``key`` (:class:`Key`), ``time_signature`` (:class:`TimeSignature`)
        - ``beat_times`` (``list[float]`` in seconds)
        - ``beat_strengths`` (``list[float]``) — one raw onset-envelope frame
          per beat, sampled at the beat's own frame. Not normalized, scaled by
          the material, and sensitive to beat-position jitter, so it is not a
          salience comparable across beats; score accents with
          ``beat_observations.onset_strength`` instead
        - ``downbeat_indices`` (``list[int]``) — positions in ``beat_times``
          that are bar starts, not a separate time series
        - ``downbeat_phase`` (``int``) — which beat of the first bar the
          analysis starts on
        - ``beat_observations`` (:class:`AnalysisBeatObservations`) — the
          beat-level evidence the downbeat and meter pass scores
        - ``beat_local_bpm`` (``list[float]``) — smoothed local tempo at each
          beat, parallel to ``beat_times``. Empty unless
          ``compute_tempo_curve`` was set
        - ``chords`` (``list[Chord]``) — detected chord segments
        - ``sections`` (``list[Section]``) — detected structural sections
        - ``timbre`` (:class:`AnalysisTimbre`) — spectral character summary
        - ``dynamics`` (:class:`AnalysisDynamics`) — loudness/dynamics summary
        - ``rhythm`` (:class:`AnalysisRhythm`) — groove/syncopation summary
        - ``melody`` (:class:`AnalysisMelody`) — melody contour and statistics
        - ``form`` (``str``) — overall form classification

    Raises:
        SonareValueError: If ``meter_candidate_numerators`` holds more entries
            than the native array can carry.
        RuntimeError: If analysis fails.
    """
    numerators = (
        tuple(_DEFAULT_METER_CANDIDATE_NUMERATORS)
        if meter_candidate_numerators is None
        else tuple(int(value) for value in meter_candidate_numerators)
    )
    # The flat C array cannot carry an over-long list, so reject it here rather
    # than truncating it into a set the caller never asked for. Every other
    # rule (non-empty, per-entry range, denominator) is the core's to enforce.
    if len(numerators) > SONARE_MAX_METER_CANDIDATE_NUMERATORS:
        raise SonareValueError(
            "analyze: meter_candidate_numerators must hold at most "
            f"{SONARE_MAX_METER_CANDIDATE_NUMERATORS} entries"
        )

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
                adaptive_tempo=int(adaptive_tempo),
                tempo_update_interval_beats=tempo_update_interval_beats,
                compute_tempo_curve=int(compute_tempo_curve),
                # Both the array and its count must be written: ctypes zeroes
                # any field left unset, and a zero count reads to the core as
                # an empty candidate set, which it rejects.
                meter_candidate_numerators=(ctypes.c_int * SONARE_MAX_METER_CANDIDATE_NUMERATORS)(
                    *numerators
                ),
                meter_candidate_numerator_count=len(numerators),
                meter_denominator=meter_denominator,
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
    on_progress: Callable[[float, str], None], state: CancellationState
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


@_guard_buffer("samples")
def analyze_with_progress(
    samples: Sequence[float] | list[float],
    sample_rate: int = 22050,
    on_progress: Callable[[float, str], None] | None = None,
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
            invoked during analysis. Progress is in ``[0, 1]``. Its return
            value is ignored. If ``None``, no callback is registered.
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


@_guard_buffer("beat_times")
def estimate_meter(
    beat_times: Sequence[float],
    beat_strengths: Sequence[float],
    *,
    candidate_numerators: Sequence[int] | None = None,
    denominator: int = 4,
    downbeat_weight: float = 1.0,
    measure_weight: float = 0.5,
    subdivision_weight: float = 0.15,
    compound_subdivision_threshold: float = 0.85,
) -> MeterEstimate:
    """Estimate meter over a beat series, without audio and without re-analyzing.

    Scoring reads only the per-beat strengths, so an existing analysis can be
    re-scored — over a different candidate set, or over an arbitrary span of
    its beats — without running the pipeline again.

    Args:
        beat_times: Beat positions in seconds, non-decreasing.
        beat_strengths: Per-beat accent value, the same length as
            ``beat_times``. Pass
            :attr:`AnalysisResult.beat_observations` ``.onset_strength``: it is
            the windowed value the library's own downbeat pass scores.
            ``AnalysisResult.beat_strengths`` also works, but it is a single
            unwindowed envelope frame per beat and scores accordingly. Neither
            needs pre-scaling: the series is divided by its own maximum before
            scoring, so only the accent contrast within it is read and the
            absolute units it arrives in do not matter.
        candidate_numerators: Meter numerators to score. ``None`` selects the
            native default ``(3, 4, 6)``. At most 16 entries, each in
            ``[2, 32]``. Widening the set does not force a wider meter — the
            default reproduces the historical result.
        denominator: Beat unit reported for the detected meter; a power of two
            in ``[1, 32]``. It is reported as requested on this path: whether a
            beat divides into three is measured from energy *between* the
            beats, which per-beat accents do not carry, so a compound meter is
            not resolvable here. A six whose beats accent 3+3 comes back with
            this denominator and ``grouping == [3, 3]`` — read the grouping,
            not the denominator, to tell a compound bar from a simple one.
            (:func:`libsonare.analyze`, which has the audio, does resolve it and
            reports 8 on its own.)
        downbeat_weight: Weight of the downbeat term in the multi-comb score.
        measure_weight: Weight of the measure-level term.
        subdivision_weight: Weight of the subdivision term.
        compound_subdivision_threshold: Subdivision support at which a meter is
            resolved as compound rather than simple.

    Returns:
        :class:`libsonare.MeterEstimate`. ``candidate_scores`` is parallel to
        ``candidate_numerators`` as requested, while ``candidates`` is ordered
        by descending support — the two do not index alike. A score is
        standardized and signed, with zero the level a numerator reaches on
        beats carrying no meter, so only the ordering and the gaps between
        entries carry meaning; scores also grow with the square root of the
        number of beats scored, so scores from spans of different lengths are
        not comparable without normalizing for length. ``grouping`` reports how
        the bar divides into accent groups of two and three beats —
        ``[3, 2, 2]`` for a 7/8 notated 3+2+2 — and always sums to the reported
        numerator. ``searched`` is False when the series was too short to score
        any candidate, in which case every other field is the fixed fallback
        rather than a measurement.

    Raises:
        SonareValueError: If ``beat_times`` and ``beat_strengths`` differ in
            length, or ``candidate_numerators`` holds more entries than the
            native array can carry.
        SonareError: If the core rejects an option value or a beat series it
            cannot answer.
    """
    numerators = (
        tuple(_DEFAULT_METER_CANDIDATE_NUMERATORS)
        if candidate_numerators is None
        else tuple(int(value) for value in candidate_numerators)
    )
    # The flat C array cannot carry an over-long list, so reject it here rather
    # than truncating it into a set the caller never asked for. Every other
    # rule (non-empty, per-entry range, denominator, weights, beat series) is
    # the core's to enforce.
    if len(numerators) > SONARE_MAX_METER_CANDIDATE_NUMERATORS:
        raise SonareValueError(
            "estimate_meter: candidate_numerators must hold at most "
            f"{SONARE_MAX_METER_CANDIDATE_NUMERATORS} entries"
        )

    lib = _get_lib()
    if not hasattr(lib, "sonare_estimate_meter_json"):
        raise _unsupported_feature_symbol("sonare_estimate_meter_json")

    c_times, time_count = _to_c_float_array(beat_times)
    c_strengths, strength_count = _to_c_float_array(beat_strengths)
    # The C entry point carries one beat count for both arrays, so a mismatch
    # never reaches the core's own check — reject it with the same meaning.
    if time_count != strength_count:
        raise SonareValueError(
            "estimate_meter: beat_times and beat_strengths must be the same length "
            f"(got {time_count} and {strength_count})"
        )

    options = SonareMeterOptions(
        # Both the array and its count must be written: ctypes zeroes any field
        # left unset, and a zero count reads to the core as an empty candidate
        # set, which it rejects.
        candidate_numerators=(ctypes.c_int * SONARE_MAX_METER_CANDIDATE_NUMERATORS)(*numerators),
        candidate_numerator_count=len(numerators),
        denominator=denominator,
        downbeat_weight=downbeat_weight,
        measure_weight=measure_weight,
        subdivision_weight=subdivision_weight,
        compound_subdivision_threshold=compound_subdivision_threshold,
    )
    out_json = ctypes.c_char_p()
    rc = lib.sonare_estimate_meter_json(
        c_times,
        c_strengths,
        ctypes.c_size_t(time_count),
        ctypes.byref(options),
        ctypes.byref(out_json),
    )
    _check(rc)
    try:
        raw = out_json.value
        data = json.loads(raw.decode("utf-8") if raw else "{}")
    finally:
        if out_json.value and hasattr(lib, "sonare_free_string"):
            lib.sonare_free_string(out_json)

    ts_d = data.get("timeSignature", {})
    return MeterEstimate(
        time_signature=TimeSignature(
            numerator=int(ts_d.get("numerator", 4)),
            denominator=int(ts_d.get("denominator", 4)),
            confidence=float(ts_d.get("confidence", 0.0)),
        ),
        downbeat_phase=int(data.get("downbeatPhase", 0)),
        # Absent only if the payload predates the field; the conservative
        # reading of a missing flag is that nothing was searched.
        searched=bool(data.get("searched", False)),
        grouping=[int(part) for part in data.get("grouping", [])],
        candidate_scores=[float(score) for score in data.get("candidateScores", [])],
        candidates=[
            TimeSignature(
                numerator=int(candidate.get("numerator", 4)),
                denominator=int(candidate.get("denominator", 4)),
                confidence=float(candidate.get("confidence", 0.0)),
            )
            for candidate in data.get("candidates", [])
        ],
    )


@_guard_buffer("samples")
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


@_guard_buffer("samples")
def analyze_impulse_response(
    samples: Sequence[float] | list[float],
    sample_rate: int = 48000,
    n_octave_bands: int = 6,
    min_decay_db: float = 30.0,
) -> AcousticResult:
    """Analyze RT60, EDT, and clarity metrics from an impulse response.

    ``min_decay_db`` selects the minimum decay range used by the native
    regression and defaults to the legacy 30 dB range.
    """
    if isinstance(n_octave_bands, bool):
        raise SonareValueError(
            "analyze_impulse_response: n_octave_bands must be a non-negative integer"
        )
    try:
        n_octave_bands_value = operator.index(n_octave_bands)
    except TypeError as exc:
        raise SonareValueError(
            "analyze_impulse_response: n_octave_bands must be a non-negative integer"
        ) from exc
    if n_octave_bands_value < 0 or n_octave_bands_value > 2**31 - 1:
        raise SonareValueError(
            "analyze_impulse_response: n_octave_bands must be a non-negative integer"
        )
    try:
        decay_db = float(min_decay_db)
        decay_db_c = ctypes.c_float(decay_db).value
    except (TypeError, ValueError, OverflowError) as exc:
        raise SonareValueError(
            "analyze_impulse_response: min_decay_db must be finite and positive"
        ) from exc
    if not math.isfinite(decay_db_c) or decay_db_c <= 0.0:
        raise SonareValueError("analyze_impulse_response: min_decay_db must be finite and positive")

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAcousticResult()
    if hasattr(lib, "sonare_analyze_impulse_response_ex"):
        rc = lib.sonare_analyze_impulse_response_ex(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_octave_bands_value),
            ctypes.c_float(decay_db_c),
            ctypes.byref(out),
        )
    elif decay_db_c != 30.0:
        raise _unsupported_feature_symbol("sonare_analyze_impulse_response_ex")
    else:
        rc = lib.sonare_analyze_impulse_response(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_int(sample_rate),
            ctypes.c_int(n_octave_bands_value),
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


@_guard_buffer("samples")
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


@_guard_buffer("samples")
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


@_guard_buffer("samples")
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


@_guard_buffer("samples")
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
