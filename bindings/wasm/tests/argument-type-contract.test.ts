/**
 * An entry point that takes its audio positionally or as a request object tells a
 * wrong-typed first argument apart from a request: `null`, a number, an array or
 * another typed array is a `TypeError` naming the field, never a property-read
 * failure ("Cannot read properties of null"). And a string the native layer reads
 * through embind is a `TypeError` too, never a raw `BindingError`. An entry point
 * that takes only a request refuses a non-object request the same way.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import * as sonare from '../src/index';

type Fn = (...args: unknown[]) => unknown;

const fn = (name: string): Fn => (sonare as unknown as Record<string, Fn>)[name];

/** [entry point, name of its first (buffer) argument]. */
const BUFFER_FIRST: Array<[string, string]> = [
  ['amplitudeToDb', 'values'],
  ['analyze', 'samples'],
  ['analyzeBpm', 'samples'],
  ['analyzeDynamics', 'samples'],
  ['analyzeImpulseResponse', 'samples'],
  ['analyzeMelody', 'samples'],
  ['analyzeRhythm', 'samples'],
  ['analyzeSections', 'samples'],
  ['analyzeTimbre', 'samples'],
  ['analyzeWithProgress', 'samples'],
  ['bassChroma', 'samples'],
  ['chordFunctionalAnalysis', 'samples'],
  ['chroma', 'samples'],
  ['chromaCens', 'samples'],
  ['chromaCqt', 'samples'],
  ['clicks', 'times'],
  ['cqt', 'samples'],
  ['cqtToAudio', 'magnitude'],
  ['cyclicTempogram', 'onsetEnvelope'],
  ['decompose', 's'],
  ['decomposeWithInit', 's'],
  ['deemphasis', 'samples'],
  ['detectAcoustic', 'samples'],
  ['detectBeats', 'samples'],
  ['detectBpm', 'samples'],
  ['detectChords', 'samples'],
  ['detectDownbeats', 'samples'],
  ['detectKey', 'samples'],
  ['detectKeyCandidates', 'samples'],
  ['detectOnsets', 'samples'],
  ['ebur128LoudnessRange', 'samples'],
  ['estimateRoom', 'samples'],
  ['estimateTuning', 'samples'],
  ['fixLength', 'values'],
  ['fourierTempogram', 'onsetEnvelope'],
  ['frameSignal', 'samples'],
  ['griffinLim', 'magnitude'],
  ['harmonic', 'samples'],
  ['hpss', 'samples'],
  ['hpssWithResidual', 'samples'],
  ['hybridCqt', 'samples'],
  ['lufs', 'samples'],
  ['lufsInterleaved', 'samples'],
  ['lufsSeriesInterleaved', 'samples'],
  ['mastering', 'samples'],
  ['masteringAssistantSuggest', 'samples'],
  ['masteringAudioProfile', 'samples'],
  ['masteringChain', 'samples'],
  ['masteringChainStereo', 'left'],
  ['masteringChainStereoWithProgress', 'left'],
  ['masteringChainWithProgress', 'samples'],
  ['masteringDynamicsCompressor', 'samples'],
  ['masteringDynamicsGate', 'samples'],
  ['masteringDynamicsTransientShaper', 'samples'],
  ['masteringRepairDeclick', 'samples'],
  ['masteringRepairDeclickStereo', 'left'],
  ['masteringRepairDeclip', 'samples'],
  ['masteringRepairDeclipStereo', 'left'],
  ['masteringRepairDecrackle', 'samples'],
  ['masteringRepairDecrackleStereo', 'left'],
  ['masteringRepairDehum', 'samples'],
  ['masteringRepairDehumStereo', 'left'],
  ['masteringRepairDenoiseClassical', 'samples'],
  ['masteringRepairDenoiseClassicalStereo', 'left'],
  ['masteringRepairDereverbClassical', 'samples'],
  ['masteringRepairDereverbClassicalStereo', 'left'],
  ['masteringRepairDetectClicks', 'samples'],
  ['masteringRepairDetectClipping', 'samples'],
  ['masteringRepairDetectCrackle', 'samples'],
  ['masteringRepairDetectHum', 'samples'],
  ['masteringRepairDetectNoiseFloor', 'samples'],
  ['masteringRepairDetectReverb', 'samples'],
  ['masteringRepairDetectTrimRange', 'samples'],
  ['masteringRepairDetectTrimRangeStereo', 'left'],
  ['masteringRepairTrimSilence', 'samples'],
  ['masteringRepairTrimSilenceStereo', 'left'],
  ['masteringStreamingPreview', 'samples'],
  ['melDelta', 'features'],
  ['melSpectrogram', 'samples'],
  ['meteringCrestFactorDb', 'samples'],
  ['meteringDcOffset', 'samples'],
  ['meteringDetectClipping', 'samples'],
  ['meteringDynamicRange', 'samples'],
  ['meteringPeakDb', 'samples'],
  ['meteringPhaseScope', 'left'],
  ['meteringPhaseScopeDecimated', 'left'],
  ['meteringRmsDb', 'samples'],
  ['meteringSilenceRatio', 'samples'],
  ['meteringSpectrum', 'samples'],
  ['meteringSpectrumFrame', 'samples'],
  ['meteringStereoCorrelation', 'left'],
  ['meteringStereoWidth', 'left'],
  ['meteringTruePeakDb', 'samples'],
  ['meteringVectorscope', 'left'],
  ['meteringVectorscopeDecimated', 'left'],
  ['mfcc', 'samples'],
  ['momentaryLufs', 'samples'],
  ['nnFilter', 's'],
  ['nnlsChroma', 'samples'],
  ['normalize', 'samples'],
  ['noteMove', 'samples'],
  ['noteStretch', 'samples'],
  ['onsetEnvelope', 'samples'],
  ['onsetStrengthMulti', 'samples'],
  ['padCenter', 'values'],
  ['pcen', 'values'],
  ['peakPick', 'values'],
  ['percussive', 'samples'],
  ['phaseVocoder', 'samples'],
  ['piptrack', 'samples'],
  ['pitchCorrectTimevarying', 'samples'],
  ['pitchCorrectToMidi', 'samples'],
  ['pitchCorrectToMidiTimevarying', 'samples'],
  ['pitchPyin', 'samples'],
  ['pitchShift', 'samples'],
  ['pitchTuning', 'frequencies'],
  ['pitchYin', 'samples'],
  ['plp', 'onsetEnvelope'],
  ['polyFeatures', 'samples'],
  ['powerToDb', 'values'],
  ['preemphasis', 'samples'],
  ['pseudoCqt', 'samples'],
  ['reassignedSpectrogram', 'samples'],
  ['remix', 'samples'],
  ['remixAlignedIntervals', 'samples'],
  ['resample', 'samples'],
  ['rmsEnergy', 'samples'],
  ['roomMorph', 'samples'],
  ['shortTermLufs', 'samples'],
  ['spectralBandwidth', 'samples'],
  ['spectralCentroid', 'samples'],
  ['spectralContrast', 'samples'],
  ['spectralEdit', 'samples'],
  ['spectralFlatness', 'samples'],
  ['spectralFlux', 'samples'],
  ['spectralRolloff', 'samples'],
  ['splitSilence', 'samples'],
  ['stft', 'samples'],
  ['stftDb', 'samples'],
  ['tempogram', 'onsetEnvelope'],
  ['tempogramRatio', 'tempogramData'],
  ['timeStretch', 'samples'],
  ['tonnetz', 'chromagram'],
  ['trim', 'samples'],
  ['trimSilence', 'samples'],
  ['vectorNormalize', 'values'],
  ['voiceChange', 'samples'],
  ['voiceChangeRealtime', 'samples'],
  ['vqt', 'samples'],
  ['vqtToAudio', 'magnitude'],
  ['waveformPeakPyramid', 'samples'],
  ['waveformPeaks', 'samples'],
  ['zeroCrossingRate', 'samples'],
  ['zeroCrossings', 'samples'],
];

/** [entry point, call that hands a number where a string belongs, argument name]. */
const STRING_ARGUMENT: Array<[string, () => unknown, string]> = [
  ['noteToHz', () => sonare.noteToHz(5 as never), 'note'],
  ['masteringInsertParamNames', () => sonare.masteringInsertParamNames(5 as never), 'name'],
  ['masteringInsertParamInfo', () => sonare.masteringInsertParamInfo(5 as never), 'name'],
  ['masteringInsertTiming', () => sonare.masteringInsertTiming(5 as never, {}, 48000), 'name'],
  ['mixSourceClassFromName', () => sonare.mixSourceClassFromName(5 as never), 'name'],
  ['mixingScenePresetJson', () => sonare.mixingScenePresetJson(5 as never), 'presetName'],
  ['synthPresetPatch', () => sonare.synthPresetPatch(5 as never), 'name'],
  [
    'validateRealtimeVoiceChangerPresetJson',
    () => sonare.validateRealtimeVoiceChangerPresetJson(5 as never),
    'json',
  ],
  [
    'realtimeVoiceChangerPresetJson',
    () => sonare.realtimeVoiceChangerPresetJson(5 as never),
    'name',
  ],
];

/** Entry points that take a request object, alone or beside a positional form. */
const REQUEST_ONLY: string[] = [
  'alignTakeToReference',
  'analyzePolyphonic',
  'assignNoteTargets',
  'autoTune',
  'chirp',
  'chordFunctions',
  'decomposeNotePitch',
  'decomposeStems',
  'decomposeStemsLinked',
  'detectBoundaries',
  'estimateMeter',
  'extractNotes',
  'extractPercussiveEvents',
  'fixFrames',
  'masteringAbMatchLoudness',
  'masteringAbMatchLoudnessStereo',
  'masteringAssistantSuggestChain',
  'masteringPairAnalyze',
  'masteringPairProcess',
  'masteringPairProcessStereo',
  'masteringProcess',
  'masteringProcessStereo',
  'masteringRepairAnalyze',
  'masteringRepairApply',
  'masteringRepairDenoiseClassicalLinked',
  'masteringRepairDereverbClassicalLinked',
  'masteringStereoAnalyze',
  'mergeNotes',
  'meteringCrestFactorDbStereo',
  'mixStereo',
  'normalizeStereo',
  'noteSegments',
  'noteTargetsFromSmf',
  'onsetBacktrack',
  'renderNotes',
  'renderPercussiveEvents',
  'renderPlayback',
  'restoreVocalEditSession',
  'segmentAgglomerative',
  'segmentCrossSimilarity',
  'segmentLagToRecurrence',
  'segmentPathEnhance',
  'segmentRecurrenceMatrix',
  'segmentRecurrenceToLag',
  'segmentSubsegment',
  'splitNote',
  'splitSilenceCommon',
  'splitSilenceCommonWithReport',
  'streamingLoudnessGain',
  'streamingLoudnessGainStereo',
  'suggestMixScene',
  'suggestMixSceneJson',
  'synthesizeRir',
  'tone',
  'transcribe',
];

/** Request entry points whose positional form leads with a number or an array of planes. */
const NUMBER_FIRST = new Set(['chirp', 'tone']);
const ARRAY_FIRST = new Set([
  'masteringRepairDenoiseClassicalLinked',
  'masteringRepairDereverbClassicalLinked',
  'mixStereo',
]);

/** Exports whose first argument is not a request, or that need a browser. */
const NOT_A_REQUEST_ENTRY = new Set([
  'init',
  'createOpfsClipPageProvider',
  'createOpfsClipPageWorker',
  'hzToNote',
  'masteringRepairNoiseBandBins',
  'realtimeVoiceChangerPresetConfig',
  'roomGeometryFromEstimate',
  'scaleCorrectionSemitones',
  'scaleMaskForMode',
  'scaleQuantizeMidi',
  'voiceCharacterPresetId',
]);

const RAW_PROPERTY_READ = /Cannot read propert|Cannot destructure|undefined is not|is not iterable/;

describe('wrong-typed first argument of a buffer-or-request entry point', () => {
  beforeAll(async () => {
    await sonare.init();
  });

  describe.each(BUFFER_FIRST)('%s', (name, argName) => {
    it.each([
      ['null', null],
      ['a number', 5],
      ['an array', [1, 2, 3]],
      ['a Float64Array', new Float64Array(8)],
    ])('refuses %s as a TypeError naming %s', (_label, value) => {
      expect(() => fn(name)(value)).toThrow(TypeError);
      expect(() => fn(name)(value)).toThrow(`${name}: ${argName} must be a Float32Array`);
    });
  });
});

describe('wrong-typed string argument', () => {
  beforeAll(async () => {
    await sonare.init();
  });

  it.each(STRING_ARGUMENT)('%s refuses a number as a TypeError', (name, call, argName) => {
    expect(call).toThrow(TypeError);
    expect(call).toThrow(`${name}: ${argName} must be a string`);
  });
});

describe('wrong-typed chord or key mode of the analysis-to-annotation conversions', () => {
  beforeAll(async () => {
    await sonare.init();
  });

  it.each([
    ['null', null],
    ['a number', 42],
    ['an array', [1, 2, 3]],
  ])('chordSymbolFromAnalysis refuses %s as a TypeError naming chord', (_label, value) => {
    expect(() => fn('chordSymbolFromAnalysis')(value)).toThrow(TypeError);
    expect(() => fn('chordSymbolFromAnalysis')(value)).toThrow(
      'chordSymbolFromAnalysis: chord must be an object',
    );
  });

  it.each([
    ['null', null],
    ['an object', {}],
    ['a boolean', true],
  ])('keyModeFromAnalysis refuses %s as a TypeError naming mode', (_label, value) => {
    expect(() => fn('keyModeFromAnalysis')(value)).toThrow(TypeError);
    expect(() => fn('keyModeFromAnalysis')(value)).toThrow(/mode/);
  });
});

describe('wrong-typed request of a request entry point', () => {
  beforeAll(async () => {
    await sonare.init();
  });

  describe.each(REQUEST_ONLY)('%s', (name) => {
    it.each([
      ['null', null],
      ['a number', 42],
      ['an array', [1, 2, 3]],
      ['a Float64Array', new Float64Array(8)],
    ])('refuses %s as a TypeError', (label, value) => {
      if (
        (label === 'a number' && NUMBER_FIRST.has(name)) ||
        (label === 'an array' && ARRAY_FIRST.has(name))
      ) {
        return; // a legitimate positional first argument
      }
      expect(() => fn(name)(value)).toThrow(TypeError);
      expect(() => fn(name)(value)).toThrow(`${name}: request must be an object`);
    });
  });

  it('leaves no export failing on a property read of a wrong-typed first argument', () => {
    const raw: string[] = [];
    for (const [name, entry] of Object.entries(sonare)) {
      if (typeof entry !== 'function' || /^[A-Z]/.test(name) || NOT_A_REQUEST_ENTRY.has(name)) {
        continue;
      }
      for (const value of [null, undefined, 42]) {
        try {
          const result = (entry as Fn)(value);
          if (result instanceof Promise) {
            result.catch(() => {});
          }
        } catch (error) {
          if (error instanceof Error && RAW_PROPERTY_READ.test(error.message)) {
            raw.push(`${name}(${String(value)}): ${error.message}`);
          }
        }
      }
    }
    expect(raw).toEqual([]);
  });
});
