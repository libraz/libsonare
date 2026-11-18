"""Stable public facade for libsonare value types.

Definitions live in one module per domain -- analysis, acoustics, repair,
mastering, engine and streaming, plus the shared enumerations and the
capability shapes.  Public names are imported explicitly so static analyzers
and IDEs retain the same API surface as the former monolithic module.
"""

from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._types_acoustic import (
    AcousticModeName as AcousticModeName,
)
from ._types_acoustic import (
    AcousticResult as AcousticResult,
)
from ._types_acoustic import (
    DereverbClassicalConfig as DereverbClassicalConfig,
)
from ._types_acoustic import (
    MaterialPresetName as MaterialPresetName,
)
from ._types_acoustic import (
    RirDiagnostic as RirDiagnostic,
)
from ._types_acoustic import (
    RirResult as RirResult,
)
from ._types_acoustic import (
    RoomEstimate as RoomEstimate,
)
from ._types_acoustic import (
    RoomGeometry as RoomGeometry,
)
from ._types_acoustic import (
    RoomMorphResult as RoomMorphResult,
)
from ._types_acoustic import (
    RoomMorphStereoResult as RoomMorphStereoResult,
)
from ._types_analysis import (
    CHORD_QUALITIES as CHORD_QUALITIES,
)
from ._types_analysis import (
    AnalysisBpmCandidate as AnalysisBpmCandidate,
)
from ._types_analysis import (
    AnalysisChord as AnalysisChord,
)
from ._types_analysis import (
    AnalysisDynamics as AnalysisDynamics,
)
from ._types_analysis import (
    AnalysisResult as AnalysisResult,
)
from ._types_analysis import (
    AnalysisRhythm as AnalysisRhythm,
)
from ._types_analysis import (
    AutoTuneResult as AutoTuneResult,
)
from ._types_analysis import (
    Beat as Beat,
)
from ._types_analysis import (
    BeatObservations as BeatObservations,
)
from ._types_analysis import (
    BpmAnalysisResult as BpmAnalysisResult,
)
from ._types_analysis import (
    BpmCandidate as BpmCandidate,
)
from ._types_analysis import (
    BpmCandidateRelation as BpmCandidateRelation,
)
from ._types_analysis import (
    Chord as Chord,
)
from ._types_analysis import (
    ChordAnalysisResult as ChordAnalysisResult,
)
from ._types_analysis import (
    ChordFunctionName as ChordFunctionName,
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
    EqSpectrumSnapshot as EqSpectrumSnapshot,
)
from ._types_analysis import (
    FunctionalChord as FunctionalChord,
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
    KeyDetection as KeyDetection,
)
from ._types_analysis import (
    LufsResult as LufsResult,
)
from ._types_analysis import (
    LufsSeriesResult as LufsSeriesResult,
)
from ._types_analysis import (
    MelSpectrogramResult as MelSpectrogramResult,
)
from ._types_analysis import (
    MeterEstimate as MeterEstimate,
)
from ._types_analysis import (
    MfccResult as MfccResult,
)
from ._types_analysis import (
    NormalizeStereoResult as NormalizeStereoResult,
)
from ._types_analysis import (
    NoteSegment as NoteSegment,
)
from ._types_analysis import (
    PhaseScopeReport as PhaseScopeReport,
)
from ._types_analysis import (
    PiptrackResult as PiptrackResult,
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
    SpectrumReport as SpectrumReport,
)
from ._types_analysis import (
    StereoBlock as StereoBlock,
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
    TrimRange as TrimRange,
)
from ._types_analysis import (
    TrimReport as TrimReport,
)
from ._types_analysis import (
    TrimSilenceResult as TrimSilenceResult,
)
from ._types_analysis import (
    TrimSilenceStereoResult as TrimSilenceStereoResult,
)
from ._types_analysis import (
    VectorscopeReport as VectorscopeReport,
)
from ._types_analysis import (
    WaveformPeaksReport as WaveformPeaksReport,
)
from ._types_capabilities import (
    Capabilities as Capabilities,
)
from ._types_capabilities import (
    CapabilitiesAbi as CapabilitiesAbi,
)
from ._types_capabilities import (
    CapabilitiesDecode as CapabilitiesDecode,
)
from ._types_capabilities import (
    CapabilitiesFeatures as CapabilitiesFeatures,
)
from ._types_capabilities import (
    CapabilityCatalog as CapabilityCatalog,
)
from ._types_capabilities import (
    CapabilityCatalogChain as CapabilityCatalogChain,
)
from ._types_capabilities import (
    CapabilityCatalogChainList as CapabilityCatalogChainList,
)
from ._types_capabilities import (
    CapabilityCatalogChainStage as CapabilityCatalogChainStage,
)
from ._types_capabilities import (
    CapabilityCatalogPresets as CapabilityCatalogPresets,
)
from ._types_capabilities import (
    MasteringAmpPresetCatalogEntry as MasteringAmpPresetCatalogEntry,
)
from ._types_capabilities import (
    MasteringChannelPolicy as MasteringChannelPolicy,
)
from ._types_capabilities import (
    MasteringInsertParamChoice as MasteringInsertParamChoice,
)
from ._types_capabilities import (
    MasteringInsertParamDependency as MasteringInsertParamDependency,
)
from ._types_capabilities import (
    MasteringInsertParamInfo as MasteringInsertParamInfo,
)
from ._types_capabilities import (
    MasteringInsertParamRelation as MasteringInsertParamRelation,
)
from ._types_capabilities import (
    MasteringInsertParamScale as MasteringInsertParamScale,
)
from ._types_capabilities import (
    MasteringInsertParamUnit as MasteringInsertParamUnit,
)
from ._types_capabilities import (
    MasteringInsertSlot as MasteringInsertSlot,
)
from ._types_capabilities import (
    MasteringInsertTiming as MasteringInsertTiming,
)
from ._types_capabilities import (
    MasteringPresetCatalogEntry as MasteringPresetCatalogEntry,
)
from ._types_capabilities import (
    MasteringPresetKind as MasteringPresetKind,
)
from ._types_capabilities import (
    MasteringProcessorCatalogEntry as MasteringProcessorCatalogEntry,
)
from ._types_capabilities import (
    MasteringProcessorCategory as MasteringProcessorCategory,
)
from ._types_capabilities import (
    MasteringProcessorKind as MasteringProcessorKind,
)
from ._types_capabilities import (
    SynthParamCondition as SynthParamCondition,
)
from ._types_capabilities import (
    SynthParamInfo as SynthParamInfo,
)
from ._types_engine import (
    AutomationPoint as AutomationPoint,
)
from ._types_engine import (
    Boundary as Boundary,
)
from ._types_engine import (
    BoundaryResult as BoundaryResult,
)
from ._types_engine import (
    CaptureInputMonitor as CaptureInputMonitor,
)
from ._types_engine import (
    CapturePunch as CapturePunch,
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
    EngineCaptureSource as EngineCaptureSource,
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
    EngineProcessWithMonitorResult as EngineProcessWithMonitorResult,
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
    MidiFxConfig as MidiFxConfig,
)
from ._types_engine import (
    MixMeterSnapshot as MixMeterSnapshot,
)
from ._types_engine import (
    MixResult as MixResult,
)
from ._types_engine import (
    MixSceneBusInput as MixSceneBusInput,
)
from ._types_engine import (
    MixSceneDocument as MixSceneDocument,
)
from ._types_engine import (
    MixSceneStripInput as MixSceneStripInput,
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
    SidechainCheck as SidechainCheck,
)
from ._types_engine import (
    SidechainRefusal as SidechainRefusal,
)
from ._types_engine import (
    SidechainSource as SidechainSource,
)
from ._types_engine import (
    TransportState as TransportState,
)
from ._types_enums import (
    AutomationCurve as AutomationCurve,
)
from ._types_enums import (
    ChannelLayout as ChannelLayout,
)
from ._types_enums import (
    ChordChromaMethod as ChordChromaMethod,
)
from ._types_enums import (
    CompressorDetector as CompressorDetector,
)
from ._types_enums import (
    DecrackleMode as DecrackleMode,
)
from ._types_enums import (
    DehumMode as DehumMode,
)
from ._types_enums import (
    DenoiseClassicalMode as DenoiseClassicalMode,
)
from ._types_enums import (
    DenoiseClassicalNoiseEstimator as DenoiseClassicalNoiseEstimator,
)
from ._types_enums import (
    EngineTelemetryError as EngineTelemetryError,
)
from ._types_enums import (
    EngineTelemetryType as EngineTelemetryType,
)
from ._types_enums import (
    EqPhaseMode as EqPhaseMode,
)
from ._types_enums import (
    EqStereoPlacement as EqStereoPlacement,
)
from ._types_enums import (
    GenreHint as GenreHint,
)
from ._types_enums import (
    KeyMode as KeyMode,
)
from ._types_enums import (
    KeyProfile as KeyProfile,
)
from ._types_enums import (
    MeterTap as MeterTap,
)
from ._types_enums import (
    MixBusRole as MixBusRole,
)
from ._types_enums import (
    NmfInit as NmfInit,
)
from ._types_enums import (
    NnFilterAggregate as NnFilterAggregate,
)
from ._types_enums import (
    NormType as NormType,
)
from ._types_enums import (
    PanLaw as PanLaw,
)
from ._types_enums import (
    PanMode as PanMode,
)
from ._types_enums import (
    PitchClass as PitchClass,
)
from ._types_enums import (
    PitchTargetMode as PitchTargetMode,
)
from ._types_enums import (
    ProjectWarpMode as ProjectWarpMode,
)
from ._types_enums import (
    SectionType as SectionType,
)
from ._types_enums import (
    SendTiming as SendTiming,
)
from ._types_enums import (
    SidechainSourceKind as SidechainSourceKind,
)
from ._types_enums import (
    SpectralEditMode as SpectralEditMode,
)
from ._types_enums import (
    TempogramMode as TempogramMode,
)
from ._types_enums import (
    TrimSilenceMode as TrimSilenceMode,
)
from ._types_enums import (
    VoicePresetId as VoicePresetId,
)
from ._types_enums import (
    WindowType as WindowType,
)
from ._types_mastering import (
    DynamicsProcessorResult as DynamicsProcessorResult,
)
from ._types_mastering import (
    LoudnessMatchResult as LoudnessMatchResult,
)
from ._types_mastering import (
    LoudnessMatchStereoResult as LoudnessMatchStereoResult,
)
from ._types_mastering import (
    MasteringChainResult as MasteringChainResult,
)
from ._types_mastering import (
    MasteringChainStereoResult as MasteringChainStereoResult,
)
from ._types_mastering import (
    MasteringLoudnessSummary as MasteringLoudnessSummary,
)
from ._types_mastering import (
    MasteringReport as MasteringReport,
)
from ._types_mastering import (
    MasteringResult as MasteringResult,
)
from ._types_mastering import (
    MasteringStereoResult as MasteringStereoResult,
)
from ._types_mastering import (
    StageGainReduction as StageGainReduction,
)
from ._types_mastering import (
    StreamingLoudnessGain as StreamingLoudnessGain,
)
from ._types_mastering_results import (
    EqBandInput as EqBandInput,
)
from ._types_mastering_results import (
    MasteringAssistantProfile as MasteringAssistantProfile,
)
from ._types_mastering_results import (
    MasteringAssistantResult as MasteringAssistantResult,
)
from ._types_mastering_results import (
    MasteringAudioProfile as MasteringAudioProfile,
)
from ._types_mastering_results import (
    MasteringAudioProfileDynamics as MasteringAudioProfileDynamics,
)
from ._types_mastering_results import (
    MasteringAudioProfileLoudness as MasteringAudioProfileLoudness,
)
from ._types_mastering_results import (
    MasteringAudioProfileSpectral as MasteringAudioProfileSpectral,
)
from ._types_mastering_results import (
    MasteringChainConfig as MasteringChainConfig,
)
from ._types_mastering_results import (
    MasteringStreamingPreviewPlatform as MasteringStreamingPreviewPlatform,
)
from ._types_mastering_results import (
    MasteringStreamingPreviewResult as MasteringStreamingPreviewResult,
)
from ._types_mastering_results import (
    MatchEqCurveResult as MatchEqCurveResult,
)
from ._types_mastering_results import (
    MatchEstimateReferenceDelaySamplesResult as MatchEstimateReferenceDelaySamplesResult,
)
from ._types_mastering_results import (
    MatchReferenceLoudnessResult as MatchReferenceLoudnessResult,
)
from ._types_mastering_results import (
    MatchTonalBalanceBand as MatchTonalBalanceBand,
)
from ._types_mastering_results import (
    MatchTonalBalanceResult as MatchTonalBalanceResult,
)
from ._types_mastering_results import (
    StereoMonoCompatBand as StereoMonoCompatBand,
)
from ._types_mastering_results import (
    StereoMonoCompatCheckLogBandsResult as StereoMonoCompatCheckLogBandsResult,
)
from ._types_mastering_results import (
    StereoMonoCompatCheckResult as StereoMonoCompatCheckResult,
)
from ._types_mastering_results import (
    StreamingPlatform as StreamingPlatform,
)
from ._types_mixing_assistant import (
    MixAnalysisBand as MixAnalysisBand,
)
from ._types_mixing_assistant import (
    MixAssistantMixProfile as MixAssistantMixProfile,
)
from ._types_mixing_assistant import (
    MixAssistantResult as MixAssistantResult,
)
from ._types_mixing_assistant import (
    MixAssistantTrackProfile as MixAssistantTrackProfile,
)
from ._types_mixing_assistant import (
    MixBandDominance as MixBandDominance,
)
from ._types_mixing_assistant import (
    MixCrowdedBand as MixCrowdedBand,
)
from ._types_mixing_assistant import (
    MixMonoRisk as MixMonoRisk,
)
from ._types_mixing_assistant import (
    MixSourceClass as MixSourceClass,
)
from ._types_mixing_assistant import (
    MixTrackAlignment as MixTrackAlignment,
)
from ._types_repair import (
    AudioDefects as AudioDefects,
)
from ._types_repair import (
    ClickDetection as ClickDetection,
)
from ._types_repair import (
    ClipDetection as ClipDetection,
)
from ._types_repair import (
    CrackleDetection as CrackleDetection,
)
from ._types_repair import (
    DeclickReport as DeclickReport,
)
from ._types_repair import (
    DeclickResult as DeclickResult,
)
from ._types_repair import (
    DeclickStage as DeclickStage,
)
from ._types_repair import (
    DeclickStereoResult as DeclickStereoResult,
)
from ._types_repair import (
    DeclipReport as DeclipReport,
)
from ._types_repair import (
    DeclipResult as DeclipResult,
)
from ._types_repair import (
    DeclipStage as DeclipStage,
)
from ._types_repair import (
    DeclipStereoResult as DeclipStereoResult,
)
from ._types_repair import (
    DecrackleReport as DecrackleReport,
)
from ._types_repair import (
    DecrackleResult as DecrackleResult,
)
from ._types_repair import (
    DecrackleStage as DecrackleStage,
)
from ._types_repair import (
    DecrackleStereoResult as DecrackleStereoResult,
)
from ._types_repair import (
    DehumReport as DehumReport,
)
from ._types_repair import (
    DehumResult as DehumResult,
)
from ._types_repair import (
    DehumStage as DehumStage,
)
from ._types_repair import (
    DehumStereoResult as DehumStereoResult,
)
from ._types_repair import (
    DenoiseLinkedResult as DenoiseLinkedResult,
)
from ._types_repair import (
    DenoiseReport as DenoiseReport,
)
from ._types_repair import (
    DenoiseResult as DenoiseResult,
)
from ._types_repair import (
    DenoiseStage as DenoiseStage,
)
from ._types_repair import (
    DenoiseStereoResult as DenoiseStereoResult,
)
from ._types_repair import (
    DereverbLinkedResult as DereverbLinkedResult,
)
from ._types_repair import (
    DereverbReport as DereverbReport,
)
from ._types_repair import (
    DereverbResult as DereverbResult,
)
from ._types_repair import (
    DereverbStage as DereverbStage,
)
from ._types_repair import (
    DereverbStereoResult as DereverbStereoResult,
)
from ._types_repair import (
    HumDetection as HumDetection,
)
from ._types_repair import (
    MasteringRepairAnalysis as MasteringRepairAnalysis,
)
from ._types_repair import (
    NoiseDetection as NoiseDetection,
)
from ._types_repair import (
    RepairApplyResult as RepairApplyResult,
)
from ._types_repair import (
    RepairStage as RepairStage,
)
from ._types_repair import (
    RepairStageName as RepairStageName,
)
from ._types_repair import (
    RepairStageReports as RepairStageReports,
)
from ._types_repair import (
    ReverbDetection as ReverbDetection,
)
from ._types_streaming import (
    CqtResult as CqtResult,
)
from ._types_streaming import (
    StreamBarChord as StreamBarChord,
)
from ._types_streaming import (
    StreamChordChange as StreamChordChange,
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
    StreamProgressiveEstimate as StreamProgressiveEstimate,
)
from ._types_streaming import (
    StreamQuantizeConfig as StreamQuantizeConfig,
)
from ._types_streaming import (
    StreamStats as StreamStats,
)

_rebind_facade_exports(globals(), "libsonare._types_")
del _rebind_facade_exports
