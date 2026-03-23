"""libsonare - librosa-like audio analysis library (Python binding)."""

from .analyzer import analyze, detect_beats, detect_bpm, detect_key, detect_onsets, version
from .audio import Audio
from .types import AnalysisResult, Key, Mode, PitchClass, TimeSignature

__all__ = [
    "AnalysisResult",
    "Audio",
    "Key",
    "Mode",
    "PitchClass",
    "TimeSignature",
    "analyze",
    "detect_beats",
    "detect_bpm",
    "detect_key",
    "detect_onsets",
    "version",
]
