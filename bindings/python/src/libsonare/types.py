"""Stable public facade for libsonare value types.

Definitions are grouped into analysis, engine, and streaming domains.  Public
names are imported explicitly so static analyzers and IDEs retain the same API
surface as the former monolithic module.
"""

from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._types_analysis import (
    AcousticResult as AcousticResult,
)
from ._types_analysis import (
    AnalysisDynamics as AnalysisDynamics,
)
from ._types_analysis import (
    AnalysisMelody as AnalysisMelody,
)
from ._types_analysis import (
    AnalysisResult as AnalysisResult,
)
from ._types_analysis import (
    AnalysisRhythm as AnalysisRhythm,
)
from ._types_analysis import (
    AnalysisTimbre as AnalysisTimbre,
)
from ._types_analysis import (
    AutomationCurve as AutomationCurve,
)
from ._types_analysis import (
    Beat as Beat,
)
from ._types_analysis import (
    BpmAnalysisResult as BpmAnalysisResult,
)
from ._types_analysis import (
    BpmCandidate as BpmCandidate,
)
from ._types_analysis import (
    BpmHypothesis as BpmHypothesis,
)
from ._types_analysis import (
    Capabilities as Capabilities,
)
from ._types_analysis import (
    CapabilitiesAbi as CapabilitiesAbi,
)
from ._types_analysis import (
    CapabilitiesDecode as CapabilitiesDecode,
)
from ._types_analysis import (
    CapabilitiesFeatures as CapabilitiesFeatures,
)
from ._types_analysis import (
    CapabilityCatalog as CapabilityCatalog,
)
from ._types_analysis import (
    CapabilityCatalogPresets as CapabilityCatalogPresets,
)
from ._types_analysis import (
    ChannelLayout as ChannelLayout,
)
from ._types_analysis import (
    Chord as Chord,
)
from ._types_analysis import (
    ChordAnalysisResult as ChordAnalysisResult,
)
from ._types_analysis import (
    ChromaResult as ChromaResult,
)
from ._types_analysis import (
    ClippingRegion as ClippingRegion,
)
from ._types_analysis import (
    ClippingReport as ClippingReport,
)
from ._types_analysis import (
    DynamicRangeReport as DynamicRangeReport,
)
from ._types_analysis import (
    DynamicsResult as DynamicsResult,
)
from ._types_analysis import (
    EngineTelemetryError as EngineTelemetryError,
)
from ._types_analysis import (
    EngineTelemetryType as EngineTelemetryType,
)
from ._types_analysis import (
    EqSpectrumSnapshot as EqSpectrumSnapshot,
)
from ._types_analysis import (
    HpssResult as HpssResult,
)
from ._types_analysis import (
    Key as Key,
)
from ._types_analysis import (
    KeyCandidate as KeyCandidate,
)
from ._types_analysis import (
    KeyProfile as KeyProfile,
)
from ._types_analysis import (
    LufsResult as LufsResult,
)
from ._types_analysis import (
    MasteringChainResult as MasteringChainResult,
)
from ._types_analysis import (
    MasteringChainStereoResult as MasteringChainStereoResult,
)
from ._types_analysis import (
    MasteringChannelPolicy as MasteringChannelPolicy,
)
from ._types_analysis import (
    MasteringInsertParamInfo as MasteringInsertParamInfo,
)
from ._types_analysis import (
    MasteringLoudnessSummary as MasteringLoudnessSummary,
)
from ._types_analysis import (
    MasteringProcessorCatalogEntry as MasteringProcessorCatalogEntry,
)
from ._types_analysis import (
    MasteringProcessorCategory as MasteringProcessorCategory,
)
from ._types_analysis import (
    MasteringProcessorKind as MasteringProcessorKind,
)
from ._types_analysis import (
    MasteringReport as MasteringReport,
)
from ._types_analysis import (
    MasteringResult as MasteringResult,
)
from ._types_analysis import (
    MasteringStereoResult as MasteringStereoResult,
)
from ._types_analysis import (
    MelSpectrogramResult as MelSpectrogramResult,
)
from ._types_analysis import (
    MeterTap as MeterTap,
)
from ._types_analysis import (
    MfccResult as MfccResult,
)
from ._types_analysis import (
    Mode as Mode,
)
from ._types_analysis import (
    NoteSegment as NoteSegment,
)
from ._types_analysis import (
    PanLaw as PanLaw,
)
from ._types_analysis import (
    PhaseScopeReport as PhaseScopeReport,
)
from ._types_analysis import (
    PiptrackResult as PiptrackResult,
)
from ._types_analysis import (
    PitchClass as PitchClass,
)
from ._types_analysis import (
    PitchResult as PitchResult,
)
from ._types_analysis import (
    ReassignedSpectrogramResult as ReassignedSpectrogramResult,
)
from ._types_analysis import (
    RhythmResult as RhythmResult,
)
from ._types_analysis import (
    RirResult as RirResult,
)
from ._types_analysis import (
    RoomEstimate as RoomEstimate,
)
from ._types_analysis import (
    SectionType as SectionType,
)
from ._types_analysis import (
    SegmentMatrix as SegmentMatrix,
)
from ._types_analysis import (
    SendTiming as SendTiming,
)
from ._types_analysis import (
    SpectrumReport as SpectrumReport,
)
from ._types_analysis import (
    StageGainReduction as StageGainReduction,
)
from ._types_analysis import (
    StftResult as StftResult,
)
from ._types_analysis import (
    TimbreFrame as TimbreFrame,
)
from ._types_analysis import (
    TimbreResult as TimbreResult,
)
from ._types_analysis import (
    TimeSignature as TimeSignature,
)
from ._types_analysis import (
    VectorscopeReport as VectorscopeReport,
)
from ._types_analysis import (
    WaveformPeaksReport as WaveformPeaksReport,
)
from ._types_engine import (
    AutomationPoint as AutomationPoint,
)
from ._types_engine import (
    ClipPageRequest as ClipPageRequest,
)
from ._types_engine import (
    EngineBounceOptions as EngineBounceOptions,
)
from ._types_engine import (
    EngineBounceResult as EngineBounceResult,
)
from ._types_engine import (
    EngineCaptureStatus as EngineCaptureStatus,
)
from ._types_engine import (
    EngineClip as EngineClip,
)
from ._types_engine import (
    EngineFreezeOptions as EngineFreezeOptions,
)
from ._types_engine import (
    EngineFreezeResult as EngineFreezeResult,
)
from ._types_engine import (
    EngineGraphConnection as EngineGraphConnection,
)
from ._types_engine import (
    EngineGraphMix as EngineGraphMix,
)
from ._types_engine import (
    EngineGraphNode as EngineGraphNode,
)
from ._types_engine import (
    EngineGraphNodeType as EngineGraphNodeType,
)
from ._types_engine import (
    EngineGraphParameterBinding as EngineGraphParameterBinding,
)
from ._types_engine import (
    EngineGraphSpec as EngineGraphSpec,
)
from ._types_engine import (
    EngineMarker as EngineMarker,
)
from ._types_engine import (
    EngineMetronomeConfig as EngineMetronomeConfig,
)
from ._types_engine import (
    EngineMidiClipSchedule as EngineMidiClipSchedule,
)
from ._types_engine import (
    EngineMidiEvent as EngineMidiEvent,
)
from ._types_engine import (
    EngineTelemetry as EngineTelemetry,
)
from ._types_engine import (
    EngineTrackMonitorMode as EngineTrackMonitorMode,
)
from ._types_engine import (
    ExternalMidiEvent as ExternalMidiEvent,
)
from ._types_engine import (
    GoniometerPoint as GoniometerPoint,
)
from ._types_engine import (
    MarkerKind as MarkerKind,
)
from ._types_engine import (
    MelodyPoint as MelodyPoint,
)
from ._types_engine import (
    MelodyResult as MelodyResult,
)
from ._types_engine import (
    MeterTelemetryRecord as MeterTelemetryRecord,
)
from ._types_engine import (
    MeterTelemetryRecordWide as MeterTelemetryRecordWide,
)
from ._types_engine import (
    MixMeterSnapshot as MixMeterSnapshot,
)
from ._types_engine import (
    MixResult as MixResult,
)
from ._types_engine import (
    ParameterInfo as ParameterInfo,
)
from ._types_engine import (
    ProjectClip as ProjectClip,
)
from ._types_engine import (
    ProjectMarker as ProjectMarker,
)
from ._types_engine import (
    ProjectSource as ProjectSource,
)
from ._types_engine import (
    ProjectTrack as ProjectTrack,
)
from ._types_engine import (
    ScopeTelemetryRecord as ScopeTelemetryRecord,
)
from ._types_engine import (
    Section as Section,
)
from ._types_engine import (
    SectionResult as SectionResult,
)
from ._types_engine import (
    TransportState as TransportState,
)
from ._types_streaming import (
    CqtResult as CqtResult,
)
from ._types_streaming import (
    InverseResult as InverseResult,
)
from ._types_streaming import (
    QuantizeConfig as QuantizeConfig,
)
from ._types_streaming import (
    StreamBarChord as StreamBarChord,
)
from ._types_streaming import (
    StreamChordChange as StreamChordChange,
)
from ._types_streaming import (
    StreamConfig as StreamConfig,
)
from ._types_streaming import (
    StreamFrames as StreamFrames,
)
from ._types_streaming import (
    StreamFramesI16 as StreamFramesI16,
)
from ._types_streaming import (
    StreamFramesU8 as StreamFramesU8,
)
from ._types_streaming import (
    StreamPatternScore as StreamPatternScore,
)
from ._types_streaming import (
    StreamStats as StreamStats,
)

_rebind_facade_exports(globals(), "libsonare._types_")
del _rebind_facade_exports
