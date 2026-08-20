"""Tests for the adaptive-tempo and meter-candidate options of ``analyze()``.

These cover the tail of ``SonareMusicAnalyzeOptions``:
- ``downbeat_indices`` / ``downbeat_phase`` reaching the Python result.
- The Python keyword defaults agreeing with the native option defaults.
- Rejection of the option values the core refuses.
- A widened numerator set resolving a meter the default set cannot.
"""

from __future__ import annotations

import inspect
import math
from typing import Any

import pytest

import libsonare
from libsonare import ErrorCode, SonareError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


def _accented_signal(
    sample_rate: int = 22050,
    bpm: float = 240.0,
    beats_per_bar: int = 4,
    bars: int = 2,
) -> list[float]:
    """Generate a percussive click track accented on every bar start.

    Each beat is a decaying tonal burst; the first beat of a bar is louder and
    an octave lower, which is the cue both the beat tracker and the meter
    estimator key on.
    """
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
def four_four_result() -> libsonare.AnalysisResult:
    """One analyze() pass over a plain 4/4 click track, shared by the module."""
    return libsonare.analyze(_accented_signal(), sample_rate=22050)


@pytest.fixture(scope="module")
def native_option_defaults() -> Any:
    """The C ABI's own SonareMusicAnalyzeOptions defaults."""
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    if not hasattr(lib, "sonare_music_analyze_options_default"):
        pytest.skip("libsonare built without sonare_music_analyze_options_default")
    return lib.sonare_music_analyze_options_default()


def test_analyze_reports_downbeats_as_indices_into_beats(four_four_result) -> None:
    """downbeat_indices addresses beat_times; it is not a parallel array."""
    result = four_four_result

    assert isinstance(result.downbeat_indices, list)
    assert all(isinstance(index, int) for index in result.downbeat_indices)
    assert isinstance(result.downbeat_phase, int)

    assert result.downbeat_indices, "an accented click track should yield at least one bar start"
    assert len(result.downbeat_indices) <= len(result.beat_times)
    for index in result.downbeat_indices:
        assert 0 <= index < len(result.beat_times)
    assert result.downbeat_indices == sorted(set(result.downbeat_indices))


def test_downbeat_camel_case_aliases_mirror_snake_case(four_four_result) -> None:
    """The camelCase aliases return the same objects as the snake_case fields."""
    assert four_four_result.downbeatIndices == four_four_result.downbeat_indices
    assert four_four_result.downbeatPhase == four_four_result.downbeat_phase


def test_analyze_keyword_defaults_match_native_option_defaults(native_option_defaults) -> None:
    """The Python keyword literals must not drift from the C ABI defaults."""
    defaults = inspect.signature(libsonare.analyze).parameters
    native = native_option_defaults

    assert defaults["adaptive_tempo"].default is bool(native.adaptive_tempo)
    assert (
        defaults["tempo_update_interval_beats"].default == native.tempo_update_interval_beats == 8
    )
    assert defaults["meter_denominator"].default == native.meter_denominator == 4

    # A None default selects the module constant, so compare that instead.
    from libsonare._analysis_reports import _DEFAULT_METER_CANDIDATE_NUMERATORS

    assert defaults["meter_candidate_numerators"].default is None
    native_numerators = tuple(
        native.meter_candidate_numerators[i] for i in range(native.meter_candidate_numerator_count)
    )
    assert tuple(_DEFAULT_METER_CANDIDATE_NUMERATORS) == native_numerators == (3, 4, 6)


@pytest.mark.parametrize(
    ("kwargs", "reason"),
    [
        ({"meter_candidate_numerators": []}, "empty candidate set"),
        ({"meter_candidate_numerators": list(range(2, 19))}, "17 entries exceeds the array"),
        ({"meter_candidate_numerators": [1, 4]}, "numerator below 2"),
        ({"meter_candidate_numerators": [4, 33]}, "numerator above 32"),
        ({"meter_denominator": 3}, "denominator is not a power of two"),
        ({"tempo_update_interval_beats": 0}, "non-positive tempo update interval"),
    ],
)
def test_invalid_meter_options_are_rejected(kwargs: dict[str, Any], reason: str) -> None:
    """Every out-of-contract option value surfaces as an InvalidParameter error.

    Only the over-long list is caught in Python (the flat C array cannot carry
    it); the rest come back from the core's own validation, and both paths
    carry the same error code.
    """
    samples = _accented_signal(bars=1)
    with pytest.raises(SonareError) as excinfo:
        libsonare.analyze(samples, sample_rate=22050, **kwargs)
    assert excinfo.value.code == int(ErrorCode.INVALID_PARAMETER), reason


def test_over_long_numerator_list_names_the_limit() -> None:
    """The Python-side rejection states the capacity it enforces."""
    with pytest.raises(ValueError, match="16"):
        libsonare.analyze(
            _accented_signal(bars=1),
            sample_rate=22050,
            meter_candidate_numerators=list(range(2, 19)),
        )


@pytest.fixture(scope="module")
def five_four_results() -> tuple[libsonare.AnalysisResult, libsonare.AnalysisResult]:
    """A five-beat click track analyzed with the default and a widened set."""
    samples = _accented_signal(bpm=170.0, beats_per_bar=5, bars=4)
    default_result = libsonare.analyze(samples, sample_rate=22050)
    widened_result = libsonare.analyze(
        samples, sample_rate=22050, meter_candidate_numerators=[5, 7]
    )
    return default_result, widened_result


@pytest.mark.slow
def test_widened_numerator_set_detects_an_odd_meter(five_four_results) -> None:
    """A numerator outside the default set is only reachable once requested."""
    default_result, widened_result = five_four_results

    assert widened_result.time_signature.numerator in (5, 7)
    assert default_result.time_signature.numerator not in (5, 7)
