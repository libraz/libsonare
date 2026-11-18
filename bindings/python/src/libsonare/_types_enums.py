"""Enumerations on the public libsonare type surface.

:class:`PitchClass` is an :class:`enum.IntEnum` because a pitch class is a
number in its own right. Every closed set is a string token (:class:`typing.Literal`) whose
position in the matching ``_*`` tuple is the C ordinal, unless its tuple says
otherwise.
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


AutomationCurve = Literal["linear", "exponential", "hold", "sCurve"]
"""Interpolation curve for scheduled mixer and engine automation events."""
_AUTOMATION_CURVES: tuple[str, ...] = get_args(AutomationCurve)

PanLaw = Literal["const3Db", "const4.5Db", "const6Db", "linear0Db"]
"""Pan law for a mixer strip.

In Balance mode centre stays unity and the law changes only the far-channel
taper; in stereo-pan and dual-pan modes the law's centre attenuation is audible
(-3.01, -4.52, -6.02 and 0 dB for the four laws in stereo-pan). A mono source
is unity at centre under every law.
"""
_PAN_LAWS: tuple[str, ...] = get_args(PanLaw)


ChannelLayout = Literal["mono", "stereo", "5.1", "7.1"]
"""Speaker bed layout for a bus or source.

Plane order is WAVE_FORMAT_EXTENSIBLE: ``"5.1"`` = L R C LFE Ls Rs, ``"7.1"`` =
L R C LFE Ls Rs Lss Rss.
"""
_CHANNEL_LAYOUTS: tuple[str, ...] = get_args(ChannelLayout)

MeterTap = Literal["preFader", "postFader"]
"""Tap point at which a strip meter snapshot is read."""
_METER_TAPS: tuple[str, ...] = get_args(MeterTap)

MixBusRole = Literal["master", "aux", "submix"]
"""Role of a mixer bus."""
_MIX_BUS_ROLES: tuple[str, ...] = get_args(MixBusRole)

SendTiming = Literal["preFader", "postFader"]
"""Pre/post-fader timing of a mixer strip send."""
# C ordinal order: post-fader is 0 so a zero-initialized send is post-fader.
_SEND_TIMINGS: tuple[str, ...] = ("postFader", "preFader")

SidechainSourceKind = Literal["track", "bus"]
"""Where a bus or master insert's sidechain key is taken from: a track's
post-strip output, or a bus's output before its gain."""
_SIDECHAIN_SOURCE_KINDS: tuple[str, ...] = get_args(SidechainSourceKind)

PanMode = Literal["balance", "stereoPan", "dualPan"]
"""How a strip pans a stereo pair: keep the image, fold to mono, or pan each side."""
_PAN_MODES: tuple[str, ...] = get_args(PanMode)


EngineTelemetryType = Literal["processBlock", "error"]
"""Realtime engine telemetry record type."""
_ENGINE_TELEMETRY_TYPES: tuple[str, ...] = get_args(EngineTelemetryType)

EngineTelemetryError = Literal[
    "none",
    "commandQueueOverflow",
    "pendingCommandOverflow",
    "boundaryOverflow",
    "telemetryOverflow",
    "captureOverflow",
    "maxBlockExceeded",
    "unknownTarget",
    "nonRealtimeSafeParameter",
    "notPrepared",
    "nonQueueableCommand",
    "automationBindTargetOverflow",
    "staleAutomationLanes",
    "smoothedParameterCapacity",
    "commandBacklogDeferred",
    "clipPageUnderrun",
    "insertAutomationOverflow",
    "midiClockOverflow",
    "metronomeOverflow",
    "invalidCommand",
    "maxChannelsExceeded",
    "parameterBaseOverflow",
]
"""Recoverable realtime engine error code of a telemetry record, in ordinal order.

``"invalidCommand"`` is emitted only by the browser worklet; it is listed so a
record moved between surfaces keeps its type.
"""
_ENGINE_TELEMETRY_ERRORS: tuple[str, ...] = get_args(EngineTelemetryError)


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

EqPhaseMode = Literal["zero", "natural", "linear"]
"""Streaming equalizer phase mode: zero latency, natural phase or linear phase."""

EqStereoPlacement = Literal["stereo", "left", "right", "mid", "side"]
"""Signal path an equalizer band sits on."""

TrimSilenceMode = Literal["peak", "lufsGated"]
"""Silence trimming detector: sample peak or gated short-term loudness."""

DecrackleMode = Literal["median", "waveletShrinkage"]
"""Crackle suppressor: median replacement or wavelet shrinkage."""

DenoiseClassicalMode = Literal["logMmse", "mmseStsa", "spectralSubtraction"]
"""Classical denoiser gain rule."""

DenoiseClassicalNoiseEstimator = Literal["quantile", "mcra", "imcra", "spp"]
"""Noise-floor tracker of the classical denoiser."""

DehumMode = Literal["subtract", "notch"]
"""Hum remover: subtract the tracked harmonic series or notch it out."""

CompressorDetector = Literal["peak", "rms", "logRms"]
"""Level detector of the compressor."""

VoicePresetId = Literal[
    "neutralMonitor",
    "brightIdol",
    "softWhisper",
    "deepNarrator",
    "robotMascot",
    "darkVillain",
    "hardTune",
    "robotMonotone",
    "choirHarmony",
    "vocoderStrings",
    "megaphone",
    "radioVoice",
]
"""Built-in realtime voice changer preset."""

ProjectWarpMode = Literal["off", "repitch", "tempoSync", "timeStretch"]
"""How a clip follows the tempo: not at all, by resampling, synced, or time-stretched."""

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
# The C ordinal is the index plus one; 0 is not a phase mode.
_EQ_PHASE_MODES: tuple[EqPhaseMode, ...] = get_args(EqPhaseMode)
_EQ_STEREO_PLACEMENTS: tuple[EqStereoPlacement, ...] = get_args(EqStereoPlacement)
_TRIM_SILENCE_MODES: tuple[TrimSilenceMode, ...] = get_args(TrimSilenceMode)
_DECRACKLE_MODES: tuple[DecrackleMode, ...] = get_args(DecrackleMode)
_DENOISE_CLASSICAL_MODES: tuple[DenoiseClassicalMode, ...] = get_args(DenoiseClassicalMode)
_DENOISE_CLASSICAL_NOISE_ESTIMATORS: tuple[DenoiseClassicalNoiseEstimator, ...] = get_args(
    DenoiseClassicalNoiseEstimator
)
_DEHUM_MODES: tuple[DehumMode, ...] = get_args(DehumMode)
_COMPRESSOR_DETECTORS: tuple[CompressorDetector, ...] = get_args(CompressorDetector)
_WARP_MODES: tuple[ProjectWarpMode, ...] = get_args(ProjectWarpMode)
_VOICE_PRESET_IDS: tuple[VoicePresetId, ...] = get_args(VoicePresetId)
# Passed to the core by name, so the order carries no ordinal.
_GENRE_HINTS: tuple[GenreHint, ...] = get_args(GenreHint)
