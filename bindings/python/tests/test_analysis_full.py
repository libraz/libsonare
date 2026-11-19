"""Tests for the full analyze() JSON path, melody pYIN/center, and progress.

These cover the full analysis result surface:
- ``analyze()`` returning the full result (chords/sections/timbre/dynamics/
  rhythm/melody/form plus per-beat strengths) via ``sonare_analyze_json``.
- ``analyze_melody(use_pyin=...)`` / ``center=...`` routing through
  ``sonare_analyze_melody_ex``.
- ``analyze(on_progress=...)`` invoking the progress callback.
"""

from __future__ import annotations

import math
from typing import Any, get_args

import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")


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


# The four summary blocks the core writes on every analysis; synthetic payloads
# below carry them so each test exercises only the field it is about.
_SUMMARY_BLOCKS: dict[str, object] = {
    "timbre": {
        "brightness": 0.5,
        "warmth": 0.5,
        "density": 0.5,
        "roughness": 0.5,
        "complexity": 0.5,
    },
    "dynamics": {
        "dynamicRangeDb": 10.0,
        "peakDb": -1.0,
        "rmsDb": -12.0,
        "crestFactor": 4.0,
        "loudnessRangeLu": 5.0,
        "isCompressed": False,
    },
    "rhythm": {
        "timeSignature": {"numerator": 4, "denominator": 4, "confidence": 0.8},
        "syncopation": 0.1,
        "grooveType": "straight",
        "patternRegularity": 0.9,
        "tempoStability": 0.9,
    },
    "melody": {
        "pitchRangeOctaves": 1.0,
        "pitchStability": 0.5,
        "meanFrequencyHz": 220.0,
        "vibratoRateHz": 0.0,
        "pitches": [],
    },
}


def test_parse_analysis_json_requires_every_summary_block() -> None:
    """The core always writes timbre, dynamics, rhythm and melody, so a missing one is refused."""
    from libsonare._analysis_detection import _parse_analysis_json

    for missing in _SUMMARY_BLOCKS:
        payload = {"key": {}, "beats": [], "tuningCents": 0.0, **_SUMMARY_BLOCKS}
        del payload[missing]
        with pytest.raises(KeyError, match=missing):
            _parse_analysis_json(payload)


def test_analyze_always_carries_every_summary_block(analyze_result: Any) -> None:
    from libsonare import AnalysisDynamics, AnalysisRhythm, MelodyResult, TimbreFrame

    result = analyze_result
    assert isinstance(result.timbre, TimbreFrame)
    assert isinstance(result.dynamics, AnalysisDynamics)
    assert isinstance(result.rhythm, AnalysisRhythm)
    assert isinstance(result.melody, MelodyResult)


def test_parse_analysis_json_retains_tempo_and_meter_hypotheses() -> None:
    """The rich JSON decoder keeps the existing estimator candidates intact."""
    from libsonare._analysis_detection import _parse_analysis_json

    result = _parse_analysis_json(
        {
            "tuningCents": 0.0,
            "bpm": 120.0,
            "bpmConfidence": 0.9,
            "bpmCandidates": [
                {"bpm": 120.0, "confidence": 0.9, "relation": "primary"},
                {"bpm": 60.0, "confidence": 0.4, "relation": "half"},
            ],
            "key": {},
            "timeSignature": {"numerator": 4, "denominator": 4, "confidence": 0.8},
            "timeSignatureCandidates": [
                {"numerator": 4, "denominator": 4, "confidence": 0.7},
                {"numerator": 3, "denominator": 4, "confidence": 0.3},
            ],
            "beats": [],
            **_SUMMARY_BLOCKS,
        }
    )

    assert [(candidate.bpm, candidate.relation) for candidate in result.bpm_candidates] == [
        (120.0, "primary"),
        (60.0, "half"),
    ]
    assert [str(candidate) for candidate in result.time_signature_candidates] == ["4/4", "3/4"]


@pytest.fixture(scope="module")
def analyze_result():
    """One full analyze() pass shared by every shape/type assertion below.

    The full pipeline takes seconds even on a 4 s signal; the tests in this
    module only inspect different facets of the same deterministic result, so
    computing it once keeps the default suite fast.
    """
    return libsonare.analyze(_generate_test_signal(), sample_rate=22050)


@pytest.fixture(scope="module")
def progress_result():
    """One analyze(on_progress=...) pass plus its recorded callback calls."""
    calls: list[tuple[float, str]] = []

    def _on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = libsonare.analyze(_generate_test_signal(), sample_rate=22050, on_progress=_on_progress)
    return result, calls


def test_analyze_returns_extended_fields(analyze_result) -> None:
    """analyze() returns the full set of fields with correct types."""
    from libsonare import (
        AnalysisDynamics,
        AnalysisResult,
        AnalysisRhythm,
        Key,
        MelodyResult,
        TimbreFrame,
        TimeSignature,
    )
    from libsonare.types import KeyMode, PitchClass

    result = analyze_result

    assert isinstance(result, AnalysisResult)

    # Existing fields preserved with correct types.
    assert isinstance(result.bpm, float)
    assert isinstance(result.bpm_confidence, float)
    assert isinstance(result.key, Key)
    assert isinstance(result.key.root, PitchClass)
    assert result.key.mode in get_args(KeyMode)
    assert isinstance(result.time_signature, TimeSignature)
    assert isinstance(result.beats, list)
    assert all(isinstance(b, libsonare.Beat) for b in result.beats)
    assert all(isinstance(b.time_sec, float) for b in result.beats)

    # New fields present.
    assert isinstance(result.downbeats, list)
    assert isinstance(result.chords, list)
    assert isinstance(result.sections, list)
    assert isinstance(result.form, str)

    assert isinstance(result.timbre, TimbreFrame)
    assert isinstance(result.dynamics, AnalysisDynamics)
    assert isinstance(result.rhythm, AnalysisRhythm)
    assert isinstance(result.melody, MelodyResult)


def test_analysis_json_uses_canonical_display_names() -> None:
    """Names emitted by the core are authoritative; Python must not rebuild them."""
    from libsonare._analysis_detection import _parse_analysis_json

    result = _parse_analysis_json(
        {
            "tuningCents": 0.0,
            "chords": [
                {
                    "root": 0,
                    "bass": 0,
                    "quality": 0,
                    "startSec": 0,
                    "endSec": 1,
                    "confidence": 1,
                    "name": "canonical-chord",
                }
            ],
            "sections": [
                {
                    "type": 2,
                    "startSec": 0,
                    "endSec": 1,
                    "energyLevel": 0.5,
                    "confidence": 1,
                    "name": "canonical-section",
                    "group": 3,
                }
            ],
            **_SUMMARY_BLOCKS,
        }
    )
    assert result.chords[0].name == "canonical-chord"
    assert result.sections[0].name == "canonical-section"
    assert result.sections[0].group == 3


def test_analyze_beats_carry_a_strength_each(analyze_result) -> None:
    """Every beat has a float strength and downbeats are drawn from the beats (JSON path)."""
    result = analyze_result
    assert all(isinstance(b.strength, float) for b in result.beats)
    beat_times = {b.time_sec for b in result.beats}
    assert all(d.time_sec in beat_times for d in result.downbeats)


def test_analyze_sub_result_types(analyze_result) -> None:
    """The JSON-decoded sub-results expose the expected scalar fields."""
    from libsonare import AnalysisChord, Section
    from libsonare.types import PitchClass, SectionType

    result = analyze_result

    # Timbre / dynamics / rhythm scalar shapes.
    assert isinstance(result.timbre.brightness, float)
    assert isinstance(result.timbre.complexity, float)
    assert isinstance(result.dynamics.dynamic_range_db, float)
    assert isinstance(result.dynamics.is_compressed, bool)
    assert isinstance(result.rhythm.syncopation, float)
    assert isinstance(result.rhythm.groove_type, str)
    assert isinstance(result.melody.mean_frequency_hz, float)
    assert isinstance(result.melody.points, list)

    # Chords map to Chord with enum roots; sections to Section with enum types.
    for chord in result.chords:
        assert isinstance(chord, AnalysisChord)
        assert isinstance(chord.root, PitchClass)
        assert isinstance(chord.quality, str)
    for section in result.sections:
        assert isinstance(section, Section)
        assert section.type in get_args(SectionType)


def test_analyze_melody_default_runs() -> None:
    """analyze_melody with defaults returns a MelodyResult contour."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    result = libsonare.analyze_melody(samples, sample_rate=22050)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)
    assert isinstance(result.mean_frequency_hz, float)


def test_analyze_melody_pyin_runs() -> None:
    """analyze_melody(use_pyin=True) runs via the extended entry point."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    result = libsonare.analyze_melody(samples, sample_rate=22050, use_pyin=True)
    assert isinstance(result, MelodyResult)
    assert isinstance(result.points, list)


def test_analyze_melody_center_flag_runs() -> None:
    """analyze_melody(center=False) runs via the extended entry point."""
    from libsonare import MelodyResult

    samples = _generate_test_signal(duration=2.0)
    centered = libsonare.analyze_melody(samples, sample_rate=22050, center=True)
    uncentered = libsonare.analyze_melody(samples, sample_rate=22050, center=False)
    assert isinstance(centered, MelodyResult)
    assert isinstance(uncentered, MelodyResult)


def test_analyze_progress_invokes_callback(progress_result) -> None:
    """A void progress callback remains compatible and returns a rich result."""
    from libsonare import AnalysisResult

    result, calls = progress_result

    assert isinstance(result, AnalysisResult)
    assert len(calls) > 0
    for progress, stage in calls:
        assert isinstance(progress, float)
        assert isinstance(stage, str)


def test_analyze_progress_false_result_does_not_cancel() -> None:
    """A progress callback's boolean return value is ignored."""
    calls: list[float] = []

    def on_progress(progress: float, _stage: str) -> object:
        calls.append(progress)
        return False

    result = libsonare.analyze(
        _generate_test_signal(duration=1.0), sample_rate=22050, on_progress=on_progress
    )

    assert any(progress > 0.5 for progress in calls)
    assert isinstance(result, libsonare.AnalysisResult)


def test_analyze_progress_cancel_callable_cancels_without_a_result() -> None:
    """The keyword-only cancel callable maps to the native cancellation callback."""
    result = None

    with pytest.raises(libsonare.SonareError) as exc:
        result = libsonare.analyze(
            _generate_test_signal(duration=1.0), sample_rate=22050, cancel=lambda: True
        )

    assert exc.value.code == 8
    assert result is None


def test_analyze_progress_no_callback() -> None:
    """analyze takes the cancellable path with no progress callback registered."""
    from libsonare import AnalysisResult

    # A short signal suffices: only the NULL-callback code path is under test.
    samples = _generate_test_signal(duration=1.0)
    result = libsonare.analyze(samples, sample_rate=22050, cancel=lambda: False)
    assert isinstance(result, AnalysisResult)


def test_analyze_progress_matches_analyze_shape(analyze_result, progress_result) -> None:
    """analyze with a callback returns the same-shaped result as without."""
    a = analyze_result
    b, _ = progress_result

    # Same field presence / list lengths align (deterministic on identical input).
    assert len(a.beats) == len(b.beats)
    assert len(a.downbeats) == len(b.downbeats)
    assert len(a.chords) == len(b.chords)
    assert len(a.sections) == len(b.sections)
    assert a.form == b.form
    assert a.timbre == b.timbre
    assert a.dynamics == b.dynamics
    assert a.rhythm == b.rhythm
    assert a.melody == b.melody


def test_parse_analysis_json_rejects_an_unmirrored_section_type() -> None:
    """A section ordinal outside SectionType names a drifted enum mirror.

    Substituting "unknown" for it would present the drift as an analysis result,
    so the decoder raises and names the ordinal instead.
    """
    from libsonare import SonareValueError
    from libsonare._analysis_detection import _parse_analysis_json

    def parse(section: dict[str, object]) -> object:
        return _parse_analysis_json(
            {
                "key": {},
                "beats": [],
                "sections": [section],
                "tuningCents": 0.0,
                **_SUMMARY_BLOCKS,
            }
        )

    with pytest.raises(SonareValueError, match="99"):
        parse({"type": 99, "startSec": 0.0, "endSec": 1.0})

    with pytest.raises(SonareValueError, match="missing its type"):
        parse({"startSec": 0.0, "endSec": 1.0})

    result = parse({"type": 3, "startSec": 0.0, "endSec": 1.0, "group": 0})
    assert result.sections[0].type == "chorus"  # type: ignore[attr-defined]


def test_parse_analysis_json_rejects_an_unmirrored_chord_quality() -> None:
    """An "unknown" chord quality and an unmapped ordinal are not the same thing.

    The analyzer reports "unknown" through an ordinal of its own, so an ordinal
    outside the table means the mirror has drifted rather than that the chord
    was unrecognized.
    """
    from libsonare import SonareValueError
    from libsonare._analysis_detection import _parse_analysis_json

    def parse(chord: dict[str, object]) -> object:
        return _parse_analysis_json(
            {"key": {}, "beats": [], "chords": [chord], "tuningCents": 0.0, **_SUMMARY_BLOCKS}
        )

    with pytest.raises(SonareValueError, match="99"):
        parse({"root": 0, "quality": 99, "startSec": 0.0, "endSec": 1.0})

    # The genuine "unknown" quality still round-trips, including as the default
    # when the producer omitted the field.
    assert (
        parse({"root": 0, "quality": 9, "startSec": 0.0, "endSec": 1.0}).chords[0].quality
        == "unknown"
    )  # type: ignore[attr-defined]
    assert parse({"root": 0, "startSec": 0.0, "endSec": 1.0}).chords[0].quality == "unknown"  # type: ignore[attr-defined]
