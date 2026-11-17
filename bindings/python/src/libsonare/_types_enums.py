"""Enumerations on the public libsonare type surface.

Most subclass :class:`enum.IntEnum` because the values cross the C ABI as
plain integers, so either the member or its number is accepted. Key mode, key
profile and section type are string tokens (:class:`typing.Literal`) whose
position in the matching ``_*`` tuple is the C ordinal.
"""

from __future__ import annotations

from enum import IntEnum
from typing import Literal, get_args


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

    In Balance mode centre stays unity and the law changes only the
    far-channel taper; in stereo-pan and dual-pan modes the law's centre
    attenuation is audible (-3.01, -4.52, -6.02 and 0 dB for the four laws in
    stereo-pan). A mono source is unity at centre under every law: the
    standalone mixer always feeds two planes and engine lanes are unity at
    centre. Only a C++ ``ChannelStrip`` built without ``mono_pan_centre_unity``
    makes the law change a mono strip's centre gain.
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


class SidechainSourceKind(IntEnum):
    """Where a bus or master insert's sidechain key is taken from.

    Mirrors ``SonareSidechainSourceKind``: a track's post-strip output, or a
    bus's output before its gain.
    """

    TRACK = 0
    BUS = 1


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


KeyMode = Literal["major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"]
"""Musical mode of a detected or requested key."""

KeyProfile = Literal[
    "krumhanslSchmuckler",
    "temperley",
    "shaath",
    "faraldoEdmt",
    "faraldoEdma",
    "faraldoEdmm",
    "bellmanBudge",
]
"""Key-profile family used by profile-correlation key detection."""

SectionType = Literal[
    "intro",
    "verse",
    "preChorus",
    "chorus",
    "bridge",
    "instrumental",
    "outro",
    "unknown",
]
"""Song-structure section type; ``"unknown"`` means no labelling rule matched."""

GenreHint = Literal["auto", "edm", "electronic", "dance", "pop", "classical", "jazz"]
"""Genre hint that picks a key profile when none is pinned."""

ChordChromaMethod = Literal["stft", "nnls", "cqt"]
"""Chroma front end used by chord detection."""

NormType = Literal["max", "l1", "l2", "power"]
"""Vector normalisation: maximum magnitude, L1, L2 or power."""

TempogramMode = Literal["autocorrelation", "cosine"]
"""Autocorrelation or cosine-similarity tempogram."""

PitchTargetMode = Literal["midi", "scale"]
"""Pitch-correction target: a fixed MIDI note or the nearest note of a scale."""

WindowType = Literal["hann", "hamming", "blackman", "rectangular"]
"""Analysis window shape."""

SpectralEditMode = Literal["gain", "attenuate", "mute", "heal"]
"""What a :class:`SpectralRegionOp` does to its rectangle."""

# Ordinal-ordered tokens: index == the C ordinal of each type.
_KEY_MODES: tuple[KeyMode, ...] = get_args(KeyMode)
_KEY_PROFILES: tuple[KeyProfile, ...] = get_args(KeyProfile)
_SECTION_TYPES: tuple[SectionType, ...] = get_args(SectionType)
_CHORD_CHROMA_METHODS: tuple[ChordChromaMethod, ...] = get_args(ChordChromaMethod)
_NORM_TYPES: tuple[NormType, ...] = get_args(NormType)
_TEMPOGRAM_MODES: tuple[TempogramMode, ...] = get_args(TempogramMode)
_PITCH_TARGET_MODES: tuple[PitchTargetMode, ...] = get_args(PitchTargetMode)
_WINDOW_TYPES: tuple[WindowType, ...] = get_args(WindowType)
_SPECTRAL_EDIT_MODES: tuple[SpectralEditMode, ...] = get_args(SpectralEditMode)
# Passed to the core by name, so the order carries no ordinal.
_GENRE_HINTS: tuple[GenreHint, ...] = get_args(GenreHint)
