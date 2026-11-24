"""Analysis and inspection wrappers for libsonare."""

from __future__ import annotations

import ctypes
import json
import math
from collections.abc import Sequence
from typing import Any

import numpy as np

from ._analysis_detection import _DEFAULT_BPM_MAX, _DEFAULT_BPM_MIN, _parse_analysis_json
from ._cancellation import (
    CancelCallback,
    CancellationState,
    ProgressCallback,
    make_cancel_trampoline,
    make_progress_trampoline,
    raise_callback_error,
)
from ._errors import _entry, _refuse, _refuse_type
from ._ffi import (
    SONARE_MAX_METER_CANDIDATE_NUMERATORS,
    SonareAcousticResult,
    SonareAnalyzeImpulseResponseOptions,
    SonareAnalyzeProgressCallback,
    SonareBpmAnalysisResult,
    SonareDynamicsResult,
    SonareMeterBeats,
    SonareMeterOptions,
    SonareMusicAnalyzeOptions,
    SonareRhythmResult,
    SonareTimbreResult,
)
from ._narrowing import _narrow_double, _Respelling
from ._runtime import (
    _C_INT_MAX,
    _C_INT_MIN,
    _check,
    _from_c_float_array,
    _get_lib,
    _guard_buffer,
    _measured,
    _measured_level,
    _narrow_int,
    _optional_from_c_float_array,
    _to_c_float,
    _to_c_float_array,
    _to_c_int,
    _to_c_size_t,
    _tuning_arg,
    _validate_samples,
)
from .types import (
    AcousticResult,
    AnalysisResult,
    BeatObservations,
    BpmAnalysisResult,
    BpmCandidate,
    DynamicsResult,
    MeterEstimate,
    RhythmResult,
    TimbreFrame,
    TimbreResult,
    TimeSignature,
)

# Meter numerators the estimator scores when the caller passes none. Mirrors
# MusicAnalyzerConfig::meter_candidate_numerators, so a default-argument
# analyze() reproduces the native default rather than a Python-side guess.
_DEFAULT_METER_CANDIDATE_NUMERATORS = (3, 4, 6)


def _music_analyze_options(
    *,
    n_fft: int,
    hop_length: int,
    bpm_min: float | None,
    bpm_max: float | None,
    start_bpm: float,
    use_triads_only: bool,
    use_hpss: bool,
    chroma_high_pass_hz: float,
    use_bass_weighted: bool,
    chroma_hop_multiplier: int,
    use_chord_hmm: bool,
    use_chord_key_context: bool,
    chord_hmm_beam_width: int,
    detect_chord_inversions: bool,
    adaptive_tempo: bool,
    tempo_update_interval_beats: int,
    compute_tempo_curve: bool,
    meter_candidate_numerators: Sequence[int] | None,
    meter_denominator: int,
    tuning_cents: float | str,
) -> SonareMusicAnalyzeOptions:
    """Build the options struct shared by :func:`analyze`."""
    numerators = (
        tuple(_DEFAULT_METER_CANDIDATE_NUMERATORS)
        if meter_candidate_numerators is None
        else tuple(meter_candidate_numerators)
    )
    # The flat C array cannot carry an over-long list, so reject it here rather
    # than truncating it into a set the caller never asked for. Entries are
    # narrowed on the C int range below so the core sees what the caller passed;
    # every other rule (non-empty, numerator domain, denominator) is the core's.
    if len(numerators) > SONARE_MAX_METER_CANDIDATE_NUMERATORS:
        raise _refuse(
            "meter_candidate_numerators must hold at most "
            f"{SONARE_MAX_METER_CANDIDATE_NUMERATORS} entries"
        )
    tuning_value, tuning_auto = _tuning_arg(tuning_cents)
    return SonareMusicAnalyzeOptions(
        n_fft=n_fft,
        hop_length=hop_length,
        bpm_min=_DEFAULT_BPM_MIN if bpm_min is None else bpm_min,
        bpm_max=_DEFAULT_BPM_MAX if bpm_max is None else bpm_max,
        start_bpm=start_bpm,
        use_triads_only=int(use_triads_only),
        use_hpss=int(use_hpss),
        chroma_high_pass_hz=chroma_high_pass_hz,
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
            *[
                _narrow_int(
                    value,
                    f"meter_candidate_numerators[{i}]",
                    _C_INT_MIN,
                    _C_INT_MAX,
                )
                for i, value in enumerate(numerators)
            ]
        ),
        meter_candidate_numerator_count=len(numerators),
        meter_denominator=meter_denominator,
        tuning_cents=tuning_value,
        tuning_auto=tuning_auto,
    )


@_entry("analyze")
@_guard_buffer("samples")
def analyze(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    *,
    n_fft: int = 2048,
    hop_length: int = 512,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    start_bpm: float = 120.0,
    use_triads_only: bool = True,
    use_hpss: bool = False,
    chroma_high_pass_hz: float = 80.0,
    use_bass_weighted: bool = True,
    chroma_hop_multiplier: int = 4,
    use_chord_hmm: bool = True,
    use_chord_key_context: bool = False,
    chord_hmm_beam_width: int = 24,
    detect_chord_inversions: bool = False,
    adaptive_tempo: bool = False,
    tempo_update_interval_beats: int = 8,
    compute_tempo_curve: bool = False,
    meter_candidate_numerators: Sequence[int] | None = None,
    meter_denominator: int = 4,
    tuning_cents: float | str = "auto",
    on_progress: ProgressCallback | None = None,
    cancel: CancelCallback | None = None,
) -> AnalysisResult:
    """Run full audio analysis on samples.

    ``n_fft`` is in samples at 22050 Hz, rescaled to the analysis rate; ``hop_length`` is the
    hop in samples of the analysed signal: the input resampled to 22050 Hz when its rate is
    above 22050 Hz, the input itself otherwise.

    Args:
        samples: Mono audio samples (1D float). See :func:`detect_bpm` for
            accepted types.
        sample_rate: Sample rate in Hz.
        start_bpm: A finite, positive BPM prior that seeds beat tracking if the
            tempo search finds none. The result's ``bpm`` is ``None`` when no
            tempo is measured.
        adaptive_tempo: Track a locally updated tempo prior through beat
            tracking instead of holding one global tempo.
        tempo_update_interval_beats: Local tempo context length in beats, used
            only when ``adaptive_tempo`` is true. Must be greater than 0.
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
        tuning_cents: Tuning offset of the recording in cents; must
            be in ``[-50, 50)``. ``"auto"`` (default) measures it from the audio, and
            :func:`reference_hz_to_tuning` converts a reference pitch to this unit.
            The value used, given or measured, is ``AnalysisResult.tuning_cents``.
            Every chroma the analysis builds (key, chords, sections) is shifted
            by it. 0 is concert A440.
        on_progress: Optional callable ``(progress: float, stage: str)``
            invoked periodically with a progress fraction in ``[0, 1]`` and a
            stage name. Its return value is ignored. An exception it raises
            stops the analysis at its next cancellation point and is raised
            from this call.
        cancel: Optional zero-argument callable polled by the native
            operation. A true return value requests cancellation, which raises
            :class:`SonareError` with code ``ErrorCode.CANCELLED``; an
            exception it raises is raised from this call.

    Returns:
        :class:`libsonare.AnalysisResult` with all fields:

        - ``bpm`` (``float``, ``None`` when no tempo was found) and
          ``bpm_confidence`` (``float`` in ``[0, 1]``)
        - ``key`` (:class:`Key`), ``time_signature`` (:class:`TimeSignature`)
        - ``beats`` (``list[Beat]``) — each with ``time_sec`` and a raw
          onset-envelope ``strength`` sampled at the beat's own frame. Not
          normalized, scaled by the material, and sensitive to beat-position
          jitter, so it is not a salience comparable across beats; score
          accents with ``beat_observations.accent`` instead
        - ``downbeats`` (``list[Beat]``) — the beats that are bar starts
        - ``downbeat_phase`` (``int``) — which beat of the first bar the
          analysis starts on
        - ``beat_observations`` (:class:`BeatObservations`) — the
          beat-level evidence the downbeat and meter pass scores
        - ``beat_local_bpm`` (float32 array) — smoothed local tempo at each
          beat, parallel to ``beats``: continuous values, each a weighted
          average of the surrounding beat intervals, so a steady tempo reads
          within about 1% at every beat. Empty unless
          ``compute_tempo_curve`` was set
        - ``chords`` (``list[AnalysisChord]``) — detected chord segments, each
          with a ``roman_numeral`` relative to ``key`` (empty for N.C.)
        - ``sections`` (``list[Section]``) — detected structural sections
        - ``timbre`` (:class:`TimbreFrame`) — spectral character summary
        - ``dynamics`` (:class:`AnalysisDynamics`) — loudness/dynamics summary
        - ``rhythm`` (:class:`AnalysisRhythm`) — groove/syncopation summary
        - ``melody`` (:class:`MelodyResult`) — melody contour and statistics
        - ``form`` (``str``) — overall form classification

    Raises:
        SonareValueError: If an argument is outside its supported domain, including
            a ``meter_candidate_numerators`` list that exceeds the native limit.
        SonareError: If analysis is cancelled or the library reports another
            failure, with its error code.
    """
    # Built before the library is touched, so an over-long numerator list is
    # refused whichever entry point runs below.
    options = _music_analyze_options(
        n_fft=n_fft,
        hop_length=hop_length,
        bpm_min=bpm_min,
        bpm_max=bpm_max,
        start_bpm=start_bpm,
        use_triads_only=use_triads_only,
        use_hpss=use_hpss,
        chroma_high_pass_hz=chroma_high_pass_hz,
        use_bass_weighted=use_bass_weighted,
        chroma_hop_multiplier=chroma_hop_multiplier,
        use_chord_hmm=use_chord_hmm,
        use_chord_key_context=use_chord_key_context,
        chord_hmm_beam_width=chord_hmm_beam_width,
        detect_chord_inversions=detect_chord_inversions,
        adaptive_tempo=adaptive_tempo,
        tempo_update_interval_beats=tempo_update_interval_beats,
        compute_tempo_curve=compute_tempo_curve,
        meter_candidate_numerators=meter_candidate_numerators,
        meter_denominator=meter_denominator,
        tuning_cents=tuning_cents,
    )
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)

    if on_progress is not None or cancel is not None:
        return _analyze_with_callbacks(
            lib, c_array, length, sample_rate, options, on_progress, cancel
        )

    out_json = ctypes.c_char_p()
    _check(
        lib.sonare_analyze_json(
            c_array,
            _to_c_size_t(length, "length"),
            _to_c_int(sample_rate, "sample_rate"),
            ctypes.byref(options),
            ctypes.byref(out_json),
        ),
        flattened="options",
    )
    try:
        raw = out_json.value
        data = json.loads(raw.decode("utf-8") if raw else "{}")
    finally:
        _free_json(lib, out_json)
    return _parse_analysis_json(data)


def _free_json(lib: ctypes.CDLL, out_json: ctypes.c_char_p) -> None:
    if out_json.value:
        lib.sonare_free_string(out_json)


def _make_analyze_progress_trampoline(
    on_progress: ProgressCallback, state: CancellationState
) -> Any:
    """Wrap a Python callback for use as a C SonareAnalyzeProgressCallback."""
    return make_progress_trampoline(SonareAnalyzeProgressCallback, on_progress, state)


def _analyze_with_callbacks(
    lib: ctypes.CDLL,
    c_array: Any,
    length: int,
    sample_rate: int,
    options: SonareMusicAnalyzeOptions,
    on_progress: ProgressCallback | None,
    cancel: CancelCallback | None,
) -> AnalysisResult:
    """Run the analysis with the callbacks set on its options and parse the result."""

    out_json = ctypes.c_char_p()
    state = CancellationState(cancel)
    c_cb = (
        _make_analyze_progress_trampoline(on_progress, state)
        if on_progress is not None
        else SonareAnalyzeProgressCallback(0)
    )
    cancel_cb = make_cancel_trampoline(state)
    options.progress_callback = c_cb
    options.progress_user_data = None
    options.cancel_callback = cancel_cb
    options.cancel_user_data = None
    rc = lib.sonare_analyze_json(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        ctypes.byref(options),
        ctypes.byref(out_json),
    )
    raise_callback_error(state, rc, lambda: _free_json(lib, out_json))
    _check(rc, flattened="options")
    try:
        raw = out_json.value
        data = json.loads(raw.decode("utf-8") if raw else "{}")
    finally:
        _free_json(lib, out_json)
    return _parse_analysis_json(data)


# The C struct is named ``beats``; the Python caller supplies an observations record.
_METER_BEATS_RESPELLED = {
    "beats.accent": _Respelling("observations.accent"),
    "beats.has_attack": _Respelling("observations.has_attack"),
    "beats.low_frequency_energy": _Respelling("observations.low_frequency_energy"),
    "beats.subdivision_accent": _Respelling("observations.subdivision_accent"),
}


@_entry("estimate_meter")
def estimate_meter(
    observations: BeatObservations,
    *,
    candidate_numerators: Sequence[int] | None = None,
    denominator: int = 4,
    downbeat_weight: float = 1.0,
    measure_weight: float = 0.5,
    subdivision_weight: float = 0.15,
    compound_subdivision_threshold: float = 0.5,
) -> MeterEstimate:
    """Estimate meter from beat-level observations, without re-analyzing audio.

    The required ``accent`` stream carries the measured attack strength for
    each beat. Optional attack, low-frequency-energy, and subdivision streams
    add evidence the analyzer uses to distinguish rests, masked beats, and
    compound meter. An existing analysis can be re-scored over a different
    candidate set or an arbitrary span without running the pipeline again.

    Args:
        observations: A :class:`BeatObservations` record. ``accent`` must hold
            one finite, nonnegative value per beat. ``has_attack`` and
            ``low_frequency_energy`` may be empty or beat-indexed;
            ``subdivision_accent`` may be empty or have one fewer value than
            ``accent``. Empty ``has_attack`` treats every beat as attacked;
            empty subdivision evidence means compound meter cannot be measured.
        candidate_numerators: Meter numerators to score. ``None`` selects the
            native default ``(3, 4, 6)``. At most 16 entries, each in
            ``[2, 32]``. Widening the set does not force a wider meter — the
            default reproduces the historical result.
        denominator: Beat unit reported for the detected meter; a power of two
            ``[1, 32]``. The detected meter reports 8 when measured subdivision
            evidence identifies compound time.
        downbeat_weight: Weight of the downbeat term in the multi-comb score.
        measure_weight: Weight of the measure-level term.
        subdivision_weight: Weight of the subdivision term.
        compound_subdivision_threshold: Mean measured subdivision accent as a
            fraction of mean beat accent, at and above which the meter is read
            as compound. Empty subdivision evidence does not affect this
            decision; measured zero values are evidence of no subdivision.

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
        rather than a measurement, the confidence 0 included. Two quantities
        share the name ``confidence``: ``time_signature.confidence`` is the
        margin over the runner-up, and each ``candidates`` entry reports its
        share of summed support.

    Raises:
        TypeError: If ``observations`` is not a :class:`BeatObservations`.
        SonareValueError: If the accent stream is empty, a stream has an invalid
            length or value, ``observations`` is None, or ``candidate_numerators``
            is too long.
        SonareError: If the core rejects an option value.
    """
    numerators = (
        tuple(_DEFAULT_METER_CANDIDATE_NUMERATORS)
        if candidate_numerators is None
        else tuple(candidate_numerators)
    )
    # The flat C array cannot carry an over-long list, so reject it here rather
    # than truncating it into a set the caller never asked for. Entries are
    # narrowed on the C int range below so the core sees what the caller passed;
    # option domains remain the core's to enforce.
    if len(numerators) > SONARE_MAX_METER_CANDIDATE_NUMERATORS:
        raise _refuse(
            "candidate_numerators must hold at most "
            f"{SONARE_MAX_METER_CANDIDATE_NUMERATORS} entries"
        )

    if observations is None:
        raise _refuse("observations must be a BeatObservations")
    if not isinstance(observations, BeatObservations):
        raise _refuse_type("observations must be a BeatObservations")

    accent = _validate_samples(observations.accent, arg_name="observations.accent")
    has_attack = np.asarray(observations.has_attack)
    if has_attack.ndim != 1:
        raise _refuse("observations.has_attack must be a one-dimensional array")
    if has_attack.size:
        if has_attack.dtype.kind not in "biu":
            raise _refuse_type("observations.has_attack must contain boolean or integer values")
        if np.any((has_attack != 0) & (has_attack != 1)):
            raise _refuse("observations.has_attack must hold only 0 or 1")
        if has_attack.size != accent.size:
            raise _refuse(
                "observations.has_attack must be empty or have the same length as "
                "observations.accent"
            )
        has_attack = np.array(has_attack, dtype=np.uint8, order="C", copy=True)
    else:
        has_attack = np.empty(0, dtype=np.uint8)

    low_frequency_energy = _validate_samples(
        observations.low_frequency_energy,
        arg_name="observations.low_frequency_energy",
        allow_empty=True,
    )
    if low_frequency_energy.size not in (0, accent.size):
        raise _refuse(
            "observations.low_frequency_energy must be empty or have the same length as "
            "observations.accent"
        )
    subdivision_accent = _validate_samples(
        observations.subdivision_accent,
        arg_name="observations.subdivision_accent",
        allow_empty=True,
    )
    if subdivision_accent.size not in (0, accent.size - 1):
        raise _refuse(
            "observations.subdivision_accent must be empty or have one fewer entry than "
            "observations.accent"
        )
    lib = _get_lib()
    c_accent, beat_count = _to_c_float_array(accent, arg_name="observations.accent")
    c_attack = (
        (ctypes.c_uint8 * has_attack.size).from_buffer(has_attack)
        if has_attack.size
        else (ctypes.c_uint8 * 0)()
    )
    c_energy, energy_count = _to_c_float_array(
        low_frequency_energy, arg_name="observations.low_frequency_energy"
    )
    c_subdivision, subdivision_count = _to_c_float_array(
        subdivision_accent, arg_name="observations.subdivision_accent"
    )
    meter_beats = SonareMeterBeats(
        accent=ctypes.cast(c_accent, ctypes.POINTER(ctypes.c_float)),
        has_attack=(
            ctypes.cast(c_attack, ctypes.POINTER(ctypes.c_uint8))
            if has_attack.size
            else ctypes.POINTER(ctypes.c_uint8)()
        ),
        low_frequency_energy=(
            ctypes.cast(c_energy, ctypes.POINTER(ctypes.c_float))
            if energy_count
            else ctypes.POINTER(ctypes.c_float)()
        ),
        subdivision_accent=(
            ctypes.cast(c_subdivision, ctypes.POINTER(ctypes.c_float))
            if subdivision_count
            else ctypes.POINTER(ctypes.c_float)()
        ),
        beat_count=beat_count,
    )

    options = SonareMeterOptions(
        # Both the array and its count must be written: ctypes zeroes any field
        # left unset, and a zero count reads to the core as an empty candidate
        # set, which it rejects.
        candidate_numerators=(ctypes.c_int * SONARE_MAX_METER_CANDIDATE_NUMERATORS)(
            *[
                _narrow_int(value, f"candidate_numerators[{i}]", _C_INT_MIN, _C_INT_MAX)
                for i, value in enumerate(numerators)
            ]
        ),
        candidate_numerator_count=len(numerators),
        denominator=denominator,
        downbeat_weight=downbeat_weight,
        measure_weight=measure_weight,
        subdivision_weight=subdivision_weight,
        compound_subdivision_threshold=compound_subdivision_threshold,
    )
    out_json = ctypes.c_char_p()
    rc = lib.sonare_estimate_meter_json(
        ctypes.byref(meter_beats),
        ctypes.byref(options),
        ctypes.byref(out_json),
    )
    _check(rc, flattened="options", respelled=_METER_BEATS_RESPELLED)
    try:
        raw = out_json.value
        data = json.loads(raw.decode("utf-8") if raw else "{}")
    finally:
        if out_json.value:
            lib.sonare_free_string(out_json)

    ts_d = data["timeSignature"]
    return MeterEstimate(
        time_signature=TimeSignature(
            numerator=int(ts_d["numerator"]),
            denominator=int(ts_d["denominator"]),
            confidence=float(ts_d["confidence"]),
        ),
        downbeat_phase=int(data["downbeatPhase"]),
        searched=bool(data["searched"]),
        grouping=np.asarray(data["grouping"], dtype=np.int32),
        candidate_scores=np.asarray(data["candidateScores"], dtype=np.float32),
        candidates=[
            TimeSignature(
                numerator=int(candidate["numerator"]),
                denominator=int(candidate["denominator"]),
                confidence=float(candidate["confidence"]),
            )
            for candidate in data["candidates"]
        ],
        unobserved_beats=np.asarray(data["unobservedBeats"], dtype=np.int32),
    )


@_entry("analyze_bpm")
@_guard_buffer("samples")
def analyze_bpm(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    n_fft: int = 2048,
    hop_length: int = 512,
    max_candidates: int = 5,
) -> BpmAnalysisResult:
    """Analyze BPM with confidence, candidates, autocorrelation, and tempogram.

    ``n_fft`` is the analysis window length in samples at 22050 Hz, rescaled to
    the input sample rate; ``hop_length`` is in samples of the input buffer.
    The tempo search spans ``bpm_min``-``bpm_max``, 60-200 BPM by default like
    every other tempo entry point; widen it to list half-tempo candidates or
    very slow material. A range other than the default is searched as given,
    and the unconstrained estimate is preferred when it lies inside it.
    ``bpm`` is ``None`` when no tempo is found and ``candidates`` is empty.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareBpmAnalysisResult()
    rc = lib.sonare_analyze_bpm(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
        _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(max_candidates, "max_candidates"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return BpmAnalysisResult(
            bpm=_measured(out.bpm),
            confidence=float(out.confidence),
            candidates=[
                BpmCandidate(
                    bpm=float(out.candidates[i].bpm),
                    confidence=float(out.candidates[i].confidence),
                )
                for i in range(out.candidate_count)
            ],
            autocorrelation=_from_c_float_array(out.autocorrelation, out.autocorrelation_count),
            tempogram=_from_c_float_array(out.tempogram, out.tempogram_count),
        )
    finally:
        lib.sonare_free_bpm_analysis_result(ctypes.byref(out))


@_entry("analyze_impulse_response")
@_guard_buffer("samples")
def analyze_impulse_response(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_octave_bands: int = 6,
    min_decay_db: float = 30.0,
) -> AcousticResult:
    """Analyze RT60, EDT, and clarity metrics from an impulse response.

    ``min_decay_db`` selects the minimum decay range used by the native
    regression and defaults to the legacy 30 dB range.
    """
    n_octave_bands_value = _narrow_int(n_octave_bands, "n_octave_bands", 0, _C_INT_MAX)
    try:
        decay_db = _narrow_double(min_decay_db, "min_decay_db")
        decay_db_c = ctypes.c_float(decay_db).value
    except (TypeError, ValueError, OverflowError) as exc:
        raise _refuse("min_decay_db must be finite and greater than 0") from exc
    if not math.isfinite(decay_db_c) or decay_db_c <= 0.0:
        raise _refuse("min_decay_db must be finite and greater than 0")

    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAcousticResult()
    options = SonareAnalyzeImpulseResponseOptions._given(min_decay_db=decay_db_c)
    rc = lib.sonare_analyze_impulse_response(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_octave_bands_value, "n_octave_bands_value"),
        ctypes.byref(options),
        ctypes.byref(out),
    )
    _check(rc, flattened="options")
    try:
        count = out.band_count
        blind = bool(out.is_blind)
        return AcousticResult(
            rt60=_measured(out.rt60),
            edt=None if blind else _measured(out.edt),
            c50=None if blind else _measured(out.c50),
            c80=None if blind else _measured(out.c80),
            d50=None if blind else _measured(out.d50),
            rt60_bands=_from_c_float_array(out.rt60_bands, count),
            edt_bands=None if blind else _from_c_float_array(out.edt_bands, count),
            c50_bands=None if blind else _optional_from_c_float_array(out.c50_bands, count),
            c80_bands=None if blind else _optional_from_c_float_array(out.c80_bands, count),
            confidence=float(out.confidence),
            is_blind=bool(out.is_blind),
        )
    finally:
        lib.sonare_free_acoustic_result(ctypes.byref(out))


@_entry("detect_acoustic")
@_guard_buffer("samples")
def detect_acoustic(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_octave_bands: int = 6,
    n_third_octave_subbands: int = 24,
    min_decay_db: float = 30.0,
    noise_floor_margin_db: float = 10.0,
) -> AcousticResult:
    """Estimate blind RT60 from ordinary audio (EDT and clarity are None)."""
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareAcousticResult()
    rc = lib.sonare_detect_acoustic(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_octave_bands, "n_octave_bands"),
        _to_c_int(n_third_octave_subbands, "n_third_octave_subbands"),
        _to_c_float(min_decay_db, "min_decay_db"),
        _to_c_float(noise_floor_margin_db, "noise_floor_margin_db"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        count = out.band_count
        blind = bool(out.is_blind)
        return AcousticResult(
            rt60=_measured(out.rt60),
            edt=None if blind else _measured(out.edt),
            c50=None if blind else _measured(out.c50),
            c80=None if blind else _measured(out.c80),
            d50=None if blind else _measured(out.d50),
            rt60_bands=_from_c_float_array(out.rt60_bands, count),
            edt_bands=None if blind else _from_c_float_array(out.edt_bands, count),
            c50_bands=None if blind else _optional_from_c_float_array(out.c50_bands, count),
            c80_bands=None if blind else _optional_from_c_float_array(out.c80_bands, count),
            confidence=float(out.confidence),
            is_blind=bool(out.is_blind),
        )
    finally:
        lib.sonare_free_acoustic_result(ctypes.byref(out))


@_entry("analyze_rhythm")
@_guard_buffer("samples")
def analyze_rhythm(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    bpm_min: float | None = None,
    bpm_max: float | None = None,
    start_bpm: float = 120.0,
    n_fft: int = 2048,
    hop_length: int = 512,
) -> RhythmResult:
    """Analyze rhythm primitives without generating a summary report.

    The returned ``bpm`` is the beat tracker's own tempo, refined from the local
    beat period. It is a third figure rather than either tempo entry point's:
    measured against synthesized click trains it differs from both at every
    sample rate, and lands closer to the known tempo than either. Analysis runs
    at ``sample_rate``. ``n_fft`` is the analysis window length in samples at
    22050 Hz, rescaled to the input sample rate; ``hop_length`` is in samples
    of the input buffer. The tempo search spans 60-200 BPM by default, as on
    every other tempo entry point. ``bpm`` is ``None`` when neither the search nor
    the tracked beats give a tempo, and the three rhythm measures are ``None``
    below the beats each needs (see :class:`RhythmResult`). ``start_bpm`` is a
    finite, positive BPM prior that seeds beat tracking if the tempo search
    finds none. The result's ``bpm`` is ``None`` when no tempo is measured.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareRhythmResult()
    rc = lib.sonare_analyze_rhythm(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(_DEFAULT_BPM_MIN if bpm_min is None else bpm_min, "bpm_min"),
        _to_c_float(_DEFAULT_BPM_MAX if bpm_max is None else bpm_max, "bpm_max"),
        _to_c_float(start_bpm, "start_bpm"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        ctypes.byref(out),
    )
    _check(rc)
    groove_names = {0: "straight", 1: "shuffle", 2: "swing"}
    try:
        return RhythmResult(
            bpm=_measured(out.bpm),
            time_signature=TimeSignature(
                numerator=int(out.time_signature.numerator),
                denominator=int(out.time_signature.denominator),
                confidence=float(out.time_signature.confidence),
            ),
            groove_type=groove_names.get(int(out.groove_type), "straight"),
            syncopation=_measured(out.syncopation),
            pattern_regularity=_measured(out.pattern_regularity),
            tempo_stability=_measured(out.tempo_stability),
            beat_intervals=_from_c_float_array(out.beat_intervals, out.beat_interval_count),
        )
    finally:
        lib.sonare_free_rhythm_result(ctypes.byref(out))


@_entry("analyze_dynamics")
@_guard_buffer("samples")
def analyze_dynamics(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    window_sec: float = 0.4,
    hop_length: int = 512,
    compression_threshold_db: float = 6.0,
) -> DynamicsResult:
    """Analyze dynamics and loudness primitives.

    ``hop_length`` is the hop in samples of the caller's buffer, at the sample rate.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareDynamicsResult()
    rc = lib.sonare_analyze_dynamics(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_float(window_sec, "window_sec"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_float(compression_threshold_db, "compression_threshold_db"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return DynamicsResult(
            dynamic_range_db=_measured_level(out.dynamic_range_db),
            peak_db=float(out.peak_db),
            rms_db=float(out.rms_db),
            crest_factor=_measured_level(out.crest_factor),
            loudness_range_lu=_measured_level(out.loudness_range_lu),
            is_compressed=bool(out.is_compressed),
            loudness_times_sec=_from_c_float_array(out.loudness_times_sec, out.loudness_count),
            loudness_rms_db=_from_c_float_array(out.loudness_rms_db, out.loudness_count),
        )
    finally:
        lib.sonare_free_dynamics_result(ctypes.byref(out))


@_entry("analyze_timbre")
@_guard_buffer("samples")
def analyze_timbre(
    samples: Sequence[float] | list[float],
    sample_rate: int,
    n_fft: int = 2048,
    hop_length: int = 512,
    n_mels: int = 128,
    n_mfcc: int = 13,
    window_sec: float = 0.5,
) -> TimbreResult:
    """Analyze timbre and spectral-shape primitives.

    ``n_fft`` is the analysis window length in samples at 22050 Hz, rescaled to
    the input sample rate; ``hop_length`` is in samples of the input buffer.
    """
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    out = SonareTimbreResult()
    rc = lib.sonare_analyze_timbre(
        c_array,
        _to_c_size_t(length, "length"),
        _to_c_int(sample_rate, "sample_rate"),
        _to_c_int(n_fft, "n_fft"),
        _to_c_size_t(hop_length, "hop_length"),
        _to_c_int(n_mels, "n_mels"),
        _to_c_int(n_mfcc, "n_mfcc"),
        _to_c_float(window_sec, "window_sec"),
        ctypes.byref(out),
    )
    _check(rc)
    try:
        return TimbreResult(
            brightness=_measured(out.brightness),
            warmth=_measured(out.warmth),
            density=_measured(out.density),
            roughness=_measured(out.roughness),
            complexity=_measured(out.complexity),
            spectral_centroid=_from_c_float_array(
                out.spectral_centroid, out.spectral_centroid_count
            ),
            spectral_flatness=_from_c_float_array(
                out.spectral_flatness, out.spectral_flatness_count
            ),
            spectral_rolloff=_from_c_float_array(out.spectral_rolloff, out.spectral_rolloff_count),
            timbre_over_time=[
                TimbreFrame(
                    brightness=_measured(out.timbre_over_time[i].brightness),
                    warmth=_measured(out.timbre_over_time[i].warmth),
                    density=_measured(out.timbre_over_time[i].density),
                    roughness=_measured(out.timbre_over_time[i].roughness),
                    complexity=_measured(out.timbre_over_time[i].complexity),
                )
                for i in range(out.timbre_over_time_count)
            ],
        )
    finally:
        lib.sonare_free_timbre_result(ctypes.byref(out))
