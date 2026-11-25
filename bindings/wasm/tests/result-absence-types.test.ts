/** A value a result cannot give is an omitted field: optional, never `null`. */

import { describe, expectTypeOf, it } from 'vitest';
import type {
  AnalysisDynamics,
  AnalysisMelody,
  AnalysisResult,
  CapabilityCatalog,
  CapabilityCatalogChain,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  CapabilityCatalogPresets,
  DecomposeNotePitchRequest,
  DynamicsResult,
  EngineBounceResult,
  EngineMeterTelemetry,
  ExtractNotesRequest,
  LufsResult,
  MasteringAssistantProfile,
  MasteringAudioProfileDefects,
  MasteringAudioProfileDynamics,
  MasteringAudioProfileLoudness,
  MasteringAudioProfileSpectral,
  MasteringInsertParamChoice,
  MasteringInsertParamDependency,
  MasteringInsertParamInfo,
  MasteringInsertParamUnit,
  MasteringInsertSlot,
  MasteringInsertTiming,
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
  ProjectLoopMode,
  ProjectMarker,
  ProjectSource,
  ProjectSourceKind,
  ProjectTempoCandidate,
  ProjectTimeline,
  ProjectTrack,
  RenderNotesRequest,
  ReverbDetection,
  RoomEstimateResult,
  RoomGeometryEstimate,
  SidechainCheck,
  SidechainRefusal,
  SplitNoteRequest,
  StereoMonoCompatBand,
  StereoMonoCompatCheckResult,
  StreamingLoudnessGainResult,
  VocalStateToken,
  VoicedFlags,
} from '../src/index';

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

describe('public result shapes', () => {
  it('keeps catalog, insert, and project results readonly', () => {
    expectTypeOf<CapabilityCatalog>().toEqualTypeOf<Readonly<CapabilityCatalog>>();
    expectTypeOf<CapabilityCatalogChain>().toEqualTypeOf<Readonly<CapabilityCatalogChain>>();
    expectTypeOf<CapabilityCatalogChainList>().toEqualTypeOf<
      Readonly<CapabilityCatalogChainList>
    >();
    expectTypeOf<CapabilityCatalogChainStage>().toEqualTypeOf<
      Readonly<CapabilityCatalogChainStage>
    >();
    expectTypeOf<CapabilityCatalogPresets>().toEqualTypeOf<Readonly<CapabilityCatalogPresets>>();
    expectTypeOf<MasteringInsertParamChoice>().toEqualTypeOf<
      Readonly<MasteringInsertParamChoice>
    >();
    expectTypeOf<MasteringInsertParamDependency>().toEqualTypeOf<
      Readonly<MasteringInsertParamDependency>
    >();
    expectTypeOf<MasteringInsertSlot>().toEqualTypeOf<Readonly<MasteringInsertSlot>>();
    expectTypeOf<MasteringInsertTiming>().toEqualTypeOf<Readonly<MasteringInsertTiming>>();
    expectTypeOf<ProjectClip>().toEqualTypeOf<Readonly<ProjectClip>>();
    expectTypeOf<ProjectMarker>().toEqualTypeOf<Readonly<ProjectMarker>>();
    expectTypeOf<ProjectSource>().toEqualTypeOf<Readonly<ProjectSource>>();
    expectTypeOf<ProjectTrack>().toEqualTypeOf<Readonly<ProjectTrack>>();

    expectTypeOf<CapabilityCatalog['processors']>().toEqualTypeOf<
      readonly MasteringProcessorCatalogEntry[]
    >();
    expectTypeOf<CapabilityCatalog['masteringPresets']>().toEqualTypeOf<
      readonly MasteringPresetCatalogEntry[]
    >();
    expectTypeOf<CapabilityCatalogChain['stages']>().toEqualTypeOf<
      readonly CapabilityCatalogChainStage[]
    >();
    expectTypeOf<CapabilityCatalogChain['lists']>().toEqualTypeOf<
      readonly CapabilityCatalogChainList[]
    >();
    expectTypeOf<CapabilityCatalogChain['params']>().toEqualTypeOf<
      readonly MasteringInsertParamInfo[]
    >();
    expectTypeOf<CapabilityCatalogPresets['mastering']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['synth']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['mixingScene']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['voiceChanger']>().toEqualTypeOf<readonly string[]>();
    expectTypeOf<CapabilityCatalogPresets['playbackRoom']>().toEqualTypeOf<readonly string[]>();
  });

  it('leaves unmeasured analysis fields absent', () => {
    const absentDynamics: Pick<AnalysisDynamics, 'dynamicRangeDb' | 'crestFactor'> = {};
    const absentMelody: Pick<AnalysisMelody, 'meanFrequencyHz' | 'vibratoRateHz'> = {};
    const absentBpm: Pick<AnalysisResult, 'bpm'> = {};
    void absentDynamics;
    void absentMelody;
    void absentBpm;

    expectTypeOf<AnalysisResult['dynamics']['dynamicRangeDb']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<AnalysisResult['dynamics']['crestFactor']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisDynamics['loudnessRangeLu']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['melody']['meanFrequencyHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<AnalysisResult['melody']['vibratoRateHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<
      'loudnessRangeDb' extends keyof AnalysisDynamics ? true : false
    >().toEqualTypeOf<false>();
    expectTypeOf<
      'meanFrequency' extends keyof AnalysisMelody ? true : false
    >().toEqualTypeOf<false>();
    expectTypeOf<
      'vibratoRate' extends keyof AnalysisMelody ? true : false
    >().toEqualTypeOf<false>();
  });

  it('leaves unmeasured dynamics fields absent', () => {
    const absent: Pick<DynamicsResult, 'dynamicRangeDb' | 'crestFactor'> = {};
    void absent;
    expectTypeOf<DynamicsResult['dynamicRangeDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<DynamicsResult['crestFactor']>().toEqualTypeOf<number | undefined>();
  });

  it('uses per-channel engine meter arrays and optional stereo readings', () => {
    const absent: Pick<
      EngineMeterTelemetry,
      'correlation' | 'integratedLufs' | 'momentaryLufs' | 'monoCompatWidth' | 'shortTermLufs'
    > = {};
    void absent;
    expectTypeOf<EngineMeterTelemetry['peakDb']>().toEqualTypeOf<(number | undefined)[]>();
    expectTypeOf<EngineMeterTelemetry['rmsDb']>().toEqualTypeOf<(number | undefined)[]>();
    expectTypeOf<EngineMeterTelemetry['truePeakDbtp']>().toEqualTypeOf<(number | undefined)[]>();
    expectTypeOf<
      'peakDbL' extends keyof EngineMeterTelemetry
        ? true
        : 'peakDbR' extends keyof EngineMeterTelemetry
          ? true
          : 'rmsDbL' extends keyof EngineMeterTelemetry
            ? true
            : 'rmsDbR' extends keyof EngineMeterTelemetry
              ? true
              : 'truePeakDbtpL' extends keyof EngineMeterTelemetry
                ? true
                : 'truePeakDbtpR' extends keyof EngineMeterTelemetry
                  ? true
                  : false
    >().toEqualTypeOf<false>();
  });

  it('leaves short-window loudness results absent', () => {
    const absentLufs: Pick<
      LufsResult,
      'maxMomentaryLufs' | 'maxShortTermLufs' | 'momentaryLufs' | 'shortTermLufs'
    > = {};
    void absentLufs;
    expectTypeOf<LufsResult['maxMomentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['maxShortTermLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['momentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<LufsResult['shortTermLufs']>().toEqualTypeOf<number | undefined>();
  });

  it('leaves short-window mastering summary values absent', () => {
    const absent: Pick<MasteringLoudnessSummary, 'maxMomentaryLufs' | 'maxShortTermLufs'> = {};
    void absent;
    expectTypeOf<MasteringLoudnessSummary['maxMomentaryLufs']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringLoudnessSummary['maxShortTermLufs']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('leaves repair analysis channels and defects absent until measured', () => {
    const absent: Pick<MasteringRepairAnalysis, 'channels' | 'defects'> = {};
    void absent;
    expectTypeOf<MasteringRepairAnalysis['channels']>().toEqualTypeOf<
      MasteringAudioProfileDefects[] | undefined
    >();
    expectTypeOf<MasteringRepairAnalysis['defects']>().toEqualTypeOf<
      MasteringAudioProfileDefects | undefined
    >();
    expectTypeOf<NonNullable<MasteringRepairAnalysis['channels']>[number]>().toEqualTypeOf<
      NonNullable<MasteringRepairAnalysis['defects']>
    >();
  });

  it('leaves an unavailable streaming normalization gain absent', () => {
    const absent: Pick<MasteringStreamingPreviewPlatform, 'normalizationGainDb'> = {};
    void absent;
    expectTypeOf<MasteringStreamingPreviewPlatform['normalizationGainDb']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('leaves unmeasured mix meter readings absent', () => {
    const absent: Pick<
      MixMeterSnapshot,
      'correlation' | 'integratedLufs' | 'momentaryLufs' | 'monoCompatWidth' | 'shortTermLufs'
    > = {};
    void absent;
    expectTypeOf<MixMeterSnapshot['correlation']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['momentaryLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['monoCompatWidth']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MixMeterSnapshot['shortTermLufs']>().toEqualTypeOf<number | undefined>();
  });

  it('leaves an unavailable noise floor absent', () => {
    const absent: Pick<NoiseDetection, 'floorDbfs'> = {};
    void absent;
    expectTypeOf<NoiseDetection['floorDbfs']>().toEqualTypeOf<number | undefined>();
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

  it('leaves reverb predictability and an unmeasured decay absent', () => {
    const absent: Pick<ReverbDetection, 'lateDecayRatioDb' | 'latePredictability'> = {};
    void absent;
    expectTypeOf<ReverbDetection['lateDecayRatioDb']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<ReverbDetection['latePredictability']>().toEqualTypeOf<number | undefined>();
  });

  it('keeps optional clip loop length and token fields', () => {
    const absent: Pick<ProjectClip, 'loopLengthPpq'> = {};
    void absent;
    expectTypeOf<ProjectClip['loopLengthPpq']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<ProjectClip['loopMode']>().toEqualTypeOf<ProjectLoopMode>();
    expectTypeOf<ProjectClip['sourceKind']>().toEqualTypeOf<ProjectSourceKind>();
  });

  it('keeps optional source metadata and a source kind token', () => {
    const absent: Pick<ProjectSource, 'contentHash' | 'externalStemRole'> = {};
    void absent;
    expectTypeOf<ProjectSource['contentHash']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<ProjectSource['externalStemRole']>().toEqualTypeOf<string | undefined>();
    expectTypeOf<ProjectSource['kind']>().toEqualTypeOf<ProjectSourceKind>();
  });

  it('leaves unavailable room dimensions absent', () => {
    const absent: RoomGeometryEstimate = {};
    void absent;
    expectTypeOf<RoomGeometryEstimate['heightM']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<RoomGeometryEstimate['lengthM']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<RoomGeometryEstimate['widthM']>().toEqualTypeOf<number | undefined>();
  });

  it('leaves a missing streaming loudness gain absent', () => {
    const absent: Pick<StreamingLoudnessGainResult, 'loudnessStaticGainDb'> = {};
    void absent;
    expectTypeOf<StreamingLoudnessGainResult['loudnessStaticGainDb']>().toEqualTypeOf<
      number | undefined
    >();
  });

  it('keeps all tempo candidate fields readonly', () => {
    expectTypeOf<ProjectTempoCandidate>().toEqualTypeOf<Readonly<ProjectTempoCandidate>>();
  });

  it('keeps the bounce result fields readonly', () => {
    expectTypeOf<
      Pick<EngineBounceResult, 'channels' | 'sampleRate' | 'integratedLufs'>
    >().toEqualTypeOf<
      Readonly<Pick<EngineBounceResult, 'channels' | 'sampleRate' | 'integratedLufs'>>
    >();
  });

  it('keeps all vocal state token fields readonly', () => {
    expectTypeOf<VocalStateToken>().toEqualTypeOf<Readonly<VocalStateToken>>();
  });
});
