from __future__ import annotations

import builtins
from collections.abc import Sequence
from dataclasses import dataclass
from enum import IntEnum
from typing import Any, Literal, NotRequired, TypeAlias, TypedDict

import numpy as np
from numpy.typing import NDArray

FloatSamples: TypeAlias = Sequence[float] | list[float] | np.ndarray[Any, Any]
# Planar audio: one buffer per channel, or a 2-D (channels, frames) array.
PlanarChannels: TypeAlias = Sequence[FloatSamples] | np.ndarray[Any, Any]

MasteringProcessorKind = Literal["realtime", "offline", "pair"]
MasteringChannelPolicy = Literal["multichannel", "stereoPairOnly", "perChannel", "passthrough"]
MasteringPresetKind = Literal["mastering", "restoration"]
MasteringInsertParamUnit = Literal[
    "dB",
    "dBFS",
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

    ``target_lufs``, ``true_peak_ceiling_db`` and
    ``max_limiter_gain_reduction_db`` are ``None`` for a ``"restoration"``
    preset, which carries no loudness stage.
    """

    name: str
    kind: MasteringPresetKind
    target_lufs: float | None
    true_peak_ceiling_db: float | None
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
class CapabilityCatalogChainAlias:
    """The other spellings of a canonical chain key; ``<i>`` stands for a list index."""

    key: str
    """Canonical chain key."""
    aliases: tuple[str, ...]
    """Other accepted spellings."""

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
    aliases: tuple[CapabilityCatalogChainAlias, ...]
    """Alternative spellings of chain keys."""

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

class AutomationCurve(IntEnum):
    LINEAR = 0
    EXPONENTIAL = 1
    HOLD = 2
    S_CURVE = 3

class PanLaw(IntEnum):
    """Mono centre-gain law; stereo Balance keeps centre unity and tapers only the far channel."""

    CONST_3DB = 0
    CONST_4_5DB = 1
    CONST_6DB = 2
    LINEAR_0DB = 3

class ChannelLayout(IntEnum):
    MONO = 0
    STEREO = 1
    FIVE_POINT_ONE = 2
    SEVEN_POINT_ONE = 3

class MeterTap(IntEnum):
    PRE_FADER = 0
    POST_FADER = 1

class SendTiming(IntEnum):
    POST_FADER = 0
    PRE_FADER = 1

class SidechainSourceKind(IntEnum):
    TRACK = 0
    BUS = 1

class EngineTelemetryType(IntEnum):
    PROCESS_BLOCK = 0
    ERROR = 1

class EngineTelemetryError(IntEnum):
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
    samples: list[float]
    key: Key
    def __init__(self, samples: list[float], key: Key) -> None: ...

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
    loudness_range_db: float
    is_compressed: bool
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_db: float,
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

class AnalysisMelody:
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    pitches: list[MelodyPoint]
    def __init__(
        self,
        pitch_range_octaves: float,
        pitch_stability: float,
        mean_frequency: float,
        vibrato_rate: float,
        pitches: list[MelodyPoint],
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
    searched at 22050 Hz. :func:`analyze_bpm` searches 30-300 BPM at the input rate
    by default, so the two can differ on the same input; pass ``bpm_min=60,
    bpm_max=200`` and a 22050 Hz input to compare them.
    """

    bpm: float
    bpm_confidence: float
    key: Key
    time_signature: TimeSignature
    beats: list[Beat]
    downbeats: list[Beat]
    downbeat_phase: int
    bpm_candidates: list[BpmCandidate]
    time_signature_candidates: list[TimeSignature]
    chords: list[AnalysisChord]
    sections: list[Section]
    timbre: TimbreFrame | None
    dynamics: AnalysisDynamics | None
    rhythm: AnalysisRhythm | None
    melody: AnalysisMelody | None
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
        beats: list[Beat] = ...,
        downbeats: list[Beat] = ...,
        downbeat_phase: int = 0,
        bpm_candidates: list[BpmCandidate] = ...,
        time_signature_candidates: list[TimeSignature] = ...,
        chords: list[AnalysisChord] = ...,
        sections: list[Section] = ...,
        timbre: TimbreFrame | None = None,
        dynamics: AnalysisDynamics | None = None,
        rhythm: AnalysisRhythm | None = None,
        melody: AnalysisMelody | None = None,
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
    relation: str | None
    def __init__(self, bpm: float, confidence: float, relation: str | None = None) -> None: ...

class BpmAnalysisResult:
    """Detailed BPM analysis result.

    ``confidence`` is the winning tempo cluster's vote share within
    ``bpm_min``-``bpm_max`` (30-300 by default), searched at the input rate;
    :func:`analyze` searches 60-200 BPM at 22050 Hz, so its ``bpm_confidence``
    can differ. ``candidates`` opens with the chosen tempo
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
    time, which only a measured impulse response provides, so they are NaN when
    ``is_blind`` is true -- ``edt`` measures the 0 to -10 dB decay and the blind
    estimator only fits the late decay ``rt60`` comes from. ``c50_bands`` and
    ``c80_bands`` are then empty arrays (not computed), while ``edt_bands`` stays
    a full-length array of NaNs so it can be indexed by the same band index as
    ``rt60_bands``."""

    rt60: float
    edt: float
    c50: float
    c80: float
    d50: float
    rt60_bands: NDArray[np.float32]
    edt_bands: NDArray[np.float32]
    c50_bands: NDArray[np.float32]
    c80_bands: NDArray[np.float32]
    confidence: float
    is_blind: bool
    def __init__(
        self,
        rt60: float,
        edt: float,
        c50: float,
        c80: float,
        d50: float,
        rt60_bands: NDArray[np.float32],
        edt_bands: NDArray[np.float32],
        c50_bands: NDArray[np.float32],
        c80_bands: NDArray[np.float32],
        confidence: float,
        is_blind: bool,
    ) -> None: ...
    @property
    def rt60Bands(self) -> list[float]: ...
    @property
    def c50Bands(self) -> list[float]: ...
    @property
    def c80Bands(self) -> list[float]: ...

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
    audio: list[float]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        audio: list[float],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...
    @property
    def sampleRate(self) -> int: ...

class RoomMorphStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...
    @property
    def sampleRate(self) -> int: ...

class RirResult:
    rir: list[float]
    sample_rate: int
    diagnostics: list[RirDiagnostic]
    def __init__(
        self,
        rir: list[float],
        sample_rate: int,
        diagnostics: list[RirDiagnostic] = ...,
    ) -> None: ...
    @property
    def sampleRate(self) -> int: ...

class RoomEstimate:
    volume: float
    length_m: float
    width_m: float
    height_m: float
    drr_db: float
    confidence: float
    band_absorption: list[float]
    rt60_bands: list[float]
    def __init__(
        self,
        volume: float,
        length_m: float,
        width_m: float,
        height_m: float,
        drr_db: float,
        confidence: float,
        band_absorption: list[float],
        rt60_bands: list[float],
    ) -> None: ...
    @property
    def drrDb(self) -> float: ...
    @property
    def bandAbsorption(self) -> list[float]: ...
    @property
    def rt60Bands(self) -> list[float]: ...

MaterialPresetName: TypeAlias = Literal["none", "concrete", "wood", "curtain", "carpet", "glass"]
AcousticModeName: TypeAlias = Literal["auto", "blind", "impulse_response"]

class RoomGeometry(TypedDict):
    length_m: float
    width_m: float
    height_m: float
    source: tuple[float, float, float]
    listener: tuple[float, float, float]
    absorption: NotRequired[float]
    band_absorption: NotRequired[list[float]]

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

class LufsResult:
    integrated_lufs: float
    momentary_lufs: float
    short_term_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    loudness_range: float
    def __init__(
        self,
        integrated_lufs: float,
        momentary_lufs: float,
        short_term_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        loudness_range: float,
    ) -> None: ...

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

    pre_left: list[float]
    pre_right: list[float]
    post_left: list[float]
    post_right: list[float]
    band_gain_db: list[float]
    profile_db: list[float]
    last_auto_gain_db: float
    seq: int
    def __init__(
        self,
        pre_left: list[float],
        pre_right: list[float],
        post_left: list[float],
        post_right: list[float],
        band_gain_db: list[float],
        profile_db: list[float],
        last_auto_gain_db: float,
        seq: int,
    ) -> None: ...
    @property
    def preLeft(self) -> list[float]: ...
    @property
    def preRight(self) -> list[float]: ...
    @property
    def postLeft(self) -> list[float]: ...
    @property
    def postRight(self) -> list[float]: ...
    @property
    def bandGainDb(self) -> list[float]: ...
    @property
    def profileDb(self) -> list[float]: ...
    @property
    def lastAutoGainDb(self) -> float: ...

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
    loudness_range_db: float
    is_compressed: bool
    loudness_times_sec: NDArray[np.float32]
    loudness_rms_db: NDArray[np.float32]
    def __init__(
        self,
        dynamic_range_db: float,
        peak_db: float,
        rms_db: float,
        crest_factor: float,
        loudness_range_db: float,
        is_compressed: bool,
        loudness_times_sec: NDArray[np.float32],
        loudness_rms_db: NDArray[np.float32],
    ) -> None: ...

class ClickDetection:
    count: int
    rejected: int
    longest_run_samples: int
    per_second: float
    def __init__(
        self, count: int, rejected: int, longest_run_samples: int, per_second: float
    ) -> None: ...

class ClipDetection:
    sample_count: int
    sample_fraction: float
    run_count: int
    longest_run_samples: int
    flat_run_count: int
    longest_flat_run_samples: int
    flat_sample_count: int
    flat_level: float  # largest run level once the two highest runs are set aside; 0 if none
    def __init__(
        self,
        sample_count: int,
        sample_fraction: float,
        run_count: int,
        longest_run_samples: int,
        flat_run_count: int,
        longest_flat_run_samples: int,
        flat_sample_count: int,
        flat_level: float,
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
    window_rms_db: list[float]
    def __init__(
        self,
        dynamic_range_db: float,
        low_percentile_db: float,
        high_percentile_db: float,
        window_rms_db: list[float],
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
    channels: int
    bucket_count: int
    samples_per_bucket: int
    def __init__(
        self,
        min: NDArray[np.float32],
        max: NDArray[np.float32],
        channels: int,
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
    start: float
    end: float
    confidence: float
    bass: PitchClass | None
    name: str
    def __init__(
        self,
        root: PitchClass,
        quality: str,
        start: float,
        end: float,
        confidence: float,
        bass: PitchClass | None = None,
        name: str = "",
    ) -> None: ...
    @property
    def duration(self) -> float: ...
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
        start: float,
        end: float,
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
        start: float,
        end: float,
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
    harmonic: list[float]
    percussive: list[float]
    length: int
    sample_rate: int
    def __init__(
        self, harmonic: list[float], percussive: list[float], length: int, sample_rate: int
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
    left: list[float]
    right: list[float]
    length: int
    left_report: DeclickReport
    right_report: DeclickReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
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
    left: list[float]
    right: list[float]
    length: int
    left_report: DeclipReport
    right_report: DeclipReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
        length: int,
        left_report: DeclipReport,
        right_report: DeclipReport,
    ) -> None: ...

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
    left: list[float]
    right: list[float]
    length: int
    left_report: DecrackleReport
    right_report: DecrackleReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
        length: int,
        left_report: DecrackleReport,
        right_report: DecrackleReport,
    ) -> None: ...

class HumDetection:
    fundamental_hz: float
    # Linear energy ratio (not dB); 1.0 means no peak.
    fundamental_prominence: float
    harmonics: int
    harmonic_dbfs: list[float]
    def __init__(
        self,
        fundamental_hz: float,
        fundamental_prominence: float,
        harmonics: int,
        harmonic_dbfs: list[float],
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
    left: list[float]
    right: list[float]
    length: int
    left_report: DehumReport
    right_report: DehumReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
        length: int,
        left_report: DehumReport,
        right_report: DehumReport,
    ) -> None: ...

class NoiseDetection:
    floor_dbfs: float
    band_floor_dbfs: list[float]
    def __init__(
        self,
        floor_dbfs: float,
        band_floor_dbfs: list[float],
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
    left: list[float]
    right: list[float]
    length: int
    report: DenoiseReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
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

class ReverbDetection:
    late_decay_ratio_db: float
    late_predictability: float
    def __init__(
        self,
        late_decay_ratio_db: float,
        late_predictability: float,
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
    left: list[float]
    right: list[float]
    length: int
    report: DereverbReport
    def __init__(
        self,
        left: list[float],
        right: list[float],
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
    left: list[float]
    right: list[float]
    length: int
    report: TrimReport
    left_range: TrimRange
    right_range: TrimRange
    def __init__(
        self,
        left: list[float],
        right: list[float],
        length: int,
        report: TrimReport,
        left_range: TrimRange,
        right_range: TrimRange,
    ) -> None: ...

class NormalizeStereoResult:
    left: list[float]
    right: list[float]
    length: int
    applied_gain_db: float
    def __init__(
        self,
        left: list[float],
        right: list[float],
        length: int,
        applied_gain_db: float,
    ) -> None: ...

class MasteringResult:
    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    non_finite_substitution_count: int
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
        non_finite_substitution_count: int = 0,
    ) -> None: ...

class MasteringStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    latency_samples: int
    loudness_target_limited: bool
    non_finite_substitution_count: int
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        latency_samples: int = 0,
        loudness_target_limited: bool = False,
        non_finite_substitution_count: int = 0,
    ) -> None: ...

class LoudnessMatch:
    reference_lufs: float
    source_lufs: float
    applied_gain_db: float
    matched_true_peak_dbtp: float
    def __init__(
        self,
        reference_lufs: float,
        source_lufs: float,
        applied_gain_db: float,
        matched_true_peak_dbtp: float,
    ) -> None: ...

class StageGainReduction:
    stage: str
    gain_reduction_db: float
    def __init__(self, stage: str, gain_reduction_db: float) -> None: ...

class MasteringAudioProfileLoudness(TypedDict):
    integratedLufs: float
    lraLu: float
    truePeakDb: float
    crestFactorDb: float

class MasteringAudioProfileSpectral(TypedDict):
    subRmsDb: float
    lowRmsDb: float
    lowMidRmsDb: float
    midRmsDb: float
    highMidRmsDb: float
    highRmsDb: float
    airRmsDb: float
    centroidHz: float
    flatness: float
    rolloffHz: float

class MasteringAudioProfileDynamics(TypedDict):
    shortTermLufsStd: float
    attackDensity: float
    sustainRatio: float

class MasteringAudioProfileDefects(TypedDict):
    measured: bool
    clickCount: int
    clickRejected: int
    clickLongestRunSamples: int
    clickPerSecond: float
    crackleSampleCount: int
    crackleSampleFraction: float
    cracklePerSecond: float
    clipSampleCount: int
    clipRunCount: int
    clipLongestRunSamples: int
    clipSampleFraction: float
    clipFlatRunCount: int
    clipFlatSampleCount: int
    clipLongestFlatRunSamples: int
    clipFlatLevel: float
    noiseFloorDbfs: float
    noiseBandPeakDbfs: float
    noiseBandPeakIndex: int
    humFundamentalHz: float
    # Linear energy ratio over the median hum candidate (not dB); 1.0 means no peak.
    humFundamentalProminence: float
    humHarmonics: int
    humFundamentalDbfs: float
    humPeakHarmonicDbfs: float
    lateDecayRatioDb: float

class MasteringAudioProfile(TypedDict):
    durationSec: float
    bpm: float
    bpmConfidence: float
    loudness: MasteringAudioProfileLoudness
    spectral: MasteringAudioProfileSpectral
    dynamics: MasteringAudioProfileDynamics
    defects: MasteringAudioProfileDefects

class MasteringRepairAnalysis(TypedDict):
    defects: MasteringAudioProfileDefects
    channels: list[MasteringAudioProfileDefects]
    declipThresholdSafe: bool
    integratedLufs: float
    recommended: list[dict[str, Any]]
    explanation: list[str]

class MasteringChainConfigDocument(TypedDict):
    version: int
    params: dict[str, Any]

class MasteringAssistantProfile(TypedDict):
    durationSec: float
    bpm: float
    bpmConfidence: float
    integratedLufs: float | None
    lraLu: float
    truePeakDb: float
    crestFactorDb: float
    spectralCentroidHz: float
    spectralFlatness: float
    attackDensity: float
    sustainRatio: float

class MasteringAssistantResult(TypedDict):
    chainConfig: MasteringChainConfigDocument
    explanation: list[str]
    profile: MasteringAssistantProfile

class MasteringStreamingPreviewPlatform(TypedDict):
    name: str
    integratedLufs: float | None
    truePeakDb: float
    normalizationGainDb: float
    ceilingRisk: bool

class MasteringStreamingPreviewResult(TypedDict):
    platforms: list[MasteringStreamingPreviewPlatform]

class MatchReferenceLoudnessResult(TypedDict):
    sourceLufs: float | None
    referenceLufs: float | None
    gainToMatchDb: float | None

class MatchTonalBalanceBand(TypedDict):
    lowHz: float
    highHz: float
    sourceDb: float
    referenceDb: float
    deviationDb: float

class MatchTonalBalanceResult(TypedDict):
    bands: list[MatchTonalBalanceBand]

class MatchTonalBalanceLogBandsResult(TypedDict):
    bands: list[MatchTonalBalanceBand]

class MatchEqCurveResult(TypedDict):
    frequencies: list[float]
    gainDb: list[float]

class MatchEstimateReferenceDelaySamplesResult(TypedDict):
    delaySamples: float

class StereoMonoCompatCheckResult(TypedDict):
    correlation: float
    width: float | None
    monoPeak: float
    sideRms: float
    likelyMonoCompatible: bool

class StereoMonoCompatBand(TypedDict):
    lowHz: float
    highHz: float
    correlation: float
    sideRms: float

class StereoMonoCompatCheckLogBandsResult(TypedDict):
    bands: list[StereoMonoCompatBand]

class StreamingLoudnessGain:
    loudness_static_gain_db: float
    true_peak_db: float
    integrated_lufs: float
    def __init__(
        self, loudness_static_gain_db: float, true_peak_db: float, integrated_lufs: float
    ) -> None: ...

class MasteringLoudnessSummary:
    integrated_lufs: float
    max_momentary_lufs: float
    max_short_term_lufs: float
    true_peak_dbtp: float
    loudness_range: float
    def __init__(
        self,
        integrated_lufs: float,
        max_momentary_lufs: float,
        max_short_term_lufs: float,
        true_peak_dbtp: float,
        loudness_range: float,
    ) -> None: ...

class MasteringReport:
    before: MasteringLoudnessSummary
    after: MasteringLoudnessSummary
    applied_gain_db: float
    max_gain_reduction_db: float
    loudness_target_limited: bool
    band_energy_delta_db: list[float]
    def __init__(
        self,
        before: MasteringLoudnessSummary,
        after: MasteringLoudnessSummary,
        applied_gain_db: float,
        max_gain_reduction_db: float,
        loudness_target_limited: bool,
        band_energy_delta_db: list[float] = ...,
    ) -> None: ...

class MasteringChainResult:
    samples: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_lra: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    non_finite_substitution_count: int
    def __init__(
        self,
        samples: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_lra: float = ...,
        loudness_target_limited: bool = ...,
        stage_gain_reductions: list[StageGainReduction] = ...,
        report: MasteringReport | None = ...,
        non_finite_substitution_count: int = ...,
    ) -> None: ...

class MasteringChainStereoResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    input_lufs: float
    output_lufs: float
    applied_gain_db: float
    stages: list[str]
    output_true_peak_dbtp: float
    output_lra: float
    loudness_target_limited: bool
    stage_gain_reductions: list[StageGainReduction]
    report: MasteringReport | None
    non_finite_substitution_count: int
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        input_lufs: float,
        output_lufs: float,
        applied_gain_db: float,
        stages: list[str],
        output_true_peak_dbtp: float = ...,
        output_lra: float = ...,
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
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
    seq: int
    channel_count: int
    peak_db: tuple[float, ...]
    rms_db: tuple[float, ...]
    true_peak_db: tuple[float, ...]
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
        true_peak_db_l: float,
        true_peak_db_r: float,
        max_true_peak_db: float,
        seq: int,
        channel_count: int = ...,
        peak_db: tuple[float, ...] = ...,
        rms_db: tuple[float, ...] = ...,
        true_peak_db: tuple[float, ...] = ...,
    ) -> None: ...

class GoniometerPoint:
    left: float
    right: float
    def __init__(self, left: float, right: float) -> None: ...

class MixResult:
    left: list[float]
    right: list[float]
    sample_rate: int
    meters: list[MixMeterSnapshot]
    def __init__(
        self,
        left: list[float],
        right: list[float],
        sample_rate: int,
        meters: list[MixMeterSnapshot],
    ) -> None: ...

class EngineTelemetry:
    type: EngineTelemetryType
    error: EngineTelemetryError
    render_frame: int
    timeline_sample: int
    audible_timeline_sample: int
    graph_latency_samples_q8: int
    value: int
    def __init__(
        self,
        type: EngineTelemetryType,
        error: EngineTelemetryError,
        render_frame: int,
        timeline_sample: int,
        audible_timeline_sample: int,
        graph_latency_samples_q8: int,
        value: int,
    ) -> None: ...
    @property
    def renderFrame(self) -> int: ...
    @property
    def timelineSample(self) -> int: ...
    @property
    def audibleTimelineSample(self) -> int: ...
    @property
    def graphLatencySamplesQ8(self) -> int: ...

class EngineTrackMonitorMode(IntEnum):
    OFF = 0
    PFL = 1
    AFL = 2

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
        curve_to_next: AutomationCurve = AutomationCurve.LINEAR,
    ) -> None: ...

class MarkerKind(IntEnum):
    MARKER = 0
    TEXT = 1
    LYRIC = 2
    CUE_POINT = 3
    KEY_SIGNATURE = 4

class SidechainCheck:
    ok: bool
    reason: str | None
    def __init__(self, ok: bool, reason: str | None = None) -> None: ...

class EngineMarker:
    id: int
    ppq: float
    name: str
    kind: int
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: int = 0,
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectMarker:
    id: int
    ppq: float
    name: str
    kind: int
    key_fifths: int
    key_minor: bool
    def __init__(
        self,
        id: int,
        ppq: float,
        name: str = "",
        kind: int = 0,
        key_fifths: int = 0,
        key_minor: bool = False,
    ) -> None: ...

class ProjectTrack:
    id: int
    kind: int
    midi_destination_id: int
    gain: float
    pan: float
    mute: bool
    solo: bool
    name: str
    def __init__(
        self,
        id: int,
        kind: int,
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
    source_kind: int
    start_ppq: float
    length_ppq: float
    source_offset_ppq: float
    gain: float
    loop_mode: int
    loop_length_ppq: float
    def __init__(
        self,
        id: int,
        track_id: int,
        source_id: int,
        source_kind: int,
        start_ppq: float,
        length_ppq: float,
        source_offset_ppq: float,
        gain: float,
        loop_mode: int,
        loop_length_ppq: float,
    ) -> None: ...

class ProjectSource:
    id: int
    kind: int
    channel_count: int
    storage_handle_id: int
    sample_rate_hint: float
    name_or_uri: str
    content_hash: str
    external_stem_role: str
    def __init__(
        self,
        id: int,
        kind: int,
        channel_count: int,
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
    click_seconds: float
    def __init__(
        self,
        enabled: bool = False,
        beat_gain: float = 0.35,
        accent_gain: float = 0.7,
        click_samples: int = 0,
        click_seconds: float = 0.0,
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
    warp_mode: str | int
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
        warp_mode: str | int = "off",
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
    source: str
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
    block_size: int
    num_channels: int
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
        block_size: int = 128,
        num_channels: int = 2,
        target_sample_rate: int = 0,
        source_sample_rate: int = 0,
        normalize_lufs: bool = False,
        target_lufs: float = -14.0,
        dither: int = 0,
        dither_bits: int = 16,
        dither_seed: int = 0,
    ) -> None: ...

class EngineBounceResult:
    interleaved: np.ndarray
    frames: int
    num_channels: int
    sample_rate: int
    integrated_lufs: float
    def __init__(
        self,
        interleaved: list[float],
        frames: int,
        num_channels: int,
        sample_rate: int,
        integrated_lufs: float,
    ) -> None: ...

class EngineFreezeOptions:
    total_frames: int
    block_size: int
    num_channels: int
    clip_id: int
    start_ppq: float
    gain: float
    def __init__(
        self,
        total_frames: int,
        block_size: int = 128,
        num_channels: int = 2,
        clip_id: int = 1,
        start_ppq: float = 0.0,
        gain: float = 1.0,
    ) -> None: ...

class EngineFreezeResult:
    clip_id: int
    frames: int
    num_channels: int
    def __init__(self, clip_id: int, frames: int, num_channels: int) -> None: ...

class EngineGraphNodeType(IntEnum):
    PASS_THROUGH = 0
    GAIN = 1

class EngineGraphMix(IntEnum):
    """Mixing intent for a graph edge.

    Edges into one destination port are taken in connection order. ``REPLACE``
    overwrites what earlier edges put on the port; ``ADD`` accumulates onto it.
    A port's first edge initializes it either way, so the two differ only on a
    shared port. An omitted ``mix`` is ``ADD``, unlike a zero-initialized C
    connection, which is ``REPLACE``.
    """

    REPLACE = 0
    ADD = 1

class EngineGraphNode:
    id: str
    type: EngineGraphNodeType
    gain_db: float
    num_ports: int
    def __init__(
        self,
        id: str,
        type: EngineGraphNodeType = EngineGraphNodeType.PASS_THROUGH,
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
        mix: EngineGraphMix = EngineGraphMix.ADD,
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
    true_peak_db_l: float
    true_peak_db_r: float
    max_true_peak_db: float
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
        true_peak_db_l: float,
        true_peak_db_r: float,
        max_true_peak_db: float,
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
    channel_count: int
    peak_db: list[float]
    rms_db: list[float]
    true_peak_db: list[float]
    max_true_peak_db: float
    correlation: float
    mono_compat_width: float
    momentary_lufs: float
    short_term_lufs: float
    integrated_lufs: float
    gain_reduction_db: float
    dropped_records: int
    input_peak_db: list[float]
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        channel_count: int,
        peak_db: list[float],
        rms_db: list[float],
        true_peak_db: list[float],
        max_true_peak_db: float,
        correlation: float,
        mono_compat_width: float,
        momentary_lufs: float,
        short_term_lufs: float,
        integrated_lufs: float,
        gain_reduction_db: float,
        dropped_records: int,
        input_peak_db: list[float] = ...,
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
    bands: list[float]
    points: list[tuple[float, float]]
    def __init__(
        self,
        target_id: int,
        render_frame: int,
        seq: int,
        dropped_records: int,
        bands: list[float],
        points: list[tuple[float, float]],
    ) -> None: ...

class ExternalMidiEvent:
    destination_id: int
    render_frame: int
    bytes: builtins.bytes
    def __init__(
        self,
        destination_id: int,
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
    start: float
    end: float
    energy_level: float  # median frame RMS in the section over the loudest section's, [0, 1]
    confidence: float
    group: int  # sections sharing a value repeat the same material; 0, 1, ... by first appearance
    name: str
    def __init__(
        self,
        type: SectionType,
        start: float,
        end: float,
        energy_level: float,
        confidence: float,
        group: int = 0,
        name: str = "",
    ) -> None: ...

class Boundary:
    time: float
    frame: int
    strength: float
    def __init__(self, time: float, frame: int, strength: float) -> None: ...

class BoundaryResult:
    boundaries: list[Boundary]
    novelty_curve: NDArray[np.float32]
    novelty_peak: float
    sample_rate: int
    hop_length: int
    frame_stride: int
    def __init__(
        self,
        boundaries: list[Boundary],
        novelty_curve: NDArray[np.float32],
        novelty_peak: float,
        sample_rate: int,
        hop_length: int,
        frame_stride: int,
    ) -> None: ...

class MelodyPoint:
    time: float
    frequency: float
    confidence: float
    def __init__(self, time: float, frequency: float, confidence: float) -> None: ...

class MelodyResult:
    points: list[MelodyPoint]
    pitch_range_octaves: float
    pitch_stability: float
    mean_frequency: float
    vibrato_rate: float
    def __init__(
        self,
        points: list[MelodyPoint],
        pitch_range_octaves: float,
        pitch_stability: float,
        mean_frequency: float,
        vibrato_rate: float,
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

class QuantizeConfig:
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

class StreamConfig:
    sample_rate: int
    # Window in samples at 44100 Hz; rescaled below that rate, never shorter than hop_length.
    n_fft: int
    hop_length: int
    n_mels: int
    fmin: float
    fmax: float
    reference_hz: float
    compute_magnitude: bool
    compute_mel: bool
    compute_chroma: bool
    compute_onset: bool
    compute_spectral: bool
    emit_every_n_frames: int
    magnitude_downsample: int
    max_pending_frames: int  # Overflow drops the newly produced frame.
    max_progression_entries: int
    key_update_interval_sec: float
    bpm_update_interval_sec: float
    window: int
    output_format: int  # Deprecated compatibility field; must be 0 (Float32).
    def __init__(
        self,
        sample_rate: int = 44100,
        n_fft: int = 2048,
        hop_length: int = 512,
        n_mels: int = 128,
        fmin: float = 0.0,
        fmax: float = 0.0,
        reference_hz: float = 440.0,
        compute_magnitude: bool = False,
        compute_mel: bool = True,
        compute_chroma: bool = True,
        compute_onset: bool = True,
        compute_spectral: bool = True,
        emit_every_n_frames: int = 1,
        magnitude_downsample: int = 1,
        max_pending_frames: int = 4096,
        max_progression_entries: int = 4096,
        key_update_interval_sec: float = 5.0,
        bpm_update_interval_sec: float = 10.0,
        window: int = 0,
        output_format: int = 0,
    ) -> None: ...

class StreamFrames:
    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps_sec: list[float]
    mel: list[float]
    chroma: list[float]
    onset_strength: list[float]
    rms_energy: list[float]
    spectral_centroid: list[float]
    spectral_flatness: list[float]
    chord_root: list[int]
    chord_quality: list[int]
    chord_confidence: list[float]
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps_sec: list[float],
        mel: list[float],
        chroma: list[float],
        onset_strength: list[float],
        rms_energy: list[float],
        spectral_centroid: list[float],
        spectral_flatness: list[float],
        chord_root: list[int],
        chord_quality: list[int],
        chord_confidence: list[float],
    ) -> None: ...

class StreamFramesU8:
    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps_sec: list[float]
    mel: list[int]
    chroma: list[int]
    onset_strength: list[int]
    rms_energy: list[int]
    spectral_centroid: list[int]
    spectral_flatness: list[int]
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps_sec: list[float],
        mel: list[int],
        chroma: list[int],
        onset_strength: list[int],
        rms_energy: list[int],
        spectral_centroid: list[int],
        spectral_flatness: list[int],
    ) -> None: ...

class StreamFramesI16:
    n_frames: int
    n_mels: int
    n_chroma: int
    feature_flags: int
    timestamps_sec: list[float]
    mel: list[int]
    chroma: list[int]
    onset_strength: list[int]
    rms_energy: list[int]
    spectral_centroid: list[int]
    spectral_flatness: list[int]
    def __init__(
        self,
        n_frames: int,
        n_mels: int,
        n_chroma: int,
        feature_flags: int,
        timestamps_sec: list[float],
        mel: list[int],
        chroma: list[int],
        onset_strength: list[int],
        rms_energy: list[int],
        spectral_centroid: list[int],
        spectral_flatness: list[int],
    ) -> None: ...

class StreamChordChange:
    root: int
    quality: int
    name: str
    start_sec: float
    confidence: float
    def __init__(
        self, root: int, quality: int, name: str, start_sec: float, confidence: float
    ) -> None: ...

class StreamBarChord:
    """``bar_index`` is the bar number, not this entry's position in the list:
    bars with no confident chord are not recorded and the oldest entries are
    dropped at the history cap, so group bars by pattern position with
    ``bar_index``. ``start_sec`` is on the same timeline as
    ``StreamFrame.timestamp`` and consecutive bars are ``bar_duration_sec`` apart.
    In ``voted_pattern``, ``bar_index`` is the pattern position and
    ``start_sec`` is unused."""

    bar_index: int
    root: int
    quality: int
    name: str
    start_sec: float
    confidence: float
    def __init__(
        self,
        bar_index: int,
        root: int,
        quality: int,
        name: str,
        start_sec: float,
        confidence: float,
    ) -> None: ...

class StreamPatternScore:
    name: str
    score: float
    def __init__(self, name: str, score: float) -> None: ...

class StreamStats:
    """``bpm_candidate_count`` is the number of tempo candidates the most recent
    BPM estimate chose from, 0 until one has run — the same quantity as
    ``AnalysisResult``'s field of the same name. ``updated`` is true when the
    key or BPM was re-estimated since the previous snapshot: one change sets it
    on exactly one snapshot however the caller chunks its input."""

    total_frames: int
    total_samples: int
    duration_sec: float
    pending_frames: int
    dropped_output_frames: int
    dropped_chord_progression_entries: int
    dropped_bar_progression_entries: int
    non_finite_discard_blocks: int
    bpm: float
    bpm_confidence: float
    bpm_candidate_count: int
    key: int
    key_minor: bool
    key_confidence: float
    key_name: str | None
    key_short_name: str | None
    chord_root: int
    chord_quality: int
    chord_name: str
    chord_confidence: float
    chord_start_sec: float
    current_bar: int
    bar_duration_sec: float
    chord_progression: list[StreamChordChange]
    bar_chord_progression: list[StreamBarChord]
    voted_pattern: list[StreamBarChord]
    pattern_length: int
    detected_pattern_name: str
    detected_pattern_score: float
    all_pattern_scores: list[StreamPatternScore]
    accumulated_sec: float
    used_frames: int
    updated: bool
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
        bpm: float,
        bpm_confidence: float,
        bpm_candidate_count: int,
        key: int,
        key_minor: bool,
        key_confidence: float,
        key_name: str | None,
        key_short_name: str | None,
        chord_root: int,
        chord_quality: int,
        chord_name: str,
        chord_confidence: float,
        chord_start_sec: float,
        current_bar: int,
        bar_duration_sec: float,
        chord_progression: list[StreamChordChange],
        bar_chord_progression: list[StreamBarChord],
        voted_pattern: list[StreamBarChord],
        pattern_length: int,
        detected_pattern_name: str,
        detected_pattern_score: float,
        all_pattern_scores: list[StreamPatternScore],
        accumulated_sec: float,
        used_frames: int,
        updated: bool,
    ) -> None: ...
