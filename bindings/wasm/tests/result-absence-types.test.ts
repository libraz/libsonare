/** A value a result cannot give is an omitted field: optional, never `null`. */

import { describe, expectTypeOf, it } from 'vitest';
import type {
  AnalysisDynamics,
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
  DecomposeNotePitchRequest,
  DynamicsResult,
  ExtractNotesRequest,
  MasteringAssistantProfile,
  MasteringAudioProfileLoudness,
  MasteringInsertParamChoice,
  MasteringInsertParamInfo,
  MasteringInsertParamUnit,
  MasteringInsertSlot,
  MasteringPresetCatalogEntry,
  MasteringProcessorCatalogEntry,
  MasteringRealtimeCost,
  MasteringStreamingPreviewPlatform,
  MatchReferenceLoudnessResult,
  MatchTonalBalanceBand,
  MergeNotesRequest,
  MixAssistantTrackProfile,
  MixMonoRisk,
  NoteSegmentsRequest,
  PitchCorrectOptions,
  PitchCorrectTimevaryingRequest,
  ProjectCompileResult,
  ProjectTimeline,
  RenderNotesRequest,
  SidechainCheck,
  SidechainRefusal,
  SplitNoteRequest,
  StereoMonoCompatCheckResult,
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
