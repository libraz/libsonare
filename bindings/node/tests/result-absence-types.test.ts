/** A value a result cannot give is an omitted field: optional, never `null`. */

import { describe, expectTypeOf, it } from 'vitest';
import type {
  CapabilityCatalogChainList,
  CapabilityCatalogChainStage,
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
  MixAssistantTrackProfile,
  ProjectCompileTimelineResult,
  ProjectTimeline,
  SidechainCheck,
  SidechainRefusal,
  StereoMonoCompatCheckResult,
} from '../src/index.js';

describe('result records type an unmeasured value as an absent field', () => {
  it('the loudness family', () => {
    expectTypeOf<MasteringAudioProfileLoudness['integratedLufs']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MasteringAssistantProfile['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MasteringStreamingPreviewPlatform['integratedLufs']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MixAssistantTrackProfile['integratedLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MatchReferenceLoudnessResult['sourceLufs']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<MatchReferenceLoudnessResult['referenceLufs']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<MatchReferenceLoudnessResult['gainToMatchDb']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<StereoMonoCompatCheckResult['width']>().toEqualTypeOf<number | undefined>();
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
    expectTypeOf<ProjectCompileTimelineResult['timeline']>().toEqualTypeOf<
      ProjectTimeline | undefined
    >();
  });
});
