"""Tests for the full analyze() JSON path, melody pYIN/center, and progress.

These cover the full analysis result surface:
- ``analyze()`` returning the full result (chords/sections/timbre/dynamics/
  rhythm/melody/form plus per-beat strengths) via ``sonare_analyze_json``.
- ``analyze_melody(use_pyin=...)`` / ``center=...`` routing through
  ``sonare_analyze_melody_ex``.
- ``analyze_with_progress`` invoking the progress callback.
"""

from __future__ import annotations

import math

import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


def _has_analyze_json() -> bool:
    """Whether the loaded library exposes the JSON analysis entry point."""
    try:
        from libsonare._runtime import _get_lib

        return hasattr(_get_lib(), "sonare_analyze_json")
    except Exception:
        return False


def _has_analyze_json_progress() -> bool:
    try:
        from libsonare._runtime import _get_lib

        return hasattr(_get_lib(), "sonare_analyze_json_with_progress")
    except Exception:
        return False


def _has_analyze_melody_ex() -> bool:
    try:
        from libsonare._runtime import _get_lib

        return hasattr(_get_lib(), "sonare_analyze_melody_ex")
    except Exception:
        return False


requires_json = pytest.mark.skipif(
    not _has_analyze_json(),
    reason="libsonare built without sonare_analyze_json",
)
requires_json_progress = pytest.mark.skipif(
    not _has_analyze_json_progress(),
    reason="libsonare built without sonare_analyze_json_with_progress",
)
requires_melody_ex = pytest.mark.skipif(
    not _has_analyze_melody_ex(),
    reason="libsonare built without sonare_analyze_melody_ex",
)


def _generate_test_signal(sample_rate: int = 22050, duration: float = 4.0) -> list[float]:
    """Generate a pulsed C-major-ish tonal signal with a steady beat.

    The amplitude is gated at ~2 Hz so the analyzer has both tonal content
    (for key/chords) and a periodic envelope (for beats/rhythm).
    """
    n = int(sample_rate * duration)
    freqs = (261.63, 329.63, 392.0)  # C major triad
    beat_hz = 2.0
    out: list[float] = []
    for i in range(n):
        t = i / sample_rate
        env = 0.5 + 0.5 * math.cos(2 * math.pi * beat_hz * t)
        sample = sum(math.sin(2 * math.pi * f * t) for f in freqs) / len(freqs)
        out.append(0.6 * env * sample)
    return out


def test_analyze_returns_extended_fields() -> None:
    """analyze() returns the full set of fields with correct types."""
    from libsonare import (
        AnalysisDynamics,
        AnalysisMelody,
        AnalysisResult,
        AnalysisRhythm,
        AnalysisTimbre,
        Key,
        TimeSignature,
    )
    from libsonare.types import Mode, PitchClass

    samples = _generate_test_signal()
    result = libsonare.analyze(samples, sample_rate=22050)

    assert isinstance(result, AnalysisResult)

    # Existing fields preserved with correct types.
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    assert isinstance(result.key, Key)
    assert isinstance(result.key.root, PitchClass)
    assert isinstance(result.key.mode, Mode)
    assert isinstance(result.time_signature, TimeSignature)
    assert isinstance(result.beat_times, list)
    assert all(isinstance(t, float) for t in result.beat_times)

    # New fields present.
    assert isinstance(result.beat_strengths, list)
    assert isinstance(result.chords, list)
    assert isinstance(result.sections, list)
    assert isinstance(result.form, str)

    if _has_analyze_json():
        # JSON path: rich sub-results are populated.
        assert isinstance(result.timbre, AnalysisTimbre)
        assert isinstance(result.dynamics, AnalysisDynamics)
        assert isinstance(result.rhythm, AnalysisRhythm)
        assert isinstance(result.melody, AnalysisMelody)


@requires_json
def test_analyze_beat_strengths_align_with_beat_times() -> None:
    """beat_strengths has the same length as beat_times (JSON path)."""
    samples = _generate_test_signal()
    result = libsonare.analyze(samples, sample_rate=22050)
    assert len(result.beat_strengths) == len(result.beat_times)
    assert all(isinstance(s, float) for s in result.beat_strengths)


@requires_json
def test_analyze_sub_result_types() -> None:
    """The JSON-decoded sub-results expose the expected scalar fields."""
    from libsonare import Chord, Section
    from libsonare.types import PitchClass, SectionType

    samples = _generate_test_signal()
    result = libsonare.analyze(samples, sample_rate=22050)

    # Timbre / dynamics / rhythm scalar shapes.
    assert isinstance(result.timbre.brightness, float)
    assert isinstance(result.timbre.complexity, float)
    assert isinstance(result.dynamics.dynamic_range_db, float)
    assert isinstance(result.dynamics.is_compressed, bool)
    assert isinstance(result.rhythm.syncopation, float)
    assert isinstance(result.rhythm.groove_type, str)
    assert isinstance(result.melody.mean_frequency, float)
    assert isinstance(result.melody.pitches, list)

    # Chords map to Chord with enum roots; sections to Section with enum types.
    for chord in result.chords:
        assert isinstance(chord, Chord)
        assert isinstance(chord.root, PitchClass)
        assert isinstance(chord.quality, str)
    for section in result.sections:
        assert isinstance(section, Section)
        assert isinstance(section.type, SectionType)

    # camelCase aliases mirror the snake_case fields.
    assert result.dynamics.dynamicRangeDb == result.dynamics.dynamic_range_db
    assert result.rhythm.grooveType == result.rhythm.groove_type
    assert result.melody.meanFrequency == result.melody.mean_frequency


def test_analyze_melody_default_runs() -> None:
    """analyze_melody with defaults returns a MelodyResult contour."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    result = libsonare.analyze_melody(samples, sample_rate=22050)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)
    assert isinstance(result.mean_frequency, float)


@requires_melody_ex
def test_analyze_melody_pyin_runs() -> None:
    """analyze_melody(use_pyin=True) runs via the extended entry point."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    result = libsonare.analyze_melody(samples, sample_rate=22050, use_pyin=True)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)


@requires_melody_ex
def test_analyze_melody_center_flag_runs() -> None:
    """analyze_melody(center=False) runs via the extended entry point."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    centered = libsonare.analyze_melody(samples, sample_rate=22050, center=True)
    uncentered = libsonare.analyze_melody(samples, sample_rate=22050, center=False)
    assert isinstance(centered, MelodyResult)
    assert isinstance(uncentered, MelodyResult)


@requires_json_progress
def test_analyze_with_progress_invokes_callback() -> None:
    """analyze_with_progress fires the callback and returns a rich result."""
    from libsonare import AnalysisResult

    samples = _generate_test_signal()
    calls: list[tuple[float, str]] = []

    def _on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = libsonare.analyze_with_progress(samples, sample_rate=22050, on_progress=_on_progress)

    assert isinstance(result, AnalysisResult)
    assert len(calls) > 0
    for progress, stage in calls:
        assert isinstance(progress, float)
        assert isinstance(stage, str)


@requires_json_progress
def test_analyze_with_progress_no_callback() -> None:
    """analyze_with_progress works with on_progress=None (null callback)."""
    from libsonare import AnalysisResult

    samples = _generate_test_signal()
    result = libsonare.analyze_with_progress(samples, sample_rate=22050)
    assert isinstance(result, AnalysisResult)


@requires_json_progress
def test_analyze_with_progress_matches_analyze_shape() -> None:
    """analyze_with_progress returns the same-shaped result as analyze()."""
    samples = _generate_test_signal()
    a = libsonare.analyze(samples, sample_rate=22050)
    b = libsonare.analyze_with_progress(samples, sample_rate=22050)

    # Same field presence / list lengths align (deterministic on identical input).
    assert len(a.beat_times) == len(b.beat_times)
    assert len(a.beat_strengths) == len(b.beat_strengths)
    assert len(a.chords) == len(b.chords)
    assert len(a.sections) == len(b.sections)
    assert a.form == b.form
    assert (a.timbre is None) == (b.timbre is None)
    assert (a.dynamics is None) == (b.dynamics is None)
    assert (a.rhythm is None) == (b.rhythm is None)
    assert (a.melody is None) == (b.melody is None)
