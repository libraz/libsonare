/** A value a result cannot give is an omitted field: optional, never `null`. */

import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  AnalysisDynamics,
  AnalysisResult,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  DecomposeNotePitchRequest,
  DynamicsResult,
  EngineMeterTelemetry,
  ExtractNotesRequest,
  LufsResult,
  MasteringAssistantProfile,
  MasteringAudioProfileDynamics,
  MasteringAudioProfileLoudness,
  MasteringAudioProfileSpectral,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertParamUnit,
  MasteringInsertSlot,
  MasteringLoudnessSummary,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
  MasteringRealtimeCost,
  MasteringRepairAnalysis,
  MasteringStreamingPreviewPlatform,
  MatchReferenceLoudnessResult,
  MatchTonalBalanceBand,
  MergeNotesRequest,
  MixAssistantTrackProfile,
  MixMeterSnapshot,
  MixMonoRisk,
  NoiseDetection,
  NoteSegmentsRequest,
  PitchCorrectOptions,
  PitchCorrectTimevaryingRequest,
  ProjectClip,
  ProjectCompileResult,
  ProjectDiagnostic,
  ProjectDiagnosticCode,
  ProjectDiagnosticSeverity,
  ProjectLoopMode,
  ProjectMarker,
  ProjectMarkerKind,
  ProjectSource,
  ProjectSourceKind,
  ProjectTimeline,
  RenderNotesRequest,
  ReverbDetection,
  RoomEstimateResult,
  RoomGeometryEstimate,
  Section,
  SectionType,
  SidechainCheck,
  SidechainRefusal,
  SplitNoteRequest,
  StereoMonoCompatBand,
  StereoMonoCompatCheckResult,
  StreamingLoudnessGainResult,
  VoicedFlags,
} from '../src/index.js';
import { Audio } from '../src/index.js';

describe('result records type an unmeasured value as an absent field', () => {
  it('the loudness family', () => {
    expectTypeOf<MixAssistantTrackProfile['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MatchReferenceLoudnessResult['gainToMatchDb']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<StereoMonoCompatCheckResult['width']>().toEqualTypeOf<number | undefined>();
  });

  it('a level silence leaves unmeasured, or an empty input never reaches, is absent', () => {
    expectTypeOf<MasteringAssistantProfile['loudnessRangeLu']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAssistantProfile['truePeakDbtp']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringAssistantProfile['crestFactorDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringAudioProfileLoudness['truePeakDbtp']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAudioProfileLoudness['crestFactorDb']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MatchTonalBalanceBand['sourceDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MatchTonalBalanceBand['referenceDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MatchTonalBalanceBand['deviationDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixAssistantTrackProfile['truePeakDbtp']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixAssistantTrackProfile['crestFactorDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMonoRisk['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMonoRisk['width']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StereoMonoCompatCheckResult['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StereoMonoCompatBand['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<RoomEstimateResult['drrDb']>().toEqualTypeOf<number | undefined>();
  });

  it('a level that is -Infinity for silence is never absent', () => {
    // An empty input is refused before the dynamics analyzer runs, so it never leaves a level absent.
    expectTypeOf<AnalysisDynamics['peakDb']>().toEqualTypeOf<number>();
    expectTypeOf<AnalysisDynamics['rmsDb']>().toEqualTypeOf<number>();
    expectTypeOf<DynamicsResult['peakDb']>().toEqualTypeOf<number>();
    expectTypeOf<DynamicsResult['rmsDb']>().toEqualTypeOf<number>();
    expectTypeOf<MasteringAudioProfileLoudness['integratedLufs']>().toEqualTypeOf<number>();
    expectTypeOf<MasteringAssistantProfile['integratedLufs']>().toEqualTypeOf<number>();
    expectTypeOf<MasteringStreamingPreviewPlatform['integratedLufs']>().toEqualTypeOf<number>();
    expectTypeOf<MatchReferenceLoudnessResult['sourceLufs']>().toEqualTypeOf<number>();
    expectTypeOf<MatchReferenceLoudnessResult['referenceLufs']>().toEqualTypeOf<number>();
  });

  it('the capability catalog', () => {
    expectTypeOf<MasteringInsertSlot['parent']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<MasteringInsertParamInfo['id']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringInsertParamInfo['min']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringInsertParamInfo['max']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringInsertParamInfo['maxRelativeTo']>().toEqualTypeOf<
      'nyquist' | undefined
    >();
    expectTypeOf<MasteringInsertParamInfo['default']>().toEqualTypeOf<
      boolean | number | undefined
    >();
    expectTypeOf<MasteringInsertParamInfo['unit']>().toEqualTypeOf<
      MasteringInsertParamUnit | undefined
    >();
    expectTypeOf<MasteringInsertParamInfo['uiMin']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringInsertParamInfo['uiMax']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringInsertParamInfo['choices']>().toEqualTypeOf<
      readonly MasteringInsertParamChoice[] | undefined
    >();
    expectTypeOf<MasteringInsertParamInfo['slot']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<MasteringProcessorCatalogEntry['realtimeCost']>().toEqualTypeOf<
      MasteringRealtimeCost | undefined
    >();
    expectTypeOf<MasteringPresetCatalogEntry['targetLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringPresetCatalogEntry['truePeakCeilingDbtp']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringPresetCatalogEntry['maxLimiterGainReductionDb']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<CapabilityCatalogChainStage['processor']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<CapabilityCatalogChainList['processor']>().toEqualTypeOf<string | undefined>();
  });

  it('the engine and project checks', () => {
    expectTypeOf<SidechainCheck['reason']>().toEqualTypeOf<SidechainRefusal | undefined>();
    expectTypeOf<ProjectCompileResult['timeline']>().toEqualTypeOf<ProjectTimeline | undefined>();
  });

  it('AnalysisDynamics migration: dynamicRangeDb and crestFactor can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<AnalysisDynamics, 'dynamicRangeDb' | 'crestFactor'>>();
    expectTypeOf<AnalysisDynamics['dynamicRangeDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisDynamics['crestFactor']>().toEqualTypeOf<number | undefined>();
  });

  it('AnalysisResult migration: section positions and optional timbre measures', () => {
    expectTypeOf<AnalysisResult['sections']>().toEqualTypeOf<Section[]>();
    expectTypeOf<Section['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<Section['endSec']>().toEqualTypeOf<number>();
    expectTypeOf<Section['type']>().toEqualTypeOf<SectionType>();
    expectTypeOf<'start' extends keyof Section ? true : false>().toEqualTypeOf<false>();
    expectTypeOf<'end' extends keyof Section ? true : false>().toEqualTypeOf<false>();
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<
        AnalysisResult['timbre'],
        'brightness' | 'warmth' | 'density' | 'roughness' | 'complexity'
      >
    >();
    expectTypeOf<AnalysisResult['timbre']['brightness']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['timbre']['warmth']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['timbre']['density']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['timbre']['roughness']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['timbre']['complexity']>().toEqualTypeOf<number | undefined>();
  });

  it('Audio.crestFactorDb migration: silence returns an absent measurement', () => {
    expectTypeOf<ReturnType<Audio['crestFactorDb']>>().toEqualTypeOf<number | undefined>();
    const audio = Audio.fromBuffer(new Float32Array(256), 22050);
    try {
      expect(audio.crestFactorDb()).toBeUndefined();
    } finally {
      audio.destroy();
    }
  });

  it('DynamicsResult migration: dynamicRangeDb and crestFactor can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<DynamicsResult, 'dynamicRangeDb' | 'crestFactor'>>();
    expectTypeOf<DynamicsResult['dynamicRangeDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<DynamicsResult['crestFactor']>().toEqualTypeOf<number | undefined>();
  });

  it('EngineMeterTelemetry migration: unmeasured scalar fields can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<
        EngineMeterTelemetry,
        'correlation' | 'integratedLufs' | 'momentaryLufs' | 'monoCompatWidth' | 'shortTermLufs'
      >
    >();
    expectTypeOf<EngineMeterTelemetry['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<EngineMeterTelemetry['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<EngineMeterTelemetry['momentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<EngineMeterTelemetry['monoCompatWidth']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<EngineMeterTelemetry['shortTermLufs']>().toEqualTypeOf<number | undefined>();
  });

  it('LufsResult migration: short-signal windows can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<LufsResult, 'maxMomentaryLufs' | 'maxShortTermLufs' | 'momentaryLufs' | 'shortTermLufs'>
    >();
    expectTypeOf<LufsResult['maxMomentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['maxShortTermLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['momentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['shortTermLufs']>().toEqualTypeOf<number | undefined>();
  });

  it('MasteringLoudnessSummary migration: short-signal windows can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<MasteringLoudnessSummary, 'maxMomentaryLufs' | 'maxShortTermLufs'>
    >();
    expectTypeOf<MasteringLoudnessSummary['maxMomentaryLufs']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringLoudnessSummary['maxShortTermLufs']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('MasteringRepairAnalysis migration: absent detector reports can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<MasteringRepairAnalysis, 'channels' | 'defects'>>();
    expectTypeOf<MasteringRepairAnalysis['channels']>().toEqualTypeOf<
      NonNullable<MasteringRepairAnalysis['channels']> | undefined
    >();
    expectTypeOf<MasteringRepairAnalysis['defects']>().toEqualTypeOf<
      NonNullable<MasteringRepairAnalysis['defects']> | undefined
    >();
  });

  it('MasteringStreamingPreviewPlatform migration: unmeasured gain can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<MasteringStreamingPreviewPlatform, 'normalizationGainDb'>
    >();
    expectTypeOf<MasteringStreamingPreviewPlatform['normalizationGainDb']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('MixMeterSnapshot migration: unmeasured scalar fields can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<
        MixMeterSnapshot,
        'correlation' | 'integratedLufs' | 'momentaryLufs' | 'monoCompatWidth' | 'shortTermLufs'
      >
    >();
    expectTypeOf<MixMeterSnapshot['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['momentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['monoCompatWidth']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['shortTermLufs']>().toEqualTypeOf<number | undefined>();
  });

  it('NoiseDetection migration: a missing floor can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<NoiseDetection, 'floorDbfs'>>();
    expectTypeOf<NoiseDetection['floorDbfs']>().toEqualTypeOf<number | undefined>();
  });

  it('ProjectClip migration: whole-clip loop length is absent and tokens stay named', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<ProjectClip, 'loopLengthPpq'>>();
    expectTypeOf<ProjectClip['loopLengthPpq']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<ProjectClip['loopMode']>().toEqualTypeOf<ProjectLoopMode>();
    expectTypeOf<ProjectClip['sourceKind']>().toEqualTypeOf<ProjectSourceKind>();
    expectTypeOf<ProjectLoopMode>().toEqualTypeOf<'off' | 'loop'>();
    expectTypeOf<ProjectSourceKind>().toEqualTypeOf<'audio' | 'midi'>();
  });

  it('ProjectDiagnostic migration: no target id is represented by omission', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<ProjectDiagnostic, 'targetId'>>();
    expectTypeOf<ProjectDiagnostic['targetId']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<ProjectDiagnostic['code']>().toEqualTypeOf<ProjectDiagnosticCode>();
    expectTypeOf<ProjectDiagnostic['severity']>().toEqualTypeOf<ProjectDiagnosticSeverity>();
  });

  it('ProjectMarker migration: unnamed markers omit name and use a named kind', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<ProjectMarker, 'name'>>();
    expectTypeOf<ProjectMarker['name']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<ProjectMarker['kind']>().toEqualTypeOf<ProjectMarkerKind>();
    expectTypeOf<ProjectMarkerKind>().toEqualTypeOf<
      'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature'
    >();
  });

  it('ProjectSource migration: absent metadata is omitted and source kind is named', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<ProjectSource, 'contentHash' | 'externalStemRole'>>();
    expectTypeOf<ProjectSource['contentHash']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<ProjectSource['externalStemRole']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<ProjectSource['kind']>().toEqualTypeOf<ProjectSourceKind>();
  });

  it('a spectral shape or level spread an input without energy does not have is absent', () => {
    expectTypeOf<MasteringAudioProfileSpectral['centroidHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringAudioProfileSpectral['flatness']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringAudioProfileSpectral['rolloffHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringAudioProfileDynamics['shortTermLufsStd']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAudioProfileDynamics['sustainRatio']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAudioProfileDynamics['attackPerSecond']>().toEqualTypeOf<number>();
    expectTypeOf<MasteringAssistantProfile['spectralCentroidHz']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAssistantProfile['spectralFlatness']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAssistantProfile['sustainRatio']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixAssistantTrackProfile['spectralCentroidHz']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MixAssistantTrackProfile['spectralFlatness']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MixAssistantTrackProfile['sustainRatio']>().toEqualTypeOf<number | undefined>();
  });

  it('ReverbDetection migration: the unrun WPE metric and an unmeasured decay can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<ReverbDetection, 'lateDecayRatioDb' | 'latePredictability'>
    >();
    expectTypeOf<ReverbDetection['lateDecayRatioDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<ReverbDetection['latePredictability']>().toEqualTypeOf<number | undefined>();
  });

  it('RoomGeometryEstimate migration: unmeasured room dimensions can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<
      Pick<RoomGeometryEstimate, 'heightM' | 'lengthM' | 'widthM'>
    >();
    expectTypeOf<RoomGeometryEstimate['heightM']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<RoomGeometryEstimate['lengthM']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<RoomGeometryEstimate['widthM']>().toEqualTypeOf<number | undefined>();
  });

  it('StreamingLoudnessGainResult migration: an unmeasured gain can be omitted', () => {
    expectTypeOf<{}>().toMatchTypeOf<Pick<StreamingLoudnessGainResult, 'loudnessStaticGainDb'>>();
    expectTypeOf<StreamingLoudnessGainResult['loudnessStaticGainDb']>().toEqualTypeOf<
      number | undefined
    >();
  });
});

describe('request types do not list null', () => {
  it('takes voicing data as an omitted field, never null', () => {
    expectTypeOf<ExtractNotesRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<ExtractNotesRequest['voicedProb']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<SplitNoteRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<SplitNoteRequest['voicedProb']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<MergeNotesRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<MergeNotesRequest['voicedProb']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<NoteSegmentsRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<NoteSegmentsRequest['voicedProb']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<PitchCorrectOptions['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<PitchCorrectOptions['voicedProb']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<PitchCorrectTimevaryingRequest['voiced']>().toEqualTypeOf<
      VoicedFlags | undefined
    >();
    expectTypeOf<PitchCorrectTimevaryingRequest['voicedProb']>().toEqualTypeOf<
      Float32Array | undefined
    >();
    expectTypeOf<DecomposeNotePitchRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    expectTypeOf<RenderNotesRequest['voiced']>().toEqualTypeOf<VoicedFlags | undefined>();
    // @ts-expect-error null is not a spelling of "omitted".
    const voiced: ExtractNotesRequest['voiced'] = null;
    // @ts-expect-error null is not a spelling of "omitted".
    const voicedProb: PitchCorrectOptions['voicedProb'] = null;
    void voiced;
    void voicedProb;
  });
});
