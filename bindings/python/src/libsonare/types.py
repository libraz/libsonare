"""Public type definitions for libsonare."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum


class PitchClass(IntEnum):
    """Musical pitch class (chromatic scale)."""

    C = 0
    CS = 1
    D = 2
    DS = 3
    E = 4
    F = 5
    FS = 6
    G = 7
    GS = 8
    A = 9
    AS = 10
    B = 11

    def __str__(self) -> str:
        _names = {
            0: "C",
            1: "C#",
            2: "D",
            3: "D#",
            4: "E",
            5: "F",
            6: "F#",
            7: "G",
            8: "G#",
            9: "A",
            10: "A#",
            11: "B",
        }
        return _names[self.value]


class Mode(IntEnum):
    """Musical mode."""

    MAJOR = 0
    MINOR = 1

    def __str__(self) -> str:
        return "major" if self == Mode.MAJOR else "minor"


@dataclass(frozen=True, slots=True)
class Key:
    """Detected musical key."""

    root: PitchClass
    mode: Mode
    confidence: float

    def __str__(self) -> str:
        return f"{self.root} {self.mode}"


@dataclass(frozen=True, slots=True)
class TimeSignature:
    """Detected time signature."""

    numerator: int
    denominator: int
    confidence: float

    def __str__(self) -> str:
        return f"{self.numerator}/{self.denominator}"


@dataclass(frozen=True, slots=True)
class AnalysisResult:
    """Full audio analysis result."""

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beat_times: list[float]
