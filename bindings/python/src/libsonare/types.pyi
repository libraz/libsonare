from __future__ import annotations

import builtins
from collections.abc import Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Literal, NamedTuple, NotRequired, TypeAlias, TypedDict

import numpy as np
from numpy.typing import NDArray

from ._project_model import ProjectLoopMode, ProjectSourceKind, ProjectTrackKind

FloatSamples: TypeAlias = Sequence[float] | list[float] | np.ndarray[Any, Any]
# Planar audio: one buffer per channel, or a 2-D (channels, frames) array.
PlanarChannels: TypeAlias = Sequence[FloatSamples] | np.ndarray[Any, Any]

MasteringProcessorKind = Literal["realtime", "offline", "pair"]
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
MasteringPresetKind = Literal["mastering", "restoration"]
MasteringInsertParamUnit = Literal[
    "dB",
    "dBFS",
    "dBTP",
    "LUFS",
    "Hz",
    "ms",
    "s",
    "samples",
    "m",
    "cm",
    "mm",
    "deg",
    "percent",
    "degC",
    "V",
    "inPerSec",
    "dBPerOct",
    "semitones",
    "cents",
    "ratio",
    "bits",
    "count",
    "N",
    "midiNote",
    "none",
]
MasteringInsertParamScale = Literal["linear", "log"]
MasteringInsertParamRelation = Literal["lt", "le", "gt", "ge"]
MasteringProcessorCategory = Literal[
    "dynamics",
    "effects",
    "eq",
    "final",
    "maximizer",
    "multiband",
    "other",
    "reference",
    "repair",
    "saturation",
    "spectral",
    "stereo",
    "utility",
    "voice",
]

@dataclass(frozen=True)
class CapabilitiesAbi:
    """ABI versions reported by :func:`libsonare.capabilities`."""

    project: int
    """Project document ABI version."""
    engine: int
    """Realtime engine ABI version."""

@dataclass(frozen=True)
class CapabilitiesFeatures:
    """Build options reported by :func:`libsonare.capabilities`.

    Every compile-time feature option has a field, ``False`` when the option
    was off. An entry point of a feature that was not built stays exported
    and raises ``NotSupported``, so probing for a symbol tells a host nothing;
    these fields are the probe.
    """

    mastering: bool
    """Mastering chain, processors and presets."""
    mixing: bool
    """Mixer, scenes and buses."""
    mixing_assistant: bool
    """Mixing assistant, which can be dropped apart from ``mixing``."""
    graph: bool
    """Processing graph engine."""
    fx: bool
    """Creative insert effects."""
    ffmpeg: bool
    """FFmpeg-backed decoders."""
    instrument_param_automation: bool
    """Automation of instrument parameters."""
    arrangement: bool
    """Arrangement editing."""
    project: bool
    """Project documents."""
    acoustic_sim: bool
    """Room acoustics simulation."""
    playback: bool
    """Device playback."""
    pitch_editor: bool
    """Pitch editor."""
    vocal_edit: bool
    """Vocal editing."""
    voice_changer: bool
    """Realtime voice changer."""
    assist: bool
    """Assist modules."""

@dataclass(frozen=True)
class CapabilitiesDecode:
    """Built-in and FFmpeg-backed decoder lists for the loaded library."""

    builtin: tuple[str, ...]
    """File extensions the built-in decoders read."""
    ffmpeg: tuple[str, ...]
    """File extensions read through FFmpeg; empty without FFmpeg."""

@dataclass(frozen=True)
class Capabilities:
    """Build and runtime descriptor returned by :func:`libsonare.capabilities`."""

    version: str
    """Library version string."""
    abi: CapabilitiesAbi
    """ABI versions."""
    platform: str
    """Target platform, such as ``"darwin-arm64"``."""
    features: CapabilitiesFeatures
    """Compiled-in build options."""
    decode: CapabilitiesDecode
    """Decodable file extensions."""
    simd: str
    """SIMD instruction set the build uses."""
    hardware_concurrency: int
    """Hardware threads the host reports."""

@dataclass(frozen=True)
class MasteringInsertParamChoice:
    """One accepted value of a closed-set insert parameter.

    ``name`` is a lowerCamel display label (the enumerator's name, or an
    integer's decimal text for a non-contiguous whole-number parameter);
    ``value`` is the number construction accepts.
    """

    name: str
    value: int

@dataclass(frozen=True)
class MasteringInsertSlot:
    """A group of an insert's keys that exists only under a condition.

    An EQ band, a multiband crossover band, or a dynamic sub-band inside one.
    ``name`` is the key prefix without the trailing dot (``"midBand3"``,
    ``"band1.dyn2"``); ``parent`` names the enclosing slot, always listed
    earlier, or is ``None``. ``activation`` is ``"anyKey"`` when the slot
    exists once any one of its keys is supplied and ``"always"`` when it exists
    without them; ``min_crossover_cutoffs`` is how many ``cutoff<i>Hz`` keys
    the crossover in effect needs for the slot to exist (0 for none, and with
    no cutoff key supplied the cutoffs that publish a default are in effect).
    """

    name: str
    parent: str | None
    activation: Literal["anyKey", "always"]
    min_crossover_cutoffs: int

@dataclass(frozen=True)
class MasteringInsertParamDependency:
    """A sibling key whose value bounds this parameter's.

    ``relation`` reads from this parameter's side against ``factor`` times
    the sibling's value: ``"le"`` with ``factor`` 0.5 means this parameter's
    value must be at most half the sibling's.
    """

    key: str
    relation: MasteringInsertParamRelation
    factor: float

@dataclass(frozen=True)
class SynthParamCondition:
    """One condition another patch field must meet, as one of the conditions
    of an alternative in :attr:`SynthParamInfo.active_when`.

    ``scope`` is ``"engine"`` for a key of the same
    :func:`synth_engine_param_info` table and ``"patch"`` for a ``SynthPatch``
    field. ``value`` is a number, a bool against a switch, or a vocabulary name
    against a wrapper enum (``body`` ``"violin"``).
    """

    key: str
    scope: Literal["engine", "patch"]
    relation: Literal["eq", "ne", "gt", "ge", "lt", "le"]
    value: float | bool | str

@dataclass(frozen=True)
class SynthParamInfo:
    """One synth parameter as :func:`synth_engine_param_info` and
    :func:`synth_patch_param_info` describe it.

    ``name`` is the key ``SynthPatch.engine_params`` (or the patch field) is
    spelled with; ``"boolean"`` is a switch given as 0 or 1. ``integer`` is
    true for a field taking whole numbers only. ``min`` / ``max`` are ``None``
    for a field with no closed range. ``default`` is the engine's base
    preset's value for an engine field and the "keep base" default for a
    patch field, a bool for a switch. ``active_when`` is not ``None`` only for
    a field that reaches the voice under conditions on other patch fields: a
    tuple of alternatives, each a tuple of conditions, and the field reaches
    the voice when every condition of at least one alternative holds, so a
    field reached through several layers lists one alternative per layer.
    What a field further needs from a performance (velocity, a controller,
    overlapping notes), or a value past which it saturates, is not stated::

        live = field.active_when is None or any(
            all(holds(c) for c in alternative) for alternative in field.active_when
        )
    """

    name: str
    type: Literal["number", "boolean"]
    default: float | bool
    unit: MasteringInsertParamUnit
    integer: bool = False
    min: float | None = None
    max: float | None = None
    active_when: tuple[tuple[SynthParamCondition, ...], ...] | None = None

@dataclass(frozen=True)
class MasteringInsertParamInfo:
    """Metadata for one key an insert processor's construction or automation reads.

    Entries come in two runs: first the processor's realtime automation
    targets in id order, then -- sorted by name -- the keys construction reads
    that are not automation targets, with ``id`` ``None`` and ``rt_safe``
    false.

    ``min_exclusive`` / ``max_exclusive`` say whether the published bound
    itself is rejected (false when the bound is ``None``). ``max_relative_to``
    is ``"nyquist"`` when the ceiling follows the processing rate: the
    effective ceiling is then the lower of ``max`` and the host's Nyquist
    frequency, both exclusive.

    ``unit`` is declared for every numeric parameter (``"none"`` for a
    fraction, a selector index or a seed) and ``None`` otherwise. ``scale`` is
    the axis a control draws the value on. ``ui_min`` / ``ui_max``, when not
    ``None``, are a display range inside ``[min, max]``; ``None`` means the
    accepted range is also the display range. ``depends_on`` lists the
    siblings whose values bound this one; a bound that only restates that
    dependency at the sibling's default is ``None``, so ``min`` / ``max`` are
    limits of the key's own. ``coupled_with`` names the other siblings that
    move this key's accepted range in a way ``depends_on`` cannot state;
    ``min`` / ``max`` are measured with them at their defaults.
    """

    name: str
    id: int | None
    rt_safe: bool
    type: Literal["boolean", "number", "enum", "string", "array"]
    min: float | None
    max: float | None
    min_exclusive: bool
    max_exclusive: bool
    max_relative_to: Literal["nyquist"] | None
    default: float | bool | None
    unit: MasteringInsertParamUnit | None
    ui_min: float | None
    ui_max: float | None
    scale: MasteringInsertParamScale
    depends_on: tuple[MasteringInsertParamDependency, ...]
    coupled_with: tuple[str, ...]
    choices: tuple[MasteringInsertParamChoice, ...] | None
    slot: str | None

@dataclass(frozen=True)
class MasteringInsertTiming:
    """Latency and tail of one insert built from given params, at a given rate.

    Returned by :func:`mastering_insert_timing`.
    """

    latency_samples: int
    """Processing latency in samples."""
    tail_samples: int
    """Audible decay after the input stops, in samples."""

@dataclass(frozen=True)
class MasteringProcessorCatalogEntry:
    """Capabilities exposed by :func:`mastering_processor_catalog`."""

    id: str
    kind: MasteringProcessorKind
    realtime_insertable: bool
    stereo_only: bool
    latency_samples: int
    tail_samples: int
    realtime_cost: Literal["low", "moderate", "high"] | None
    channel_policy: MasteringChannelPolicy
    category: MasteringProcessorCategory
    causal: bool
    params: tuple[MasteringInsertParamInfo, ...]
    slots: tuple[MasteringInsertSlot, ...]

@dataclass(frozen=True)
class MasteringAmpPresetCatalogEntry:
    """One built-in amp-sim rig, as returned by :func:`mastering_amp_preset_catalog`.

    ``params`` holds the rig's resolved starting values under the insert's
    camelCase parameter keys (``drive``, ``bassDb``, ``ampModel``, ...), with
    enum-valued keys as integer ordinals. Read-only metadata for an editor
    positioning inherited controls -- the DSP is still driven by the rig
    index plus sparse overrides.
    """

    index: int
    name: str
    params: dict[str, float | int | bool]

@dataclass(frozen=True)
class CapabilityCatalogPresets:
    """Built-in preset identifiers grouped by public feature family."""

    mastering: tuple[str, ...]
    """Mastering chain presets."""
    synth: tuple[str, ...]
    """NativeSynth presets."""
    mixing_scene: tuple[str, ...]
    """Mixing scene presets."""
    voice_changer: tuple[str, ...]
    """Voice changer presets."""
    playback_room: tuple[str, ...]
    """Playback room presets."""

@dataclass(frozen=True)
class MasteringPresetCatalogEntry:
    """One mastering preset's kind and loudness targets.

    ``target_lufs``, ``true_peak_ceiling_dbtp`` and
    ``max_limiter_gain_reduction_db`` are ``None`` for a ``"restoration"``
    preset, which carries no loudness stage.
    """

    name: str
    kind: MasteringPresetKind
    target_lufs: float | None
    true_peak_ceiling_dbtp: float | None
    max_limiter_gain_reduction_db: float | None

@dataclass(frozen=True)
class CapabilityCatalogChainStage:
    """One flat chain stage; ``<prefix>.<leaf>`` keys are ``processor``'s parameters.

    ``processor`` is ``None`` when every key of the stage is chain-only.
    """

    prefix: str
    """Key prefix of the stage."""
    processor: str | None
    """Processor id describing the stage's keys."""
    enabled_key: str
    """Chain key that switches the stage on."""

@dataclass(frozen=True)
class CapabilityCatalogChainList:
    """An indexed chain list.

    ``<key>.<i>`` is the chain parameter ``key`` when ``processor`` is ``None``;
    otherwise ``<key>.<i>.<field>`` is that processor's ``field``.
    """

    key: str
    """Chain key of the list."""
    processor: str | None
    """Processor id of each item, or ``None`` for a list of numbers."""
    min_items: int
    """Fewest items the list accepts."""
    max_items: int
    """Most items the list accepts."""

@dataclass(frozen=True)
class CapabilityCatalogChain:
    """How each flat mastering chain key is described.

    ``params`` holds the keys no processor describes, with bounds measured
    through the chain's own validator.
    """

    stages: tuple[CapabilityCatalogChainStage, ...]
    """Flat chain stages."""
    lists: tuple[CapabilityCatalogChainList, ...]
    """Indexed chain lists."""
    params: tuple[MasteringInsertParamInfo, ...]
    """Chain-only keys."""

@dataclass(frozen=True)
class CapabilityCatalog:
    """Machine-readable catalog returned by :func:`capability_catalog`."""

    version: str
    """Library version string."""
    abi: CapabilitiesAbi
    """ABI versions."""
    processors: tuple[MasteringProcessorCatalogEntry, ...]
    """Every mastering processor."""
    chain: CapabilityCatalogChain
    """Flat mastering chain keys."""
    presets: CapabilityCatalogPresets
    """Built-in preset identifiers."""
    mastering_presets: tuple[MasteringPresetCatalogEntry, ...]
    """Mastering presets with their loudness targets."""

class PitchClass(IntEnum):
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

KeyMode: TypeAlias = Literal[
    "major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"
]

GenreHint: TypeAlias = Literal["auto", "edm", "electronic", "dance", "pop", "classical", "jazz"]

ChordChromaMethod: TypeAlias = Literal["stft", "nnls", "cqt"]

NormType: TypeAlias = Literal["max", "l1", "l2", "power"]

TempogramMode: TypeAlias = Literal["autocorrelation", "cosine"]

PitchTargetMode: TypeAlias = Literal["midi", "scale"]

WindowType: TypeAlias = Literal["hann", "hamming", "blackman", "rectangular"]
NmfInit: TypeAlias = Literal["random", "nndsvd"]

SpectralEditMode: TypeAlias = Literal["gain", "attenuate", "mute", "heal"]

EqPhaseMode: TypeAlias = Literal["zero", "natural", "linear"]

EqStereoPlacement: TypeAlias = Literal["stereo", "left", "right", "mid", "side"]

TrimSilenceMode: TypeAlias = Literal["peak", "lufsGated"]

DecrackleMode: TypeAlias = Literal["median", "waveletShrinkage"]

DenoiseClassicalMode: TypeAlias = Literal["logMmse", "mmseStsa", "spectralSubtraction"]

DenoiseClassicalNoiseEstimator: TypeAlias = Literal["quantile", "mcra", "imcra", "spp"]

DehumMode: TypeAlias = Literal["subtract", "notch"]

CompressorDetector: TypeAlias = Literal["peak", "rms", "logRms"]

ProjectWarpMode: TypeAlias = Literal["off", "repitch", "tempoSync", "timeStretch"]

VoicePresetId: TypeAlias = Literal[
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

KeyProfile: TypeAlias = Literal[
    "krumhanslSchmuckler",
    "temperley",
    "shaath",
    "faraldoEdmt",
    "faraldoEdma",
    "faraldoEdmm",
    "bellmanBudge",
]

SectionType: TypeAlias = Literal[
    "intro",
    "verse",
    "preChorus",
    "chorus",
    "bridge",
    "instrumental",
    "outro",
    "unknown",
]

AutomationCurve = Literal["linear", "exponential", "hold", "sCurve"]
PanLaw = Literal["const3Db", "const4.5Db", "const6Db", "linear0Db"]

PanMode = Literal["balance", "stereoPan", "dualPan"]
ChannelLayout = Literal["mono", "stereo", "5.1", "7.1"]
MeterTap = Literal["preFader", "postFader"]
MixBusRole = Literal["master", "aux", "submix"]
SendTiming = Literal["preFader", "postFader"]
SidechainSourceKind = Literal["track", "bus"]

EngineTelemetryType = Literal["processBlock", "error"]

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

class Key:
    """Detected musical key.

    ``confidence`` is the share of the model's belief that this key is the
    answer, in ``[0, 1)``: a softmax over the profile correlations of every
    candidate that was scored, so it falls as the runner-up closes in and two
    keys that split the evidence -- a relative major and minor, typically --
    each report about half.

    It is the model's own belief, **not** a measured accuracy. A confident wrong
    answer is entirely possible, so a pipeline that branches on it must choose
    its own threshold against its own material.
    """

    root: PitchClass
    mode: KeyMode
    confidence: float
    def __init__(self, root: PitchClass, mode: KeyMode, confidence: float) -> None: ...
    @property
    def name(self) -> str: ...
    @property
    def short_name(self) -> str: ...

class KeyDetection(Key):
    tuning_cents: float
    def __init__(
        self, root: PitchClass, mode: KeyMode, confidence: float, tuning_cents: float = ...
    ) -> None: ...

class AutoTuneResult:
    samples: NDArray[np.float32]
    key: Key
    def __init__(self, samples: NDArray[np.float32], key: Key) -> None: ...

class KeyCandidate:
    key: Key
    correlation: float
    def __init__(self, key: Key, correlation: float) -> None: ...

class TimeSignature:
    numerator: int
    denominator: int
    confidence: float
    def __init__(self, numerator: int, denominator: int, confidence: float) -> None: ...

class Beat:
    """Beat event. ``strength`` is a single raw frame of the onset envelope
    sampled at the beat's own frame — not normalized, scaled by the material,
    and sensitive to beat-position jitter, so it is not a salience comparable
    across beats. Score accents with
    ``AnalysisResult.beat_observations.onset_strength``, the windowed value the
    library's own downbeat pass uses."""

    time_sec: float
    strength: float
    def __init__(self, time_sec: float, strength: float) -> None: ...

class AnalysisDynamics:
    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_lu: float
    is_compressed: bool
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_lu: float,
        is_compressed: bool,
    ) -> None: ...

class AnalysisRhythm:
    time_signature: TimeSignature
    syncopation: float
    groove_type: str
    pattern_regularity: float
    tempo_stability: float
    def __init__(
        self,
        time_signature: TimeSignature,
        syncopation: float,
        groove_type: str,
        pattern_regularity: float,
        tempo_stability: float,
    ) -> None: ...

class BeatObservations:
    """Beat-level evidence the downbeat and meter pass scores, one value per
    beat and parallel to ``AnalysisResult.beats``. An empty stream means the
    analysis could not produce it, not that every beat scored zero:
    ``low_frequency_energy`` is empty without audio, and ``chord_change`` is
    empty until chords are analyzed. ``onset_strength`` is the windowed
    aggregate around each beat, distinct from ``Beat.strength`` (a single raw
    unwindowed envelope frame)."""

    onset_strength: NDArray[np.float32]
    low_frequency_energy: NDArray[np.float32]
    chord_change: NDArray[np.float32]
    def __init__(
        self,
        onset_strength: NDArray[np.float32] = ...,
        low_frequency_energy: NDArray[np.float32] = ...,
        chord_change: NDArray[np.float32] = ...,
    ) -> None: ...

class AnalysisResult:
    """Full audio analysis result.

    ``bpm_confidence`` is the winning tempo cluster's vote share within 60-200 BPM,
    searched at 22050 Hz. :func:`analyze_bpm` searches the same range at the input
    rate, so the two can differ on an input at another rate.
    """

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    timbre: TimbreFrame
    dynamics: AnalysisDynamics
    rhythm: AnalysisRhythm
    melody: MelodyResult
    beats: list[Beat]
    downbeats: list[Beat]
    downbeat_phase: int
    bpm_candidates: list[AnalysisBpmCandidate]
    time_signature_candidates: list[TimeSignature]
    chords: list[AnalysisChord]
    sections: list[Section]
    beat_observations: BeatObservations | None
    beat_local_bpm: NDArray[np.float32]
    form: str
    tuning_cents: float
    def __init__(
        self,
        bpm: float,
        bpm_confidence: float,
        key: Key,
        time_signature: TimeSignature,
        timbre: TimbreFrame,
        dynamics: AnalysisDynamics,
        rhythm: AnalysisRhythm,
        melody: MelodyResult,
        beats: list[Beat] = ...,
        downbeats: list[Beat] = ...,
        downbeat_phase: int = 0,
        bpm_candidates: list[AnalysisBpmCandidate] = ...,
        time_signature_candidates: list[TimeSignature] = ...,
        chords: list[AnalysisChord] = ...,
        sections: list[Section] = ...,
        beat_observations: BeatObservations | None = None,
        beat_local_bpm: NDArray[np.float32] = ...,
        form: str = "",
        tuning_cents: float = 0.0,
    ) -> None: ...

class MeterEstimate:
    """Meter estimated over a caller-supplied beat series. ``candidate_scores``
    is parallel to the numerators that were *requested*, in request order,
    while ``candidates`` is ordered by descending support — the two do not
    index alike. A score is standardized and signed: zero is the level a
    numerator reaches on beats carrying no meter, so only the ordering and the
    gaps between entries carry meaning, and a score grows with the square root
    of how many beats were scored, so scores from spans of different lengths
    are not comparable without normalizing for length. ``grouping`` is how the
    bar divides, in beats per accent group — ``[3, 2, 2]`` for a 7/8 notated
    3+2+2 — and always sums to the reported numerator; a single entry means no
    internal division was resolved. ``searched`` is False when the series was
    too short to score any candidate, in which case every other field is the
    fixed fallback rather than a measurement, the confidence 0 included. Two
    different quantities share the name ``confidence``:
    ``time_signature.confidence`` is the margin over the runner-up, while a
    ``candidates`` entry's is that candidate's share of the summed support."""

    time_signature: TimeSignature
    downbeat_phase: int
    searched: bool
    grouping: NDArray[np.int32]
    candidate_scores: NDArray[np.float32]
    candidates: list[TimeSignature]
    def __init__(
        self,
        time_signature: TimeSignature,
        downbeat_phase: int,
        searched: bool,
        grouping: NDArray[np.int32] = ...,
        candidate_scores: NDArray[np.float32] = ...,
        candidates: list[TimeSignature] = ...,
    ) -> None: ...

class BpmCandidate:
    bpm: float
    confidence: float
    def __init__(self, bpm: float, confidence: float) -> None: ...

BpmCandidateRelation: TypeAlias = Literal["primary", "half", "double", "other"]

class AnalysisBpmCandidate(BpmCandidate):
    relation: BpmCandidateRelation
    def __init__(self, bpm: float, confidence: float, relation: BpmCandidateRelation) -> None: ...

class BpmAnalysisResult:
    """Detailed BPM analysis result.

    ``confidence`` is the winning tempo cluster's vote share within
    ``bpm_min``-``bpm_max`` (60-200 by default), searched at the input rate;
    :func:`analyze` searches at 22050 Hz, so its ``bpm_confidence`` can differ.
    ``candidates`` opens with the chosen tempo
    (``candidates[0].bpm == bpm``); the other histogram bins follow by
    descending vote share.
    """

    bpm: float
    confidence: float
    candidates: list[BpmCandidate]
    autocorrelation: NDArray[np.float32]
    tempogram: NDArray[np.float32]
    def __init__(
        self,
        bpm: float,
        confidence: float,
        candidates: list[BpmCandidate],
        autocorrelation: NDArray[np.float32],
        tempogram: NDArray[np.float32],
    ) -> None: ...

class AcousticResult:
    """Room acoustic parameters from a blind recording or a measured impulse
    response (``is_blind`` distinguishes the two). Only ``rt60`` (and
    ``rt60_bands``) is estimated in both modes.

    ``c50``/``c80``/``d50`` and ``edt`` require a known direct-sound arrival
    time, which only a measured impulse response provides, so they are ``None``
    when ``is_blind`` is true -- ``edt`` measures the 0 to -10 dB decay and the
    blind estimator only fits the late decay ``rt60`` comes from. ``edt_bands``,
    ``c50_bands`` and ``c80_bands`` are ``None`` then too."""

    rt60: float
    edt: float | None
    c50: float | None
    c80: float | None
    d50: float | None
    rt60_bands: NDArray[np.float32]
    edt_bands: NDArray[np.float32] | None
    c50_bands: NDArray[np.float32] | None
    c80_bands: NDArray[np.float32] | None
    confidence: float
    is_blind: bool
    def __init__(
        self,
        rt60: float,
        edt: float | None,
        c50: float | None,
        c80: float | None,
        d50: float | None,
        rt60_bands: NDArray[np.float32],
        edt_bands: NDArray[np.float32] | None,
        c50_bands: NDArray[np.float32] | None,
        c80_bands: NDArray[np.float32] | None,
        confidence: float,
        is_blind: bool,
    ) -> None: ...

class RirDiagnostic:
    code: str
    message: str
    severity: str
    def __init__(
        self,
        code: str,
        message: str,
        severity: str,
    ) -> None: ...

class RoomMorphResult:
    audio: NDArray[np.float32]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        audio: NDArray[np.float32],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...

class RoomMorphStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...

class RirResult:
    rir: NDArray[np.float32]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        rir: NDArray[np.float32],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...

class RoomEstimate:
    volume: float
    length_m: float
    width_m: float
    height_m: float
    drr_db: float
    confidence: float
    band_absorption: NDArray[np.float32]
    rt60_bands: NDArray[np.float32]
    def __init__(
        self,
        volume: float,
        length_m: float,
        width_m: float,
        height_m: float,
        drr_db: float,
        confidence: float,
        band_absorption: NDArray[np.float32],
        rt60_bands: NDArray[np.float32],
    ) -> None: ...

MaterialPresetName: TypeAlias = Literal["none", "concrete", "wood", "curtain", "carpet", "glass"]
AcousticModeName: TypeAlias = Literal["auto", "blind", "impulseResponse"]

class RoomGeometry(TypedDict):
    length_m: float
    width_m: float
    height_m: float
    source: tuple[float, float, float]
    listener: tuple[float, float, float]
    absorption: NotRequired[float]
    band_absorption: NotRequired[NDArray[np.float32]]

class DereverbClassicalConfig(TypedDict):
    threshold: float
    attenuation: float
    n_fft: int
    hop_length: int
    t60_sec: float
    late_delay_ms: float
    over_subtraction: float
    spectral_floor: float
    wpe_enabled: bool
    wpe_iterations: int
    wpe_taps: int
    wpe_strength: float

class LufsSeriesResult:
    momentary_lufs: NDArray[np.float32]
    short_term_lufs: NDArray[np.float32]
    def __init__(
        self,
        momentary_lufs: NDArray[np.float32],
        short_term_lufs: NDArray[np.float32],
    ) -> None: ...

class LufsResult:
    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    loudness_range_lu: float
    def __init__(
        self,
        integrated_lufs: float,
        momentary_lufs: float,
        short_term_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        loudness_range_lu: float,
    ) -> None: ...

class StereoBlock(NamedTuple):
    """One processed stereo block; unpacks as ``left, right``."""

    left: NDArray[np.float32]
    right: NDArray[np.float32]

class EqSpectrumSnapshot:
    """Realtime equalizer snapshot.

    ``pre_left``/``pre_right`` and ``post_left``/``post_right`` are the pre- and
    post-EQ waveform streams (uniformly decimated time-domain samples), so they
    are a scope feed rather than a spectral estimate. ``profile_db`` is the
    frequency-domain view: the post-EQ signal is Hann-windowed, transformed and
    its bin powers summed into 16 geometrically spaced bands covering 20 Hz to
    20 kHz, in amplitude decibels relative to full scale, rising immediately and
    falling smoothly.
    """

    pre_left: NDArray[np.float32]
    pre_right: NDArray[np.float32]
    post_left: NDArray[np.float32]
    post_right: NDArray[np.float32]
    band_gain_db: NDArray[np.float32]
    profile_db: NDArray[np.float32]
    last_auto_gain_db: float
    seq: int
    def __init__(
        self,
        pre_left: NDArray[np.float32],
        pre_right: NDArray[np.float32],
        post_left: NDArray[np.float32],
        post_right: NDArray[np.float32],
        band_gain_db: NDArray[np.float32],
        profile_db: NDArray[np.float32],
        last_auto_gain_db: float,
        seq: int,
    ) -> None: ...

class RhythmResult:
    bpm: float
    time_signature: TimeSignature
    groove_type: str
    syncopation: float
    pattern_regularity: float
    tempo_stability: float
    beat_intervals: NDArray[np.float32]
    def __init__(
        self,
        bpm: float,
        time_signature: TimeSignature,
        groove_type: str,
        syncopation: float,
        pattern_regularity: float,
        tempo_stability: float,
        beat_intervals: NDArray[np.float32],
    ) -> None: ...

class DynamicsResult:
    dynamic_range_db: float
    peak_db: float
    rms_db: float
    crest_factor: float
    loudness_range_lu: float
    is_compressed: bool
    loudness_times_sec: NDArray[np.float32]
    loudness_rms_db: NDArray[np.float32]
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_lu: float,
        is_compressed: bool,
        loudness_times_sec: NDArray[np.float32],
        loudness_rms_db: NDArray[np.float32],
    ) -> None: ...

@dataclass(frozen=True)
class ClickDetection:
    count: int
    rejected: int
    longest_run_samples: int
    per_second: float
    def __init__(
        self, count: int, rejected: int, longest_run_samples: int, per_second: float
    ) -> None: ...

@dataclass(frozen=True)
class ClipDetection:
    clipped_samples: int
    clipped_ratio: float
    run_count: int
    longest_run_samples: int
    flat_run_count: int
    longest_flat_run_samples: int
    flat_sample_count: int
    flat_level: float | None  # largest run level once the two highest runs are set aside
    def __init__(
        self,
        clipped_samples: int,
        clipped_ratio: float,
        run_count: int,
        longest_run_samples: int,
        flat_run_count: int,
        longest_flat_run_samples: int,
        flat_sample_count: int,
        flat_level: float | None,
    ) -> None: ...

class ClippingRegion:
    start_sample: int
    end_sample: int
    length: int
    peak: float
    def __init__(self, start_sample: int, end_sample: int, length: int, peak: float) -> None: ...

class ClippingReport:
    clipped_samples: int
    clipping_ratio: float
    max_clipped_peak: float
    regions: list[ClippingRegion]
    def __init__(
        self,
        clipped_samples: int,
        clipping_ratio: float,
        max_clipped_peak: float,
        regions: list[ClippingRegion],
    ) -> None: ...

class DynamicRangeReport:
    dynamic_range_db: float
    low_percentile_db: float
    high_percentile_db: float
    window_rms_db: NDArray[np.float32]
    def __init__(
        self,
        dynamic_range_db: float,
        low_percentile_db: float,
        high_percentile_db: float,
        window_rms_db: NDArray[np.float32],
    ) -> None: ...

class VectorscopeReport:
    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    def __init__(self, mid: NDArray[np.float32], side: NDArray[np.float32]) -> None: ...

class PhaseScopeReport:
    mid: NDArray[np.float32]
    side: NDArray[np.float32]
    radius: NDArray[np.float32]
    angle_rad: NDArray[np.float32]
    correlation: float
    average_abs_angle_rad: float
    max_radius: float
    def __init__(
        self,
        mid: NDArray[np.float32],
        side: NDArray[np.float32],
        radius: NDArray[np.float32],
        angle_rad: NDArray[np.float32],
        correlation: float,
        average_abs_angle_rad: float,
        max_radius: float,
    ) -> None: ...

class SpectrumReport:
    frequencies: NDArray[np.float32]
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]
    db: NDArray[np.float32]
    n_fft: int
    sample_rate: int
    def __init__(
        self,
        frequencies: NDArray[np.float32],
        magnitude: NDArray[np.float32],
        power: NDArray[np.float32],
        db: NDArray[np.float32],
        n_fft: int,
        sample_rate: int,
    ) -> None: ...

class WaveformPeaksReport:
    min: NDArray[np.float32]
    max: NDArray[np.float32]
    num_channels: int
    bucket_count: int
    samples_per_bucket: int
    def __init__(
        self,
        min: NDArray[np.float32],
        max: NDArray[np.float32],
        num_channels: int,
        bucket_count: int,
        samples_per_bucket: int,
    ) -> None: ...

class TimbreFrame:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    def __init__(
        self,
        brightness: float,
        warmth: float,
        density: float,
        roughness: float,
        complexity: float,
    ) -> None: ...

class TimbreResult:
    brightness: float
    warmth: float
    density: float
    roughness: float
    complexity: float
    spectral_centroid: NDArray[np.float32]
    spectral_flatness: NDArray[np.float32]
    spectral_rolloff: NDArray[np.float32]
    timbre_over_time: list[TimbreFrame]
    def __init__(
        self,
        brightness: float,
        warmth: float,
        density: float,
        roughness: float,
        complexity: float,
        spectral_centroid: NDArray[np.float32],
        spectral_flatness: NDArray[np.float32],
        spectral_rolloff: NDArray[np.float32],
        timbre_over_time: list[TimbreFrame],
    ) -> None: ...

CHORD_QUALITIES: tuple[str, ...]

class Chord:
    """Detected chord. ``name`` is the chord symbol (``"Am7"``, ``"C/E"``; ``"N.C."``
    for no chord), filled by the analysis functions."""

    root: PitchClass
    quality: str
    start_sec: float
    end_sec: float
    confidence: float
    bass: PitchClass | None
    name: str
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start_sec: float,
        end_sec: float,
        confidence: float,
        bass: PitchClass | None = None,
        name: str = "",
    ) -> None: ...
    @property
    def duration_sec(self) -> float: ...
    @property
    def root_name(self) -> str: ...
    @property
    def bass_name(self) -> str: ...

class AnalysisChord(Chord):
    roman_numeral: str
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start_sec: float,
        end_sec: float,
        confidence: float,
        bass: PitchClass | None = None,
        name: str = "",
        roman_numeral: str = "",
    ) -> None: ...

class ChordAnalysisResult:
    chords: list[Chord]
    tuning_cents: float
    def __init__(self, chords: list[Chord], tuning_cents: float = ...) -> None: ...

ChordFunctionName: TypeAlias = Literal["tonic", "subdominant", "dominant", "chromatic", "none"]

class FunctionalChord(Chord):
    roman_numeral: str
    function: ChordFunctionName
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start_sec: float,
        end_sec: float,
        confidence: float,
        bass: PitchClass | None = None,
        name: str = "",
        roman_numeral: str = "",
        function: ChordFunctionName = "none",
    ) -> None: ...

class StftResult:
    n_fft: int
    hop_length: int
    sample_rate: int
    magnitude: NDArray[np.float32]
    power: NDArray[np.float32]
    def __init__(
        self,
        n_fft: int,
        hop_length: int,
        sample_rate: int,
        magnitude: NDArray[np.float32],
        power: NDArray[np.float32],
    ) -> None: ...

class MelSpectrogramResult:
    sample_rate: int
    hop_length: int
    power: NDArray[np.float32]
    db: NDArray[np.float32]
    n_fft: int
    fmin_hz: float
    fmax_hz: float
    htk: bool
    def __init__(
        self,
        sample_rate: int,
        hop_length: int,
        power: NDArray[np.float32],
        db: NDArray[np.float32],
        n_fft: int,
        fmin_hz: float,
        fmax_hz: float,
        htk: bool,
    ) -> None: ...

class MfccResult:
    coefficients: NDArray[np.float32]
    sample_rate: int
    hop_length: int
    n_fft: int
    n_mels: int
    fmin_hz: float
    fmax_hz: float
    htk: bool
    lifter: float
    def __init__(
        self,
        coefficients: NDArray[np.float32],
        sample_rate: int,
        hop_length: int,
        n_fft: int,
        n_mels: int,
        fmin_hz: float,
        fmax_hz: float,
        htk: bool,
        lifter: float,
    ) -> None: ...

class ChromaResult:
    sample_rate: int
    hop_length: int
    features: NDArray[np.float32]
    mean_energy: NDArray[np.float32]
    def __init__(
        self,
        sample_rate: int,
        hop_length: int,
        features: NDArray[np.float32],
        mean_energy: NDArray[np.float32],
    ) -> None: ...

class PitchResult:
    f0: NDArray[np.float32]
    voiced_prob: NDArray[np.float32]
    voiced_flag: NDArray[np.bool_]
    median_f0: float
    mean_f0: float
    def __init__(
        self,
        f0: NDArray[np.float32],
        voiced_prob: NDArray[np.float32],
        voiced_flag: NDArray[np.bool_],
        median_f0: float,
        mean_f0: float,
    ) -> None: ...

class PiptrackResult:
    pitches: NDArray[np.float32]
    magnitudes: NDArray[np.float32]
    def __init__(self, pitches: NDArray[np.float32], magnitudes: NDArray[np.float32]) -> None: ...

class ReassignedSpectrogramResult:
    magnitude: NDArray[np.float32]
    times: NDArray[np.float32]
    frequencies: NDArray[np.float32]
    def __init__(
        self,
        magnitude: NDArray[np.float32],
        times: NDArray[np.float32],
        frequencies: NDArray[np.float32],
    ) -> None: ...

class NoteSegment:
    frame_start: int
    frame_end: int
    start_sec: float
    end_sec: float
    median_cents: float
    def __init__(
        self,
        frame_start: int,
        frame_end: int,
        start_sec: float,
        end_sec: float,
        median_cents: float,
    ) -> None: ...

class HpssResult:
    harmonic: NDArray[np.float32]
    percussive: NDArray[np.float32]
    length: int
    sample_rate: int
    def __init__(
        self,
        harmonic: NDArray[np.float32],
        percussive: NDArray[np.float32],
        length: int,
        sample_rate: int,
    ) -> None: ...

class DeclickReport:
    detected: ClickDetection
    repaired_runs: int
    repaired_samples: int
    linked_runs: int
    lpc_model_used: bool
    def __init__(
        self,
        detected: ClickDetection,
        repaired_runs: int,
        repaired_samples: int,
        linked_runs: int,
        lpc_model_used: bool,
    ) -> None: ...

class DeclickResult:
    samples: np.ndarray[Any, Any]
    report: DeclickReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DeclickReport) -> None: ...

class DeclickStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    left_report: DeclickReport
    right_report: DeclickReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        left_report: DeclickReport,
        right_report: DeclickReport,
    ) -> None: ...

class DeclipReport:
    detected: ClipDetection
    lpc_reconstructed_runs: int
    interpolated_runs: int
    repaired_samples: int
    linked_runs: int
    def __init__(
        self,
        detected: ClipDetection,
        lpc_reconstructed_runs: int,
        interpolated_runs: int,
        repaired_samples: int,
        linked_runs: int,
    ) -> None: ...

class DeclipResult:
    samples: np.ndarray[Any, Any]
    report: DeclipReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DeclipReport) -> None: ...

class DeclipStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    left_report: DeclipReport
    right_report: DeclipReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        left_report: DeclipReport,
        right_report: DeclipReport,
    ) -> None: ...

@dataclass(frozen=True)
class CrackleDetection:
    sample_count: int
    sample_fraction: float
    per_second: float
    def __init__(self, sample_count: int, sample_fraction: float, per_second: float) -> None: ...

class DecrackleReport:
    detected: CrackleDetection
    replaced_samples: int
    detail_coefficients: int
    shrunk_coefficients: int
    noise_sigma: float
    def __init__(
        self,
        detected: CrackleDetection,
        replaced_samples: int,
        detail_coefficients: int,
        shrunk_coefficients: int,
        noise_sigma: float,
    ) -> None: ...

class DecrackleResult:
    samples: np.ndarray[Any, Any]
    report: DecrackleReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DecrackleReport) -> None: ...

class DecrackleStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    left_report: DecrackleReport
    right_report: DecrackleReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        left_report: DecrackleReport,
        right_report: DecrackleReport,
    ) -> None: ...

@dataclass(frozen=True)
class HumDetection:
    fundamental_hz: float
    # Linear energy ratio (not dB); 1.0 means no peak.
    fundamental_prominence: float
    harmonics: int
    harmonic_dbfs: NDArray[np.float32]
    def __init__(
        self,
        fundamental_hz: float,
        fundamental_prominence: float,
        harmonics: int,
        harmonic_dbfs: NDArray[np.float32],
    ) -> None: ...

class DehumReport:
    detected: HumDetection
    notched_harmonics: int
    applied_fundamental_hz: float
    fundamental_drift_hz: float
    def __init__(
        self,
        detected: HumDetection,
        notched_harmonics: int,
        applied_fundamental_hz: float,
        fundamental_drift_hz: float,
    ) -> None: ...

class DehumResult:
    samples: np.ndarray[Any, Any]
    report: DehumReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DehumReport) -> None: ...

class DehumStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    left_report: DehumReport
    right_report: DehumReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        left_report: DehumReport,
        right_report: DehumReport,
    ) -> None: ...

@dataclass(frozen=True)
class NoiseDetection:
    floor_dbfs: float
    band_floor_dbfs: NDArray[np.float32]
    def __init__(
        self,
        floor_dbfs: float,
        band_floor_dbfs: NDArray[np.float32],
    ) -> None: ...

class DenoiseReport:
    detected: NoiseDetection
    mean_reduction_db: float
    max_reduction_db: float
    floor_limited_fraction: float
    def __init__(
        self,
        detected: NoiseDetection,
        mean_reduction_db: float,
        max_reduction_db: float,
        floor_limited_fraction: float,
    ) -> None: ...

class DenoiseResult:
    samples: np.ndarray[Any, Any]
    report: DenoiseReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DenoiseReport) -> None: ...

class DenoiseStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    report: DenoiseReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        report: DenoiseReport,
    ) -> None: ...

class DenoiseLinkedResult:
    channels: list[NDArray[np.float32]]
    report: DenoiseReport
    def __init__(
        self,
        channels: list[NDArray[np.float32]],
        report: DenoiseReport,
    ) -> None: ...

class RepairStageReports:
    stage: str
    scope: str
    reports: list[
        DeclipReport
        | DeclickReport
        | DecrackleReport
        | DehumReport
        | DenoiseReport
        | DereverbReport
    ]
    def __init__(
        self,
        stage: str,
        scope: str,
        reports: list[
            DeclipReport
            | DeclickReport
            | DecrackleReport
            | DehumReport
            | DenoiseReport
            | DereverbReport
        ],
    ) -> None: ...

class RepairApplyResult:
    channels: list[NDArray[np.float32]]
    reports: list[RepairStageReports]
    def __init__(
        self,
        channels: list[NDArray[np.float32]],
        reports: list[RepairStageReports],
    ) -> None: ...

@dataclass(frozen=True)
class AudioDefects:
    click: ClickDetection
    clip: ClipDetection
    crackle: CrackleDetection
    noise: NoiseDetection
    hum: HumDetection
    reverb: ReverbDetection
    def __init__(
        self,
        click: ClickDetection,
        clip: ClipDetection,
        crackle: CrackleDetection,
        noise: NoiseDetection,
        hum: HumDetection,
        reverb: ReverbDetection,
    ) -> None: ...

RepairStageName: TypeAlias = Literal[
    "declip", "declick", "decrackle", "dehum", "denoise", "dereverb"
]

class DeclipStage:
    stage: Literal["declip"]
    clip_threshold_db: float | None
    lpc_order: int | None
    iterations: int | None
    lpc_blend: float | None
    def __init__(
        self,
        *,
        clip_threshold_db: float | None = None,
        lpc_order: int | None = None,
        iterations: int | None = None,
        lpc_blend: float | None = None,
    ) -> None: ...

class DeclickStage:
    stage: Literal["declick"]
    threshold: float | None
    neighbor_ratio: float | None
    max_click_samples: int | None
    lpc_order: int | None
    residual_ratio: float | None
    def __init__(
        self,
        *,
        threshold: float | None = None,
        neighbor_ratio: float | None = None,
        max_click_samples: int | None = None,
        lpc_order: int | None = None,
        residual_ratio: float | None = None,
    ) -> None: ...

class DecrackleStage:
    stage: Literal["decrackle"]
    threshold: float | None
    mode: str | None
    levels: int | None
    def __init__(
        self,
        *,
        threshold: float | None = None,
        mode: str | None = None,
        levels: int | None = None,
    ) -> None: ...

class DehumStage:
    stage: Literal["dehum"]
    fundamental_hz: float | None
    harmonics: int | None
    q: float | None
    adaptive: bool | None
    search_range_hz: float | None
    adaptation: float | None
    frame_size: int | None
    pll_bandwidth: float | None
    mode: str | None
    def __init__(
        self,
        *,
        fundamental_hz: float | None = None,
        harmonics: int | None = None,
        q: float | None = None,
        adaptive: bool | None = None,
        search_range_hz: float | None = None,
        adaptation: float | None = None,
        frame_size: int | None = None,
        pll_bandwidth: float | None = None,
        mode: str | None = None,
    ) -> None: ...

class DenoiseStage:
    stage: Literal["denoise"]
    mode: str | None
    noise_estimator: str | None
    n_fft: int | None
    hop_length: int | None
    dd_alpha: float | None
    reduction_db: float | None
    over_subtraction: float | None
    spectral_floor: float | None
    noise_estimation_quantile: float | None
    speech_presence_gain: bool | None
    gain_smoothing: bool | None
    def __init__(
        self,
        *,
        mode: str | None = None,
        noise_estimator: str | None = None,
        n_fft: int | None = None,
        hop_length: int | None = None,
        dd_alpha: float | None = None,
        reduction_db: float | None = None,
        over_subtraction: float | None = None,
        spectral_floor: float | None = None,
        noise_estimation_quantile: float | None = None,
        speech_presence_gain: bool | None = None,
        gain_smoothing: bool | None = None,
    ) -> None: ...

class DereverbStage:
    stage: Literal["dereverb"]
    threshold: float | None
    attenuation: float | None
    n_fft: int | None
    hop_length: int | None
    t60_sec: float | None
    late_delay_ms: float | None
    over_subtraction: float | None
    spectral_floor: float | None
    wpe_enabled: bool | None
    wpe_iterations: int | None
    wpe_taps: int | None
    wpe_strength: float | None
    def __init__(
        self,
        *,
        threshold: float | None = None,
        attenuation: float | None = None,
        n_fft: int | None = None,
        hop_length: int | None = None,
        t60_sec: float | None = None,
        late_delay_ms: float | None = None,
        over_subtraction: float | None = None,
        spectral_floor: float | None = None,
        wpe_enabled: bool | None = None,
        wpe_iterations: int | None = None,
        wpe_taps: int | None = None,
        wpe_strength: float | None = None,
    ) -> None: ...

RepairStage: TypeAlias = (
    DeclipStage | DeclickStage | DecrackleStage | DehumStage | DenoiseStage | DereverbStage
)

class MasteringRepairAnalysis:
    defects: AudioDefects | None
    channels: list[AudioDefects] | None
    declip_threshold_safe: bool
    integrated_lufs: float
    recommended: list[RepairStage]
    explanation: list[str]
    def __init__(
        self,
        defects: AudioDefects | None,
        channels: list[AudioDefects] | None,
        declip_threshold_safe: bool,
        integrated_lufs: float,
        recommended: list[RepairStage],
        explanation: list[str],
    ) -> None: ...

@dataclass(frozen=True)
class ReverbDetection:
    late_decay_ratio_db: float
    late_predictability: float | None
    def __init__(
        self,
        late_decay_ratio_db: float,
        late_predictability: float | None,
    ) -> None: ...

class DereverbReport:
    detected: ReverbDetection
    mean_reduction_db: float
    suppressed_fraction: float
    wpe_predictor_norm: float
    def __init__(
        self,
        detected: ReverbDetection,
        mean_reduction_db: float,
        suppressed_fraction: float,
        wpe_predictor_norm: float,
    ) -> None: ...

class DereverbResult:
    samples: np.ndarray[Any, Any]
    report: DereverbReport
    def __init__(self, samples: np.ndarray[Any, Any], report: DereverbReport) -> None: ...

class DereverbStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    report: DereverbReport
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        report: DereverbReport,
    ) -> None: ...

class DereverbLinkedResult:
    channels: list[NDArray[np.float32]]
    report: DereverbReport
    def __init__(
        self,
        channels: list[NDArray[np.float32]],
        report: DereverbReport,
    ) -> None: ...

class TrimRange:
    first: int
    last_exclusive: int
    def __init__(
        self,
        first: int,
        last_exclusive: int,
    ) -> None: ...

class TrimReport:
    range: TrimRange
    removed_head_samples: int
    removed_tail_samples: int
    def __init__(
        self,
        range: TrimRange,
        removed_head_samples: int,
        removed_tail_samples: int,
    ) -> None: ...

class TrimSilenceResult:
    samples: np.ndarray[Any, Any]
    report: TrimReport
    def __init__(self, samples: np.ndarray[Any, Any], report: TrimReport) -> None: ...

class TrimSilenceStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    report: TrimReport
    left_range: TrimRange
    right_range: TrimRange
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        report: TrimReport,
        left_range: TrimRange,
        right_range: TrimRange,
    ) -> None: ...

class NormalizeStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    length: int
    applied_gain_db: float
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        length: int,
        applied_gain_db: float,
    ) -> None: ...

class MasteringResult:
    samples: NDArray[np.float32]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    non_finite_substitution_count: int
    def __init__(
        self,
        samples: NDArray[np.float32],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
        non_finite_substitution_count: int = 0,
    ) -> None: ...

class MasteringStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    non_finite_substitution_count: int
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
        non_finite_substitution_count: int = 0,
    ) -> None: ...

class LoudnessMatchResult:
    samples: NDArray[np.float32]
    sample_rate: int
    reference_lufs: float | None
    source_lufs: float | None
    applied_gain_db: float
    matched_true_peak_dbtp: float
    def __init__(
        self,
        samples: NDArray[np.float32],
        sample_rate: int,
        reference_lufs: float | None,
        source_lufs: float | None,
        applied_gain_db: float,
        matched_true_peak_dbtp: float,
    ) -> None: ...

class LoudnessMatchStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    sample_rate: int
    reference_lufs: float | None
    source_lufs: float | None
    applied_gain_db: float
    matched_true_peak_dbtp: float
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        sample_rate: int,
        reference_lufs: float | None,
        source_lufs: float | None,
        applied_gain_db: float,
        matched_true_peak_dbtp: float,
    ) -> None: ...

class DynamicsProcessorResult:
    samples: NDArray[np.float32]
    latency_samples: int
    def __init__(self, samples: NDArray[np.float32], latency_samples: int) -> None: ...

class StageGainReduction:
    stage: str
    gain_reduction_db: float
    def __init__(self, stage: str, gain_reduction_db: float) -> None: ...

MixAnalysisBand: TypeAlias = Literal["sub", "low", "lowMid", "mid", "highMid", "high", "air"]

MixSourceClass: TypeAlias = Literal[
    "unknown",
    "kick",
    "snare",
    "hiHat",
    "tom",
    "cymbal",
    "bass",
    "guitar",
    "keys",
    "strings",
    "lead",
    "vocal",
    "backing",
    "percussion",
    "fx",
    "drumKit",
]

class MixAssistantTrackProfile:
    strip_id: str
    name: str | None
    source: MixSourceClass
    source_confidence: float
    usable: bool
    exclusion_reason: str | None
    num_channels: int
    duration_sec: float
    integrated_lufs: float | None
    true_peak_dbtp: float
    crest_factor_db: float
    spectral_centroid_hz: float
    spectral_flatness: float
    attack_per_second: float
    sustain_ratio: float
    band_occupancy: dict[MixAnalysisBand, float]
    def __init__(
        self,
        strip_id: str,
        name: str | None,
        source: MixSourceClass,
        source_confidence: float,
        usable: bool,
        exclusion_reason: str | None,
        num_channels: int,
        duration_sec: float,
        integrated_lufs: float | None,
        true_peak_dbtp: float,
        crest_factor_db: float,
        spectral_centroid_hz: float,
        spectral_flatness: float,
        attack_per_second: float,
        sustain_ratio: float,
        band_occupancy: dict[MixAnalysisBand, float],
    ) -> None: ...

class MixBandDominance:
    masker: str
    maskee: str
    band: MixAnalysisBand
    ratio: float
    valid_frames: int
    def __init__(
        self,
        masker: str,
        maskee: str,
        band: MixAnalysisBand,
        ratio: float,
        valid_frames: int,
    ) -> None: ...

class MixTrackAlignment:
    reference: str
    target: str
    lag_samples: int
    correlation: float
    polarity_opposed: bool
    def __init__(
        self,
        reference: str,
        target: str,
        lag_samples: int,
        correlation: float,
        polarity_opposed: bool,
    ) -> None: ...

class MixCrowdedBand:
    band: MixAnalysisBand
    crowding: float
    def __init__(self, band: MixAnalysisBand, crowding: float) -> None: ...

class MixMonoRisk:
    strip_id: str
    correlation: float
    width: float
    wide_low_end: bool
    def __init__(
        self,
        strip_id: str,
        correlation: float,
        width: float,
        wide_low_end: bool,
    ) -> None: ...

class MixAssistantMixProfile:
    track_count: int
    band_dominance: list[MixBandDominance]
    alignment: list[MixTrackAlignment]
    crowded_bands: list[MixCrowdedBand]
    mono_risks: list[MixMonoRisk]
    def __init__(
        self,
        track_count: int,
        band_dominance: list[MixBandDominance],
        alignment: list[MixTrackAlignment],
        crowded_bands: list[MixCrowdedBand],
        mono_risks: list[MixMonoRisk],
    ) -> None: ...

class MixAssistantResult:
    scene: dict[str, Any]
    tracks: list[MixAssistantTrackProfile]
    mix: MixAssistantMixProfile
    explanation: list[str]
    def __init__(
        self,
        scene: dict[str, Any],
        tracks: list[MixAssistantTrackProfile],
        mix: MixAssistantMixProfile,
        explanation: list[str],
    ) -> None: ...

@dataclass(frozen=True)
class MasteringAudioProfileLoudness:
    integrated_lufs: float | None
    loudness_range_lu: float
    true_peak_dbtp: float
    crest_factor_db: float
    def __init__(
        self,
        integrated_lufs: float | None,
        loudness_range_lu: float,
        true_peak_dbtp: float,
        crest_factor_db: float,
    ) -> None: ...

@dataclass(frozen=True)
class MasteringAudioProfileSpectral:
    sub_rms_db: float
    low_rms_db: float
    low_mid_rms_db: float
    mid_rms_db: float
    high_mid_rms_db: float
    high_rms_db: float
    air_rms_db: float
    centroid_hz: float
    flatness: float
    rolloff_hz: float
    def __init__(
        self,
        sub_rms_db: float,
        low_rms_db: float,
        low_mid_rms_db: float,
        mid_rms_db: float,
        high_mid_rms_db: float,
        high_rms_db: float,
        air_rms_db: float,
        centroid_hz: float,
        flatness: float,
        rolloff_hz: float,
    ) -> None: ...

@dataclass(frozen=True)
class MasteringAudioProfileDynamics:
    short_term_lufs_std: float
    attack_per_second: float
    sustain_ratio: float
    def __init__(
        self,
        short_term_lufs_std: float,
        attack_per_second: float,
        sustain_ratio: float,
    ) -> None: ...

@dataclass(frozen=True)
class MasteringAudioProfile:
    duration_sec: float
    bpm: float
    bpm_confidence: float
    loudness: MasteringAudioProfileLoudness
    spectral: MasteringAudioProfileSpectral
    dynamics: MasteringAudioProfileDynamics
    defects: AudioDefects | None
    def __init__(
        self,
        duration_sec: float,
        bpm: float,
        bpm_confidence: float,
        loudness: MasteringAudioProfileLoudness,
        spectral: MasteringAudioProfileSpectral,
        dynamics: MasteringAudioProfileDynamics,
        defects: AudioDefects | None,
    ) -> None: ...

MasteringChainConfig: TypeAlias = dict[str, Any]
EqBandInput: TypeAlias = dict[str, Any]

@dataclass(frozen=True)
class MasteringAssistantProfile:
    duration_sec: float
    bpm: float
    bpm_confidence: float
    integrated_lufs: float | None
    loudness_range_lu: float
    true_peak_dbtp: float
    crest_factor_db: float
    spectral_centroid_hz: float
    spectral_flatness: float
    attack_per_second: float
    sustain_ratio: float
    def __init__(
        self,
        duration_sec: float,
        bpm: float,
        bpm_confidence: float,
        integrated_lufs: float | None,
        loudness_range_lu: float,
        true_peak_dbtp: float,
        crest_factor_db: float,
        spectral_centroid_hz: float,
        spectral_flatness: float,
        attack_per_second: float,
        sustain_ratio: float,
    ) -> None: ...

@dataclass(frozen=True)
class MasteringAssistantResult:
    chain_config: MasteringChainConfig
    explanation: list[str]
    profile: MasteringAssistantProfile
    def __init__(
        self,
        chain_config: MasteringChainConfig,
        explanation: list[str],
        profile: MasteringAssistantProfile,
    ) -> None: ...

class StreamingPlatform(TypedDict):
    name: str
    target_lufs: float
    ceiling_dbtp: float

@dataclass(frozen=True)
class MasteringStreamingPreviewPlatform:
    name: str
    integrated_lufs: float | None
    true_peak_dbtp: float
    normalization_gain_db: float
    ceiling_risk: bool
    def __init__(
        self,
        name: str,
        integrated_lufs: float | None,
        true_peak_dbtp: float,
        normalization_gain_db: float,
        ceiling_risk: bool,
    ) -> None: ...

@dataclass(frozen=True)
class MasteringStreamingPreviewResult:
    platforms: list[MasteringStreamingPreviewPlatform]
    def __init__(self, platforms: list[MasteringStreamingPreviewPlatform]) -> None: ...

@dataclass(frozen=True)
class MatchReferenceLoudnessResult:
    source_lufs: float | None
    reference_lufs: float | None
    gain_to_match_db: float | None
    def __init__(
        self,
        source_lufs: float | None,
        reference_lufs: float | None,
        gain_to_match_db: float | None,
    ) -> None: ...

@dataclass(frozen=True)
class MatchTonalBalanceBand:
    low_hz: float
    high_hz: float
    source_db: float
    reference_db: float
    deviation_db: float
    def __init__(
        self,
        low_hz: float,
        high_hz: float,
        source_db: float,
        reference_db: float,
        deviation_db: float,
    ) -> None: ...

@dataclass(frozen=True)
class MatchTonalBalanceResult:
    bands: list[MatchTonalBalanceBand]
    def __init__(self, bands: list[MatchTonalBalanceBand]) -> None: ...

@dataclass(frozen=True)
class MatchEqCurveResult:
    frequencies: NDArray[np.float32]
    gain_db: NDArray[np.float32]
    def __init__(self, frequencies: NDArray[np.float32], gain_db: NDArray[np.float32]) -> None: ...

@dataclass(frozen=True)
class MatchEstimateReferenceDelaySamplesResult:
    delay_samples: int
    def __init__(self, delay_samples: int) -> None: ...

@dataclass(frozen=True)
class StereoMonoCompatCheckResult:
    correlation: float
    width: float | None
    mono_peak: float
    side_rms: float
    likely_mono_compatible: bool
    def __init__(
        self,
        correlation: float,
        width: float | None,
        mono_peak: float,
        side_rms: float,
        likely_mono_compatible: bool,
    ) -> None: ...

@dataclass(frozen=True)
class StereoMonoCompatBand:
    low_hz: float
    high_hz: float
    correlation: float
    side_rms: float
    def __init__(
        self,
        low_hz: float,
        high_hz: float,
        correlation: float,
        side_rms: float,
    ) -> None: ...

@dataclass(frozen=True)
class StereoMonoCompatCheckLogBandsResult:
    bands: list[StereoMonoCompatBand]
    def __init__(self, bands: list[StereoMonoCompatBand]) -> None: ...

class StreamingLoudnessGain:
    loudness_static_gain_db: float
    true_peak_dbtp: float | None
    integrated_lufs: float | None
    def __init__(
        self,
        loudness_static_gain_db: float,
        true_peak_dbtp: float | None,
        integrated_lufs: float | None,
    ) -> None: ...

class MasteringLoudnessSummary:
    integrated_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    true_peak_dbtp: float
    loudness_range_lu: float
    def __init__(
        self,
        integrated_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        true_peak_dbtp: float,
        loudness_range_lu: float,
    ) -> None: ...

class MasteringReport:
    before: MasteringLoudnessSummary
    after: MasteringLoudnessSummary
    applied_gain_db: float
    max_gain_reduction_db: float
    loudness_target_limited: bool
    band_energy_delta_db: NDArray[np.float32]
    def __init__(
        self,
        before: MasteringLoudnessSummary,
        after: MasteringLoudnessSummary,
        applied_gain_db: float,
        max_gain_reduction_db: float,
        loudness_target_limited: bool,
        band_energy_delta_db: NDArray[np.float32] = ...,
    ) -> None: ...

class MasteringChainResult:
    samples: NDArray[np.float32]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_loudness_range_lu: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    non_finite_substitution_count: int
    def __init__(
        self,
        samples: NDArray[np.float32],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_loudness_range_lu: float = ...,
        loudness_target_limited: bool = ...,
        stage_gain_reductions: list[StageGainReduction] = ...,
        report: MasteringReport | None = ...,
        non_finite_substitution_count: int = ...,
    ) -> None: ...

class MasteringChainStereoResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_loudness_range_lu: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    non_finite_substitution_count: int
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_loudness_range_lu: float = ...,
        loudness_target_limited: bool = ...,
        stage_gain_reductions: list[StageGainReduction] = ...,
        report: MasteringReport | None = ...,
        non_finite_substitution_count: int = ...,
    ) -> None: ...

class MixMeterSnapshot:
    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    correlation: float
    mono_compat_width: float
    mono_compat_peak: float
    mono_compat_side_rms: float
    likely_mono_compatible: bool
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    true_peak_dbtp_l: float
    true_peak_dbtp_r: float
    max_true_peak_dbtp: float
    seq: int
    num_channels: int | None
    peak_db: tuple[float, ...]
    rms_db: tuple[float, ...]
    true_peak_dbtp: tuple[float, ...]
    def __init__(
        self,
        peak_db_l: float,
        peak_db_r: float,
        rms_db_l: float,
        rms_db_r: float,
        correlation: float,
        mono_compat_width: float,
        mono_compat_peak: float,
        mono_compat_side_rms: float,
        likely_mono_compatible: bool,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        true_peak_dbtp_l: float,
        true_peak_dbtp_r: float,
        max_true_peak_dbtp: float,
        seq: int,
        num_channels: int | None = ...,
        peak_db: tuple[float, ...] = ...,
        rms_db: tuple[float, ...] = ...,
        true_peak_dbtp: tuple[float, ...] = ...,
    ) -> None: ...

class GoniometerPoint:
    left: float
    right: float
    def __init__(self, left: float, right: float) -> None: ...

class MixResult:
    left: NDArray[np.float32]
    right: NDArray[np.float32]
    sample_rate: int
    meters: list[MixMeterSnapshot]
    def __init__(
        self,
        left: NDArray[np.float32],
        right: NDArray[np.float32],
        sample_rate: int,
        meters: list[MixMeterSnapshot],
    ) -> None: ...

class EngineTelemetry:
    type: EngineTelemetryType
    error: EngineTelemetryError
    render_frame: int
    timeline_sample: int
    audible_timeline_sample: int
    graph_latency_samples: float
    value: int
    def __init__(
        self,
        type: EngineTelemetryType,
        error: EngineTelemetryError,
        render_frame: int,
        timeline_sample: int,
        audible_timeline_sample: int,
        graph_latency_samples: float,
        value: int,
    ) -> None: ...

EngineTrackMonitorMode = Literal["off", "pfl", "afl"]

class ParameterInfo:
    id: int
    name: str
    unit: str
    min_value: float
    max_value: float
    default_value: float
    rt_safe: bool
    default_curve: AutomationCurve
    def __init__(
        self,
        id: int,
        name: str,
        unit: str,
        min_value: float,
        max_value: float,
        default_value: float,
        rt_safe: bool,
        default_curve: AutomationCurve,
    ) -> None: ...

class AutomationPoint:
    ppq: float
    value: float
    curve_to_next: AutomationCurve
    def __init__(
        self,
        ppq: float,
        value: float,
        curve_to_next: AutomationCurve = "linear",
    ) -> None: ...

MarkerKind = Literal["marker", "text", "lyric", "cuePoint", "keySignature"]
SidechainRefusal = Literal[
    "invalidTarget",
    "insertOutOfRange",
    "undeclaredSource",
    "invalidSourceKind",
    "selfKey",
    "cycle",
    "tableFull",
    "planRefused",
]
EngineCaptureSource = Literal["output", "input"]
MidiFxConfig = dict[str, Any]
MixSceneStripInput = dict[str, Any]
MixSceneBusInput = dict[str, Any]
MixSceneDocument = dict[str, Any]

class SidechainSource:
    kind: SidechainSourceKind
    id: int
    def __init__(self, kind: SidechainSourceKind, id: int) -> None: ...

class CaptureInputMonitor:
    enabled: bool
    gain: float
    def __init__(self, enabled: bool, gain: float = 1.0) -> None: ...

class CapturePunch:
    start_ppq: float | None
    start_sample: int | None
    start_sec: float | None
    end_ppq: float | None
    end_sample: int | None
    end_sec: float | None
    def __init__(
        self,
        *,
        start_ppq: float | None = None,
        start_sample: int | None = None,
        start_sec: float | None = None,
        end_ppq: float | None = None,
        end_sample: int | None = None,
        end_sec: float | None = None,
    ) -> None: ...

class SidechainCheck:
    ok: bool
    reason: SidechainRefusal | None
    def __init__(self, ok: bool, reason: SidechainRefusal | None = None) -> None: ...

class EngineMarker:
    id: int
    ppq: float
    name: str
    kind: MarkerKind
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: MarkerKind = "marker",
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectMarker:
    id: int
    ppq: float
    name: str
    kind: MarkerKind
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: MarkerKind = "marker",
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectTrack:
    id: int
    kind: ProjectTrackKind
    midi_destination_id: int
    gain: float
    pan: float
    mute: bool
    solo: bool
    name: str
    def __init__(
        self,
        id: int,
        kind: ProjectTrackKind,
        midi_destination_id: int,
        gain: float,
        pan: float,
        mute: bool,
        solo: bool,
        name: str,
    ) -> None: ...

class ProjectClip:
    id: int
    track_id: int
    source_id: int
    source_kind: ProjectSourceKind
    start_ppq: float
    length_ppq: float
    source_offset_ppq: float
    gain: float
    loop_mode: ProjectLoopMode
    loop_length_ppq: float
    def __init__(
        self,
        id: int,
        track_id: int,
        source_id: int,
        source_kind: ProjectSourceKind,
        start_ppq: float,
        length_ppq: float,
        source_offset_ppq: float,
        gain: float,
        loop_mode: ProjectLoopMode,
        loop_length_ppq: float,
    ) -> None: ...

class ProjectSource:
    id: int
    kind: ProjectSourceKind
    num_channels: int
    storage_handle_id: int
    sample_rate_hint: float
    name_or_uri: str
    content_hash: str
    external_stem_role: str
    def __init__(
        self,
        id: int,
        kind: ProjectSourceKind,
        num_channels: int,
        storage_handle_id: int,
        sample_rate_hint: float,
        name_or_uri: str,
        content_hash: str = "",
        external_stem_role: str = "",
    ) -> None: ...

class EngineMetronomeConfig:
    enabled: bool
    beat_gain: float
    accent_gain: float
    click_samples: int
    click_sec: float
    def __init__(
        self,
        enabled: bool = False,
        beat_gain: float = 0.35,
        accent_gain: float = 0.7,
        click_samples: int = 0,
        click_sec: float = 0.0,
    ) -> None: ...

class EngineClip:
    id: int
    channels: PlanarChannels | None
    start_ppq: float
    track_id: int
    length_samples: int | None
    clip_offset_samples: int
    loop: bool
    gain: float
    fade_in_samples: int
    fade_out_samples: int
    warp_mode: ProjectWarpMode
    warp_anchors: list[tuple[float, float]] | None
    page_provider: object | None
    def __init__(
        self,
        id: int,
        channels: PlanarChannels | None,
        start_ppq: float,
        track_id: int = 0,
        length_samples: int | None = None,
        clip_offset_samples: int = 0,
        loop: bool = False,
        gain: float = 1.0,
        fade_in_samples: int = 0,
        fade_out_samples: int = 0,
        warp_mode: ProjectWarpMode = "off",
        warp_anchors: list[tuple[float, float]] | None = None,
        page_provider: object | None = None,
    ) -> None: ...

class EngineMidiEvent:
    render_frame: int
    word0: int
    word1: int
    word2: int
    word3: int
    word_count: int
    group: int
    sysex_handle: int
    def __init__(
        self,
        render_frame: int,
        word0: int = 0,
        word1: int = 0,
        word2: int = 0,
        word3: int = 0,
        word_count: int = 0,
        group: int = 0,
        sysex_handle: int = 0,
    ) -> None: ...

class EngineMidiClipSchedule:
    events: list[EngineMidiEvent]
    id: int
    track_id: int
    destination_id: int | None
    start_sample: int
    start_ppq: float
    length_samples: int
    loop: bool
    loop_length_samples: int
    gain: float
    fade_in_samples: int
    fade_out_samples: int
    def __init__(
        self,
        events: list[EngineMidiEvent],
        id: int = 0,
        track_id: int = 0,
        destination_id: int | None = None,
        start_sample: int = 0,
        start_ppq: float = 0.0,
        length_samples: int = 0,
        loop: bool = False,
        loop_length_samples: int = 0,
        gain: float = 1.0,
        fade_in_samples: int = 0,
        fade_out_samples: int = 0,
    ) -> None: ...

class ClipPageRequest:
    clip_id: int
    channel: int
    sample: int
    def __init__(self, clip_id: int, channel: int, sample: int) -> None: ...

class EngineCaptureStatus:
    captured_frames: int
    overflow_count: int
    armed: bool
    punch_enabled: bool
    source: EngineCaptureSource
    record_offset_samples: int
    def __init__(
        self,
        captured_frames: int,
        overflow_count: int,
        armed: bool,
        punch_enabled: bool,
        source: str,
        record_offset_samples: int,
    ) -> None: ...

class EngineBounceOptions:
    total_frames: int
    num_channels: int
    block_size: int | None
    # 0 = same as the source (no resampling).
    target_sample_rate: int
    # 0 = the engine's prepared rate; a non-zero value must equal it.
    source_sample_rate: int
    normalize_lufs: bool
    target_lufs: float
    dither: int
    dither_bits: int
    dither_seed: int
    def __init__(
        self,
        total_frames: int,
        num_channels: int,
        block_size: int | None = None,
        target_sample_rate: int = 0,
        source_sample_rate: int = 0,
        normalize_lufs: bool = False,
        target_lufs: float = -14.0,
        dither: int = 0,
        dither_bits: int = 16,
        dither_seed: int = 0,
    ) -> None: ...

class EngineProcessWithMonitorResult:
    output: tuple[NDArray[np.float32], ...]
    monitor: tuple[NDArray[np.float32], ...]
    def __init__(
        self,
        output: tuple[NDArray[np.float32], ...],
        monitor: tuple[NDArray[np.float32], ...],
    ) -> None: ...

class EngineBounceResult:
    interleaved: np.ndarray
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float
    def __init__(
        self,
        interleaved: NDArray[np.float32],
        frames: int,
        num_channels: int,
        sample_rate: int,
        integrated_lufs: float,
    ) -> None: ...

class EngineFreezeOptions:
    total_frames: int
    num_channels: int
    block_size: int | None
    clip_id: int
    start_ppq: float
    gain: float
    def __init__(
        self,
        total_frames: int,
        num_channels: int,
        block_size: int | None = None,
        clip_id: int = 1,
        start_ppq: float = 0.0,
        gain: float = 1.0,
    ) -> None: ...

class EngineFreezeResult:
    clip_id: int
    frames: int
    num_channels: int
    def __init__(self, clip_id: int, frames: int, num_channels: int) -> None: ...

EngineGraphNodeType = Literal["passThrough", "gain"]
EngineGraphMix = Literal["replace", "add"]
"""Mixing intent for a graph edge.

Edges into one destination port are taken in connection order. ``"replace"``
overwrites what earlier edges put on the port; ``"add"`` accumulates onto it.
A port's first edge initializes it either way, so the two differ only on a
shared port. An omitted ``mix`` is ``"add"``, unlike a zero-initialized C
connection, which is ``"replace"``.
"""

class EngineGraphNode:
    id: str
    type: EngineGraphNodeType
    gain_db: float
    num_ports: int
    def __init__(
        self,
        id: str,
        type: EngineGraphNodeType = "passThrough",
        gain_db: float = 0.0,
        num_ports: int = 0,
    ) -> None: ...

class EngineGraphConnection:
    source_node: str
    source_port: int
    dest_node: str
    dest_port: int
    mix: EngineGraphMix
    def __init__(
        self,
        source_node: str,
        source_port: int,
        dest_node: str,
        dest_port: int,
        mix: EngineGraphMix = "add",
    ) -> None: ...

class EngineGraphParameterBinding:
    param_id: int
    node_id: str
    def __init__(self, param_id: int, node_id: str) -> None: ...

class EngineGraphSpec:
    nodes: list[EngineGraphNode]
    connections: list[EngineGraphConnection]
    input_node: str
    output_node: str
    num_channels: int
    parameter_bindings: list[EngineGraphParameterBinding] | None
    def __init__(
        self,
        nodes: list[EngineGraphNode],
        connections: list[EngineGraphConnection],
        input_node: str,
        output_node: str,
        num_channels: int = 2,
        parameter_bindings: list[EngineGraphParameterBinding] | None = None,
    ) -> None: ...

class MeterTelemetryRecord:
    """Stereo meter snapshot drained from the engine meter tap.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

    target_id: int
    render_frame: int
    seq: int
    peak_db_l: float
    peak_db_r: float
    rms_db_l: float
    rms_db_r: float
    true_peak_dbtp_l: float
    true_peak_dbtp_r: float
    max_true_peak_dbtp: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    input_peak_db_l: float
    input_peak_db_r: float
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        peak_db_l: float,
        peak_db_r: float,
        rms_db_l: float,
        rms_db_r: float,
        true_peak_dbtp_l: float,
        true_peak_dbtp_r: float,
        max_true_peak_dbtp: float,
        correlation: float,
        mono_compat_width: float,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        dropped_records: int,
        input_peak_db_l: float = ...,
        input_peak_db_r: float = ...,
    ) -> None: ...

class MeterTelemetryRecordWide:
    """Per-plane meter snapshot for a surround mix target.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

    target_id: int
    render_frame: int
    seq: int
    num_channels: int
    peak_db: NDArray[np.float32]
    rms_db: NDArray[np.float32]
    true_peak_dbtp: NDArray[np.float32]
    max_true_peak_dbtp: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    input_peak_db: NDArray[np.float32]
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        num_channels: int,
        peak_db: NDArray[np.float32],
        rms_db: NDArray[np.float32],
        true_peak_dbtp: NDArray[np.float32],
        max_true_peak_dbtp: float,
        correlation: float,
        mono_compat_width: float,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        dropped_records: int,
        input_peak_db: NDArray[np.float32] = ...,
    ) -> None: ...

class ScopeTelemetryRecord:
    """Spectrum + goniometer snapshot drained from the scope tap.

    ``target_id`` encodes the mix target: ``0`` master mix, ``lane_index + 1``
    (1..32) for track lanes, ``33 + bus_index`` (33..40) for buses, and
    ``0xFFFF`` for the input-monitor capture tap. Handle ``0xFFFF`` explicitly --
    a naive ``target_id - 1`` index into a 32-entry lane array runs past its end.
    """

    target_id: int
    render_frame: int
    seq: int
    dropped_records: int
    bands: NDArray[np.float32]
    points: list[tuple[float, float]]
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        dropped_records: int,
        bands: NDArray[np.float32],
        points: list[tuple[float, float]],
    ) -> None: ...

class ExternalMidiEvent:
    destination_id: int | None
    render_frame: int
    bytes: builtins.bytes
    def __init__(
        self,
        destination_id: int | None,
        render_frame: int,
        bytes: builtins.bytes,
    ) -> None: ...

class TransportState:
    playing: bool
    looping: bool
    render_frame: int
    sample_position: int
    ppq_position: float
    bpm: float
    loop_start_ppq: float
    loop_end_ppq: float
    sample_rate: float
    bar_start_ppq: float
    bar_count: int
    time_signature: TimeSignature
    beat: int
    beat_fraction: float
    def __init__(
        self,
        playing: bool,
        looping: bool,
        render_frame: int,
        sample_position: int,
        ppq_position: float,
        bpm: float,
        loop_start_ppq: float,
        loop_end_ppq: float,
        sample_rate: float,
        bar_start_ppq: float,
        bar_count: int,
        time_signature: TimeSignature,
        beat: int,
        beat_fraction: float,
    ) -> None: ...

class Section:
    type: SectionType
    start_sec: float
    end_sec: float
    energy_level: float  # median frame RMS in the section over the loudest section's, [0, 1]
    confidence: float
    group: int  # sections sharing a value repeat the same material; 0, 1, ... by first appearance
    name: str
    def __init__(
        self,
        type: SectionType,
        start_sec: float,
        end_sec: float,
        energy_level: float,
        confidence: float,
        group: int = 0,
        name: str = "",
    ) -> None: ...

class Boundary:
    time_sec: float
    frame: int
    strength: float
    def __init__(self, time_sec: float, frame: int, strength: float) -> None: ...

class BoundaryResult:
    boundaries: list[Boundary]
    novelty_curve: NDArray[np.float32]
    novelty_peak: float | None
    sample_rate: int
    hop_length: int
    frame_stride: int
    def __init__(
        self,
        boundaries: list[Boundary],
        novelty_curve: NDArray[np.float32],
        novelty_peak: float | None,
        sample_rate: int,
        hop_length: int,
        frame_stride: int,
    ) -> None: ...

class MelodyPoint:
    time_sec: float
    frequency_hz: float | None
    confidence: float
    def __init__(self, time_sec: float, frequency_hz: float | None, confidence: float) -> None: ...

class MelodyResult:
    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency_hz: float
    vibrato_rate_hz: float
    def __init__(
        self,
        points: list[MelodyPoint],
        pitch_range_octaves: float,
        pitch_stability: float,
        mean_frequency_hz: float,
        vibrato_rate_hz: float,
    ) -> None: ...

class CqtResult:
    hop_length: int
    sample_rate: int
    magnitude: NDArray[np.float32]
    frequencies: NDArray[np.float32]
    def __init__(
        self,
        hop_length: int,
        sample_rate: int,
        magnitude: NDArray[np.float32],
        frequencies: NDArray[np.float32],
    ) -> None: ...

class StreamQuantizeConfig:
    mel_db_min: float
    mel_db_max: float
    onset_max: float
    rms_max: float
    centroid_max: float
    def __init__(
        self,
        mel_db_min: float = -80.0,
        mel_db_max: float = 0.0,
        onset_max: float = 50.0,
        rms_max: float = 1.0,
        centroid_max: float = 11025.0,
    ) -> None: ...

class StreamFrames:
    """Matrices are band-major ``(bands, n_frames)``; a feature not computed is
    ``None``. ``mel`` is linear power."""

    n_frames: int
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.float32] | None
    chroma: NDArray[np.float32] | None
    onset_strength: NDArray[np.float32] | None
    rms_energy: NDArray[np.float32]
    spectral_centroid: NDArray[np.float32] | None
    spectral_flatness: NDArray[np.float32] | None
    chord_root: NDArray[np.int32] | None
    chord_quality: NDArray[np.int32] | None
    chord_confidence: NDArray[np.float32] | None
    def __init__(
        self,
        n_frames: int,
        timestamps_sec: NDArray[np.float32],
        mel: NDArray[np.float32] | None,
        chroma: NDArray[np.float32] | None,
        onset_strength: NDArray[np.float32] | None,
        rms_energy: NDArray[np.float32],
        spectral_centroid: NDArray[np.float32] | None,
        spectral_flatness: NDArray[np.float32] | None,
        chord_root: NDArray[np.int32] | None,
        chord_quality: NDArray[np.int32] | None,
        chord_confidence: NDArray[np.float32] | None,
    ) -> None: ...

class StreamFramesU8:
    """Shaped as :class:`StreamFrames`; ``mel`` is dB quantized over
    ``[mel_db_min, mel_db_max]``."""

    n_frames: int
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.uint8] | None
    chroma: NDArray[np.uint8] | None
    onset_strength: NDArray[np.uint8] | None
    rms_energy: NDArray[np.uint8]
    spectral_centroid: NDArray[np.uint8] | None
    spectral_flatness: NDArray[np.uint8] | None
    def __init__(
        self,
        n_frames: int,
        timestamps_sec: NDArray[np.float32],
        mel: NDArray[np.uint8] | None,
        chroma: NDArray[np.uint8] | None,
        onset_strength: NDArray[np.uint8] | None,
        rms_energy: NDArray[np.uint8],
        spectral_centroid: NDArray[np.uint8] | None,
        spectral_flatness: NDArray[np.uint8] | None,
    ) -> None: ...

class StreamFramesI16:
    """Shaped as :class:`StreamFrames`; ``mel`` is dB quantized over
    ``[mel_db_min, mel_db_max]``."""

    n_frames: int
    timestamps_sec: NDArray[np.float32]
    mel: NDArray[np.int16] | None
    chroma: NDArray[np.int16] | None
    onset_strength: NDArray[np.int16] | None
    rms_energy: NDArray[np.int16]
    spectral_centroid: NDArray[np.int16] | None
    spectral_flatness: NDArray[np.int16] | None
    def __init__(
        self,
        n_frames: int,
        timestamps_sec: NDArray[np.float32],
        mel: NDArray[np.int16] | None,
        chroma: NDArray[np.int16] | None,
        onset_strength: NDArray[np.int16] | None,
        rms_energy: NDArray[np.int16],
        spectral_centroid: NDArray[np.int16] | None,
        spectral_flatness: NDArray[np.int16] | None,
    ) -> None: ...

class StreamChordChange:
    root: PitchClass
    quality: str
    name: str
    start_sec: float
    confidence: float
    def __init__(
        self, root: PitchClass, quality: str, name: str, start_sec: float, confidence: float
    ) -> None: ...

class StreamBarChord:
    """``bar_index`` is the bar number, not this entry's position in the list:
    bars with no confident chord are not recorded and the oldest entries are
    dropped at the history cap, so group bars by pattern position with
    ``bar_index``. ``start_sec`` is on the same timeline as
    ``StreamFrames.timestamps_sec`` and consecutive bars are ``bar_duration_sec``
    apart. In ``voted_pattern``, ``bar_index`` is the pattern position,
    ``start_sec`` is unused, and a position no chord was voted for is ``None``."""

    bar_index: int
    root: PitchClass
    quality: str
    name: str
    start_sec: float
    confidence: float
    def __init__(
        self,
        bar_index: int,
        root: PitchClass,
        quality: str,
        name: str,
        start_sec: float,
        confidence: float,
    ) -> None: ...

class StreamPatternScore:
    name: str
    score: float
    def __init__(self, name: str, score: float) -> None: ...

class StreamProgressiveEstimate:
    """A field is ``None`` until the analyzer has an estimate for it.
    ``bpm_candidate_count`` is the number of tempo candidates the most recent
    BPM estimate chose from, 0 until one has run. ``updated`` is true when the
    key or BPM was re-estimated since the previous snapshot: one change sets it
    on exactly one snapshot however the caller chunks its input."""

    bpm: float | None
    bpm_confidence: float | None
    bpm_candidate_count: int
    key: Key | None
    chord: StreamChordChange | None
    chord_progression: list[StreamChordChange]
    bar_chord_progression: list[StreamBarChord]
    current_bar: int | None
    bar_duration_sec: float | None
    voted_pattern: list[StreamBarChord | None]
    pattern_length: int
    detected_pattern_name: str | None
    detected_pattern_score: float | None
    all_pattern_scores: list[StreamPatternScore]
    accumulated_sec: float
    used_frames: int
    updated: bool
    def __init__(
        self,
        bpm: float | None,
        bpm_confidence: float | None,
        bpm_candidate_count: int,
        key: Key | None,
        chord: StreamChordChange | None,
        chord_progression: list[StreamChordChange],
        bar_chord_progression: list[StreamBarChord],
        current_bar: int | None,
        bar_duration_sec: float | None,
        voted_pattern: list[StreamBarChord | None],
        pattern_length: int,
        detected_pattern_name: str | None,
        detected_pattern_score: float | None,
        all_pattern_scores: list[StreamPatternScore],
        accumulated_sec: float,
        used_frames: int,
        updated: bool,
    ) -> None: ...

class StreamStats:
    total_frames: int
    total_samples: int
    duration_sec: float
    pending_frames: int
    dropped_output_frames: int
    dropped_chord_progression_entries: int
    dropped_bar_progression_entries: int
    non_finite_discard_blocks: int
    estimate: StreamProgressiveEstimate
    def __init__(
        self,
        total_frames: int,
        total_samples: int,
        duration_sec: float,
        pending_frames: int,
        dropped_output_frames: int,
        dropped_chord_progression_entries: int,
        dropped_bar_progression_entries: int,
        non_finite_discard_blocks: int,
        estimate: StreamProgressiveEstimate,
    ) -> None: ...
