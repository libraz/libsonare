"""Tests for ``estimate_meter`` and ``AnalysisResult.beat_observations``.

``estimate_meter`` scores a caller-supplied beat series, so most of these run
without audio at all. The ``beat_observations`` cases need a real ``analyze()``
pass and share one module-scoped result.
"""

from __future__ import annotations

import math

import pytest

import libsonare
from libsonare import ErrorCode, SonareError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


def _beat_series(
    beats_per_bar: int,
    bars: int = 8,
    bpm: float = 120.0,
    accent: float = 1.0,
    weak: float = 0.3,
) -> tuple[list[float], list[float]]:
    """A click pattern as parallel beat-time / beat-strength arrays.

    Every bar start carries ``accent`` and every other beat ``weak``, which is
    the periodicity the multi-comb score keys on.
    """
    interval = 60.0 / bpm
    count = beats_per_bar * bars
    times = [index * interval for index in range(count)]
    strengths = [accent if index % beats_per_bar == 0 else weak for index in range(count)]
    return times, strengths


def test_default_candidate_set_reports_four_four() -> None:
    """A four-beat accent pattern resolves as 4/4 with the native defaults."""
    times, strengths = _beat_series(4)

    result = libsonare.estimate_meter(times, strengths)

    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert result.time_signature.confidence > 0.0
    # candidateScores is parallel to the requested numerators (3, 4, 6).
    assert len(result.candidate_scores) == 3
    assert result.candidates
    assert str(result.candidates[0]) == "4/4"


@pytest.mark.parametrize("beats_per_bar", [5, 7])
def test_widened_candidate_set_is_what_detects_an_odd_meter(beats_per_bar: int) -> None:
    """The odd meter is reachable only once its numerator is requested."""
    times, strengths = _beat_series(beats_per_bar)

    default_result = libsonare.estimate_meter(times, strengths)
    widened_result = libsonare.estimate_meter(
        times, strengths, candidate_numerators=[3, 4, 5, 6, 7]
    )

    assert widened_result.time_signature.numerator == beats_per_bar
    assert default_result.time_signature.numerator != beats_per_bar


def test_requested_denominator_is_reported() -> None:
    """The beat unit comes back as requested rather than as the 4 default."""
    times, strengths = _beat_series(4)

    result = libsonare.estimate_meter(times, strengths, denominator=8)

    assert result.time_signature.denominator == 8
    assert all(candidate.denominator == 8 for candidate in result.candidates)


def test_candidate_scores_index_by_request_order() -> None:
    """Scores follow the requested order; candidates follow descending support."""
    times, strengths = _beat_series(4)
    requested = [7, 4, 3]

    result = libsonare.estimate_meter(times, strengths, candidate_numerators=requested)

    assert len(result.candidate_scores) == len(requested)
    best_requested = requested[result.candidate_scores.index(max(result.candidate_scores))]
    assert best_requested == 4
    # The candidate list is ordered by support, so its head is the winner while
    # the score list's head is whatever numerator was asked for first.
    assert result.candidates[0].numerator == 4
    assert requested[0] == 7


@pytest.mark.parametrize("beats_per_bar", [3, 4, 5, 6, 7])
def test_downbeat_phase_is_within_the_reported_numerator(beats_per_bar: int) -> None:
    """The phase names a beat of the reported bar, so it cannot reach it."""
    times, strengths = _beat_series(beats_per_bar)

    result = libsonare.estimate_meter(times, strengths, candidate_numerators=[3, 4, 5, 6, 7])

    assert 0 <= result.downbeat_phase < result.time_signature.numerator


def test_mismatched_beat_array_lengths_are_rejected() -> None:
    """One C beat count covers both arrays, so Python has to catch the pairing."""
    times, strengths = _beat_series(4)

    with pytest.raises(ValueError, match="same length"):
        libsonare.estimate_meter(times, strengths[:-1])


def test_empty_beat_series_is_rejected_by_the_core() -> None:
    """No beats is unanswerable, so it is an error rather than a fabricated 4/4.

    The wording comes from the core, so it is matched as an unanchored
    substring: the surrounding decoration differs per surface. It keeps the
    field name, because a bare "must not be empty" also matches the empty
    candidate-list guard and would let this pass for the wrong reason. The
    exception is a plain :class:`SonareError` rather than the
    :class:`SonareValueError` the Python-side length check raises, because this
    rejection happens in the core.
    """
    with pytest.raises(SonareError, match="beatTimes must not be empty") as excinfo:
        libsonare.estimate_meter([], [])
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_single_beat_series_still_returns_the_low_confidence_default() -> None:
    """The line is drawn at empty, not at short — one beat is still answered."""
    result = libsonare.estimate_meter([0.0], [1.0])

    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert result.time_signature.confidence == pytest.approx(0.5)
    assert result.downbeat_phase == 0


def test_empty_candidate_numerators_is_rejected_by_the_core() -> None:
    """A cleared candidate list is an error, not a request for the default set."""
    times, strengths = _beat_series(4)

    with pytest.raises(SonareError) as excinfo:
        libsonare.estimate_meter(times, strengths, candidate_numerators=[])
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_over_long_candidate_list_names_the_limit() -> None:
    """The flat C array cannot carry more than 16, and Python says so."""
    times, strengths = _beat_series(4)

    with pytest.raises(ValueError, match="16"):
        libsonare.estimate_meter(times, strengths, candidate_numerators=list(range(2, 19)))


def test_camel_case_aliases_mirror_snake_case() -> None:
    """The camelCase aliases return the same values as the snake_case fields."""
    times, strengths = _beat_series(4)

    result = libsonare.estimate_meter(times, strengths)

    assert result.timeSignature == result.time_signature
    assert result.downbeatPhase == result.downbeat_phase
    assert result.candidateScores == result.candidate_scores


def _accented_signal(
    sample_rate: int = 22050,
    bpm: float = 240.0,
    beats_per_bar: int = 4,
    bars: int = 1,
) -> list[float]:
    """A percussive click track accented on every bar start."""
    beat_seconds = 60.0 / bpm
    n = int(sample_rate * beat_seconds * beats_per_bar * bars)
    out: list[float] = []
    for i in range(n):
        t = i / sample_rate
        beat_index = int(t / beat_seconds)
        phase = t - beat_index * beat_seconds
        is_downbeat = beat_index % beats_per_bar == 0
        envelope = (1.0 if is_downbeat else 0.45) * math.exp(-phase * 26.0)
        frequency = 110.0 if is_downbeat else 220.0
        tone = math.sin(2 * math.pi * frequency * t) + 0.6 * math.sin(
            2 * math.pi * frequency * 3 * t
        )
        transient = math.sin(2 * math.pi * 3000.0 * t)
        out.append(0.7 * envelope * (0.7 * tone + 0.3 * transient))
    return out


@pytest.fixture(scope="module")
def analyzed() -> libsonare.AnalysisResult:
    """One analyze() pass over a click track, shared by the observation tests."""
    return libsonare.analyze(_accented_signal(), sample_rate=22050)


def test_analyze_reports_beat_observations(analyzed: libsonare.AnalysisResult) -> None:
    """The evidence streams reach the Python result through the JSON path."""
    observations = analyzed.beat_observations

    assert observations is not None
    assert isinstance(observations, libsonare.AnalysisBeatObservations)
    assert analyzed.beatObservations is observations


def test_non_empty_observation_streams_are_beat_indexed(
    analyzed: libsonare.AnalysisResult,
) -> None:
    """Each stream is one value per beat; empty means unavailable, not zeroed."""
    observations = analyzed.beat_observations
    assert observations is not None

    beat_count = len(analyzed.beat_times)
    assert beat_count > 0
    for stream in (
        observations.onset_strength,
        observations.low_frequency_energy,
        observations.chord_change,
    ):
        assert stream == [] or len(stream) == beat_count


def test_onset_strength_is_not_the_raw_beat_strength(
    analyzed: libsonare.AnalysisResult,
) -> None:
    """The windowed observation and the raw envelope frame are different values.

    ``beat_strengths`` is a single unwindowed frame of the onset envelope at
    the beat's frame; ``beat_observations.onset_strength`` is the windowed
    aggregate the downbeat pass scores. They are genuinely distinct
    quantities, so an element-wise match would mean one of them stopped being
    what it claims to be.
    """
    observations = analyzed.beat_observations
    assert observations is not None
    assert observations.onset_strength, "the click track should produce an onset stream"

    assert observations.onset_strength != analyzed.beat_strengths


def test_observations_feed_estimate_meter(analyzed: libsonare.AnalysisResult) -> None:
    """The documented strength source re-scores an analysis without audio."""
    observations = analyzed.beat_observations
    assert observations is not None
    assert len(observations.onset_strength) == len(analyzed.beat_times)

    result = libsonare.estimate_meter(analyzed.beat_times, observations.onset_strength)

    assert result.time_signature.numerator in (3, 4, 6)
    assert 0 <= result.downbeat_phase < result.time_signature.numerator
