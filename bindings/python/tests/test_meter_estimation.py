"""Tests for ``estimate_meter`` and ``AnalysisResult.beat_observations``.

``estimate_meter`` scores a caller-supplied beat series, so most of these run
without audio at all. The ``beat_observations`` cases need a real ``analyze()``
pass and share one module-scoped result.
"""

from __future__ import annotations

import math

import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError

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


def test_empty_beat_series_is_rejected_by_the_facade() -> None:
    """No beats is unanswerable, so it is an error rather than a fabricated 4/4.

    The rejection now comes from the facade preflight rather than from the core:
    ``beat_times`` is a buffer parameter like any other, and letting only this
    one report through the core gave the same class of mistake two different
    diagnostics — a :class:`SonareError` naming the C field ``beatTimes`` here,
    a :class:`SonareValueError` naming the Python argument everywhere else. The
    message keeps the argument name, because a bare "must not be empty" also
    matches the empty candidate-list guard and would pass for the wrong reason.
    """
    with pytest.raises(SonareValueError, match="beat_times must not be empty") as excinfo:
        libsonare.estimate_meter([], [])
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_negative_beat_times_still_reach_the_core_check() -> None:
    """The facade preflight is a diagnostic layer, not a replacement.

    It only rejects empty and non-finite input, so the core's own domain rule
    stays reachable and keeps reporting under its own wording.
    """
    with pytest.raises(SonareError, match="beatTimes") as excinfo:
        libsonare.estimate_meter([-1.0, 0.5], [1.0, 1.0])
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_single_beat_series_still_returns_the_low_confidence_default() -> None:
    """The line is drawn at empty, not at short — one beat is still answered."""
    result = libsonare.estimate_meter([0.0], [1.0])

    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert result.time_signature.confidence == pytest.approx(0.5)
    assert result.downbeat_phase == 0
    assert result.searched is False


def test_searched_separates_the_fallback_from_a_detection() -> None:
    """Below the eight-beat search floor nothing is scored, and it says so.

    A caller scoring many spans — a meter map over a beat series, say — needs
    the fallback to be recognizable as one. The confidence alone does not do
    that: 0.5 reads as a middling detection rather than as "no search ran".
    """
    times, strengths = _beat_series(4, bars=8)
    detected = libsonare.estimate_meter(times, strengths)
    assert detected.searched is True

    for count in range(1, 8):
        result = libsonare.estimate_meter(times[:count], strengths[:count])
        assert result.searched is False, count
        # Every field belongs to the fallback rather than to a candidate.
        assert result.time_signature.numerator == 4
        assert result.grouping == [4]
        assert all(score == 0.0 for score in result.candidate_scores)

    # Eight beats is the first span that is actually searched.
    assert libsonare.estimate_meter(times[:8], strengths[:8]).searched is True


def test_scores_grow_with_the_span_and_so_do_not_rank_spans() -> None:
    """The documented reason a segmentation search must normalize for length."""
    short_times, short_strengths = _beat_series(4, bars=4)
    long_times, long_strengths = _beat_series(4, bars=16)

    short = libsonare.estimate_meter(short_times, short_strengths)
    long = libsonare.estimate_meter(long_times, long_strengths)

    assert short.time_signature.numerator == long.time_signature.numerator == 4
    # Same meter, same accent contrast, four times the beats: about twice the
    # score, because the evidence accumulates with the square root of the count.
    ratio = max(long.candidate_scores) / max(short.candidate_scores)
    assert 1.5 < ratio < 2.5


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


def test_candidate_scores_sit_below_zero_without_meter() -> None:
    """A beat series with no accent structure scores below the no-meter level.

    The scores are standardized against what a numerator reaches on beats
    carrying no meter, so zero is that level rather than an arbitrary floor,
    and a flat series lands under it. The wider the numerator the further
    under: it searched more phases and found nothing in any of them, which is
    exactly the advantage the standardization removes.
    """
    times = [index * 0.5 for index in range(48)]
    strengths = [0.5] * 48

    result = libsonare.estimate_meter(times, strengths, candidate_numerators=[3, 4, 13])

    assert all(score < 0.0 for score in result.candidate_scores)
    assert result.candidate_scores == sorted(result.candidate_scores, reverse=True)
    assert result.time_signature.confidence < 0.6


def test_widening_the_candidate_set_keeps_a_clear_meter() -> None:
    """Offering wider numerators does not pull a clear 4/4 away from 4."""
    times = [index * 0.5 for index in range(48)]
    strengths = [1.0 if index % 4 == 0 else 0.3 for index in range(48)]

    narrow = libsonare.estimate_meter(times, strengths)
    wide = libsonare.estimate_meter(
        times, strengths, candidate_numerators=[3, 4, 5, 6, 7, 9, 11, 12, 13]
    )

    assert narrow.time_signature.numerator == 4
    assert wide.time_signature.numerator == 4
    assert wide.time_signature.denominator == narrow.time_signature.denominator

    scores = dict(zip([3, 4, 5, 6, 7, 9, 11, 12, 13], wide.candidate_scores, strict=True))
    # A numerator sharing no period with the true one has nothing to align to
    # and must land below the no-meter level. 12 is excluded because it is a
    # multiple of 4 and genuinely does describe the same accents, and 6 because
    # it shares every third bar with 4 -- those are real partial support, not
    # the width advantage this guards against.
    for numerator in (3, 5, 7, 9, 11, 13):
        assert scores[numerator] < 0.0, f"{numerator} scored above the no-meter level"
    assert scores[4] == max(wide.candidate_scores)
    assert scores[4] - scores[12] > 2.0


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
    """One analyze() pass over a click track, shared by the observation tests.

    Long enough to carry the estimator past its eight-beat search floor, so the
    cases that feed the observations back into ``estimate_meter`` exercise the
    multi-comb search rather than the fixed default a short series reports. The
    tempo is one the beat tracker's default range admits, so the beats it
    returns are the clicks themselves and the accent period is the bar.
    """
    return libsonare.analyze(_accented_signal(bpm=120.0, bars=8), sample_rate=22050)


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
    """The documented strength source re-scores an analysis without audio.

    The material is accented on every fourth beat, so the documented input has
    to resolve as 4/4 and not merely land somewhere in the candidate set: the
    stream carries the envelope's own units, and a scoring path that read its
    absolute level rather than its contrast would answer with whichever
    candidate happens to be listed first.
    """
    observations = analyzed.beat_observations
    assert observations is not None
    assert len(observations.onset_strength) == len(analyzed.beat_times)
    assert len(analyzed.beat_times) >= 8, "the estimator only searches above its beat floor"

    result = libsonare.estimate_meter(analyzed.beat_times, observations.onset_strength)

    assert result.time_signature.numerator == 4
    assert result.time_signature.denominator == 4
    assert 0 <= result.downbeat_phase < result.time_signature.numerator
    assert result.candidate_scores.index(max(result.candidate_scores)) == 1


def test_estimate_meter_reads_accent_contrast_not_absolute_level(
    analyzed: libsonare.AnalysisResult,
) -> None:
    """Rescaling a beat series leaves the estimate untouched.

    ``beat_observations.onset_strength`` is a windowed aggregate in the onset
    envelope's units, so its values run well above 1 on ordinary material and a
    caller has no scale to hand it in. Scoring the series against its own
    maximum is what makes that safe; saturating it instead would flatten every
    beat to one value and tie every candidate.
    """
    observations = analyzed.beat_observations
    assert observations is not None
    assert max(observations.onset_strength) > 1.0, "the stream is not pre-normalized"

    peak = max(observations.onset_strength)
    normalized = [value / peak for value in observations.onset_strength]

    raw_result = libsonare.estimate_meter(analyzed.beat_times, observations.onset_strength)
    normalized_result = libsonare.estimate_meter(analyzed.beat_times, normalized)

    assert raw_result.time_signature.numerator == normalized_result.time_signature.numerator
    assert raw_result.time_signature.denominator == normalized_result.time_signature.denominator
    assert raw_result.time_signature.confidence == normalized_result.time_signature.confidence
    assert raw_result.downbeat_phase == normalized_result.downbeat_phase
    assert raw_result.candidate_scores == normalized_result.candidate_scores

    # A tie across every candidate is exactly what saturation produces, so name
    # it rather than leaving it to the equality above.
    assert len(set(raw_result.candidate_scores)) == len(raw_result.candidate_scores)


def _grouped_series(
    grouping: tuple[int, ...],
    bars: int = 12,
    accent: float = 1.0,
    secondary: float = 0.65,
    weak: float = 0.35,
) -> tuple[list[float], list[float]]:
    """A beat series whose bars divide exactly as ``grouping`` says.

    The downbeat is loudest, every following group starts on a middling beat and
    the rest are quiet, which is the accent shape an additive meter is written
    for.
    """
    accent_positions = set()
    position = 0
    for part in grouping[:-1]:
        position += part
        accent_positions.add(position)

    numerator = sum(grouping)
    count = numerator * bars
    times = [index * 0.5 for index in range(count)]
    strengths = [
        accent
        if index % numerator == 0
        else (secondary if index % numerator in accent_positions else weak)
        for index in range(count)
    ]
    return times, strengths


@pytest.mark.parametrize(
    "grouping",
    [(3, 2, 2), (2, 3, 2), (2, 2, 3), (3, 2), (2, 3), (2, 2, 2, 3), (3, 3, 3, 2, 2)],
)
def test_grouping_reports_how_the_bar_divides(grouping: tuple[int, ...]) -> None:
    """A seven comes back as 3+2+2 or 2+2+3, not as a bare seven.

    These layouts share a numerator and an accent count and differ only in where
    the accents fall, so the numerator alone cannot tell them apart.
    """
    times, strengths = _grouped_series(grouping)

    result = libsonare.estimate_meter(
        times, strengths, candidate_numerators=[3, 4, 5, 6, 7, 9, 11, 13]
    )

    assert result.time_signature.numerator == sum(grouping)
    assert tuple(result.grouping) == grouping


def test_grouping_always_sums_to_the_reported_numerator() -> None:
    """The invariant every consumer of the field relies on.

    Checked across the whole candidate range, so it covers the numerators too
    wide to divide as well as the ones that divide.
    """
    for numerator in range(2, 33):
        times, strengths = _beat_series(numerator, bars=6)
        result = libsonare.estimate_meter(times, strengths, candidate_numerators=[numerator])

        assert result.grouping, f"{numerator} reported no grouping"
        assert sum(result.grouping) == result.time_signature.numerator
        assert all(part > 0 for part in result.grouping)


def test_grouping_is_a_single_group_below_the_search_floor() -> None:
    """Too few beats to search reports an undivided bar, not a guessed 2+2."""
    times, strengths = _beat_series(4, bars=1)
    assert len(times) < 8

    result = libsonare.estimate_meter(times[:4], strengths[:4])

    assert result.time_signature.confidence <= 0.5
    assert result.grouping == [result.time_signature.numerator]


def test_how_a_six_divides_is_reported_as_a_grouping_not_a_beat_unit() -> None:
    """3+3 and 2+2+2 differ in the grouping; neither changes the requested unit.

    Whether a beat divides into three is measured between the beats, which this
    entry point never sees, so the compound reading is not one it can reach.
    """
    compound_times, compound_strengths = _grouped_series((3, 3))
    compound = libsonare.estimate_meter(compound_times, compound_strengths)

    assert compound.time_signature.numerator == 6
    assert compound.time_signature.denominator == 4
    assert compound.grouping == [3, 3]
    assert all(candidate.denominator == 4 for candidate in compound.candidates)

    simple_times, simple_strengths = _grouped_series((2, 2, 2))
    simple = libsonare.estimate_meter(simple_times, simple_strengths)

    assert simple.time_signature.numerator == 6
    assert simple.time_signature.denominator == 4
    assert simple.grouping == [2, 2, 2]

    # The unit follows the request even for the bar that divides into threes.
    in_eighths = libsonare.estimate_meter(compound_times, compound_strengths, denominator=8)
    assert in_eighths.time_signature.denominator == 8
    assert in_eighths.grouping == [3, 3]


def test_observations_feed_the_grouping(analyzed: libsonare.AnalysisResult) -> None:
    """The grouping survives the round trip through a real observation stream."""
    observations = analyzed.beat_observations
    assert observations is not None

    result = libsonare.estimate_meter(analyzed.beat_times, observations.onset_strength)

    assert sum(result.grouping) == result.time_signature.numerator
    assert result.grouping == [2, 2]
