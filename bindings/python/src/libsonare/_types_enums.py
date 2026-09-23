"""Integer enumerations on the public libsonare type surface.

They subclass :class:`enum.IntEnum` because the values cross the C ABI as
plain integers, so either the member or its number is accepted.
"""

from __future__ import annotations

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
    DORIAN = 2
    PHRYGIAN = 3
    LYDIAN = 4
    MIXOLYDIAN = 5
    LOCRIAN = 6

    def __str__(self) -> str:
        names = {
            Mode.MAJOR: "major",
            Mode.MINOR: "minor",
            Mode.DORIAN: "dorian",
            Mode.PHRYGIAN: "phrygian",
            Mode.LYDIAN: "lydian",
            Mode.MIXOLYDIAN: "mixolydian",
            Mode.LOCRIAN: "locrian",
        }
        return names[self]


class AutomationCurve(IntEnum):
    """Interpolation curve for scheduled mixer automation events."""

    LINEAR = 0
    EXPONENTIAL = 1
    HOLD = 2
    S_CURVE = 3

    def __str__(self) -> str:
        names = {
            AutomationCurve.LINEAR: "linear",
            AutomationCurve.EXPONENTIAL: "exponential",
            AutomationCurve.HOLD: "hold",
            AutomationCurve.S_CURVE: "s-curve",
        }
        return names[self]


class PanLaw(IntEnum):
    """Pan law for a mixer strip.

    On mono strips it changes centre gain. On stereo strips using Balance,
    centre remains unity and the selected law changes only the far-channel
    taper.
    """

    CONST_3DB = 0
    CONST_4_5DB = 1
    CONST_6DB = 2
    LINEAR_0DB = 3


class ChannelLayout(IntEnum):
    """Speaker bed layout for a bus or source (mirrors SonareChannelLayout).

    Plane order is WAVE_FORMAT_EXTENSIBLE: ``FIVE_POINT_ONE`` = L R C LFE Ls Rs,
    ``SEVEN_POINT_ONE`` = L R C LFE Ls Rs Lss Rss.
    """

    MONO = 0
    STEREO = 1
    FIVE_POINT_ONE = 2
    SEVEN_POINT_ONE = 3


class MeterTap(IntEnum):
    """Tap point at which a strip meter snapshot is read."""

    PRE_FADER = 0
    POST_FADER = 1


class SendTiming(IntEnum):
    """Pre/post-fader timing of a mixer strip send.

    POST_FADER is 0 so a zero-initialized C ABI send defaults to post-fader; the
    integer mirrors ``SonareSendTiming`` and is never serialized (scene/project
    JSON uses the strings ``"pre"``/``"post"``).
    """

    POST_FADER = 0
    PRE_FADER = 1


class SectionType(IntEnum):
    """Song-structure section type (mirrors sonare::SectionType ordinals).

    ``PRE_CHORUS`` is never produced by the analyzer: it has no detection
    branch, so filtering sections on it always yields an empty result. Every
    other value is reachable. ``UNKNOWN`` means the analyzer did not identify
    the segment -- no boundary was detected, or the segment matched none of the
    positive branches -- and comes with ``confidence`` 0.
    """

    INTRO = 0
    VERSE = 1
    PRE_CHORUS = 2
    CHORUS = 3
    BRIDGE = 4
    INSTRUMENTAL = 5
    OUTRO = 6
    UNKNOWN = 7


class EngineTelemetryType(IntEnum):
    """Realtime engine telemetry record type."""

    PROCESS_BLOCK = 0
    ERROR = 1


class EngineTelemetryError(IntEnum):
    """Recoverable realtime engine error codes."""

    NONE = 0
    COMMAND_QUEUE_OVERFLOW = 1
    PENDING_COMMAND_OVERFLOW = 2
    BOUNDARY_OVERFLOW = 3
    TELEMETRY_OVERFLOW = 4
    CAPTURE_OVERFLOW = 5
    MAX_BLOCK_EXCEEDED = 6
    UNKNOWN_TARGET = 7
    NON_REALTIME_SAFE_PARAMETER = 8
    NOT_PREPARED = 9
    NON_QUEUEABLE_COMMAND = 10
    AUTOMATION_BIND_TARGET_OVERFLOW = 11
    STALE_AUTOMATION_LANES = 12
    SMOOTHED_PARAMETER_CAPACITY = 13
    COMMAND_BACKLOG_DEFERRED = 14
    CLIP_PAGE_UNDERRUN = 15
    INSERT_AUTOMATION_OVERFLOW = 16
    MIDI_CLOCK_OVERFLOW = 17
    METRONOME_OVERFLOW = 18
    # Ordinal 19 is reserved by the WASM worklet protocol.
    MAX_CHANNELS_EXCEEDED = 20
    PARAMETER_BASE_OVERFLOW = 21


class KeyProfile(IntEnum):
    """Key-profile family used by profile-correlation key detection."""

    KRUMHANSL_SCHMUCKLER = 0
    TEMPERLEY = 1
    SHAATH = 2
    FARALDO_EDMT = 3
    FARALDO_EDMA = 4
    FARALDO_EDMM = 5
    BELLMAN_BUDGE = 6
