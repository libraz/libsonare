/**
 * An entry point that takes its audio positionally or as a request object tells a
 * wrong-typed first argument apart from a request: `null`, a number, an array or
 * another typed array is a `TypeError` naming the field, never a property-read
 * failure ("Cannot read properties of null"). And a string the native layer reads
 * through embind is a `TypeError` too, never a raw `BindingError`.
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
