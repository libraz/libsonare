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
  ['transcribe', 'samples'],
  ['detectBoundaries', 'samples'],
  ['analyzePolyphonic', 'samples'],
  ['analyze', 'samples'],
  ['analyzeBpm', 'samples'],
  ['analyzeDynamics', 'samples'],
  ['analyzeImpulseResponse', 'samples'],
  ['analyzeMelody', 'samples'],
  ['analyzeRhythm', 'samples'],
  ['analyzeSections', 'samples'],
  ['analyzeTimbre', 'samples'],
  ['bassChroma', 'samples'],
  ['chordFunctionalAnalysis', 'samples'],
  ['chroma', 'samples'],
  ['chromaCens', 'samples'],
  ['chromaCqt', 'samples'],
  ['cqt', 'samples'],
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
  ['frameSignal', 'samples'],
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
  ['masteringRepairDeclip', 'samples'],
  ['masteringRepairDecrackle', 'samples'],
  ['masteringRepairDehum', 'samples'],
  ['masteringRepairDenoiseClassical', 'samples'],
  ['masteringRepairDereverbClassical', 'samples'],
  ['masteringRepairDetectClicks', 'samples'],
  ['masteringRepairDetectClipping', 'samples'],
  ['masteringRepairDetectCrackle', 'samples'],
  ['masteringRepairDetectHum', 'samples'],
  ['masteringRepairDetectNoiseFloor', 'samples'],
  ['masteringRepairDetectReverb', 'samples'],
  ['masteringRepairDetectTrimRange', 'samples'],
  ['masteringRepairTrimSilence', 'samples'],
  ['masteringStreamingPreview', 'samples'],
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
  ['nnlsChroma', 'samples'],
  ['normalize', 'samples'],
  ['noteMove', 'samples'],
  ['noteStretch', 'samples'],
  ['onsetStrength', 'samples'],
  ['onsetStrengthMulti', 'samples'],
  ['percussive', 'samples'],
  ['phaseVocoder', 'samples'],
  ['piptrack', 'samples'],
  ['pitchCorrectTimevarying', 'samples'],
  ['pitchCorrectToMidi', 'samples'],
  ['pitchPyin', 'samples'],
  ['pitchShift', 'samples'],
  ['pitchYin', 'samples'],
  ['polyFeatures', 'samples'],
  ['preemphasis', 'samples'],
  ['pseudoCqt', 'samples'],
  ['reassignedSpectrogram', 'samples'],
  ['remix', 'samples'],
  ['remixAlignedIntervals', 'samples'],
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
  ['timeStretch', 'samples'],
  ['trim', 'samples'],
  ['trimSilence', 'samples'],
  ['voiceChange', 'samples'],
  ['vqt', 'samples'],
  ['waveformPeakPyramid', 'samples'],
  ['waveformPeaks', 'samples'],
  ['zeroCrossingRate', 'samples'],
  ['zeroCrossings', 'samples'],
  ['autoTune', 'samples'],
  ['decomposeStems', 'samples'],
  ['extractNotes', 'samples'],
  ['extractPercussiveEvents', 'samples'],
  ['mergeNotes', 'samples'],
  ['renderNotes', 'samples'],
  ['renderPercussiveEvents', 'samples'],
  ['splitNote', 'samples'],
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
    'realtimeVoiceChangerValidatePresetJson',
    () => sonare.realtimeVoiceChangerValidatePresetJson(5 as never),
    'json',
  ],
  [
    'realtimeVoiceChangerPresetJson',
    () => sonare.realtimeVoiceChangerPresetJson(5 as never),
    'name',
  ],
];

/** Sample-taking entry points that also accept a plain number array as the audio. */
const ARRAY_SAMPLES = new Set(['extractPercussiveEvents', 'renderPercussiveEvents']);

/** Entry points that take a request object, alone or beside a positional form. */
const REQUEST_ONLY: string[] = [
  'decompose',
  'nnFilter',
  'voiceChangeRealtime',
  'masteringRepairDeclickStereo',
  'masteringRepairDeclipStereo',
  'masteringRepairDecrackleStereo',
  'masteringRepairDehumStereo',
  'masteringRepairDenoiseClassicalStereo',
  'masteringRepairDereverbClassicalStereo',
  'masteringRepairDetectTrimRangeStereo',
  'masteringRepairTrimSilenceStereo',
  'alignTakeToReference',
  'amplitudeToDb',
  'assignNoteTargets',
  'chirp',
  'chordFunctions',
  'clicks',
  'cqtToAudio',
  'cyclicTempogram',
  'decomposeNotePitch',
  'decomposeStemsLinked',
  'estimateMeter',
  'fixFrames',
  'fixLength',
  'fourierTempogram',
  'griffinLim',
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
  'melDelta',
  'meteringCrestFactorDbStereo',
  'mixStereo',
  'normalizeStereo',
  'noteSegments',
  'noteTargetsFromSmf',
  'onsetBacktrack',
  'padCenter',
  'pcen',
  'peakPick',
  'pitchTuning',
  'plp',
  'powerToDb',
  'renderPlayback',
  'resample',
  'restoreVocalEditSession',
  'segmentAgglomerative',
  'segmentCrossSimilarity',
  'segmentLagToRecurrence',
  'segmentPathEnhance',
  'segmentRecurrenceMatrix',
  'segmentRecurrenceToLag',
  'segmentSubsegment',
  'splitSilenceCommon',
  'splitSilenceCommonWithReport',
  'streamingLoudnessGain',
  'streamingLoudnessGainStereo',
  'suggestMixScene',
  'suggestMixSceneJson',
  'synthesizeRir',
  'tempogram',
  'tempogramRatio',
  'tone',
  'tonnetz',
  'vectorNormalize',
  'vqtToAudio',
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
    ])('refuses %s as a TypeError naming %s', (label, value) => {
      expect(() => fn(name)(value)).toThrow(TypeError);
      // A number array is a legal sample buffer there, so the missing rate is what is refused.
      const expected =
        label === 'an array' && ARRAY_SAMPLES.has(name)
          ? `${name}: sampleRate must be a number`
          : `${name}: ${argName} must be a Float32Array`;
      expect(() => fn(name)(value)).toThrow(expected);
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
