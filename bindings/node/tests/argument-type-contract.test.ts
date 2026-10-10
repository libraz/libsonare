/**
 * An entry point that takes a request object, alone or beside a positional form,
 * refuses a non-object request as a `TypeError` naming the request, never a
 * property-read failure ("Cannot read properties of null"). One taking its audio
 * positionally or as a request names the buffer field instead, as the WASM facade does.
 */

import { describe, expect, it } from 'vitest';
import * as sonare from '../src/index.js';

type Fn = (...args: unknown[]) => unknown;

const fn = (name: string): Fn => (sonare as unknown as Record<string, Fn>)[name];

/** Request entry points whose positional form leads with a number or an array. */
const NUMBER_FIRST = new Set(['chirp', 'tone']);
const ARRAY_FIRST = new Set([
  'fixFrames',
  'masteringRepairDenoiseClassicalLinked',
  'masteringRepairDereverbClassicalLinked',
  'mixStereo',
  'onsetBacktrack',
]);

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

/** Exports whose first argument is not a request, or that need a browser. */
const NOT_A_REQUEST_ENTRY = new Set([
  'chordSymbolFromAnalysis',
  'keyModeFromAnalysis',
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

describe.each(REQUEST_ONLY)('%s', (name) => {
  it('is exported', () => {
    expect(typeof fn(name)).toBe('function');
  });

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

// The WASM table also names entry points that exist only there.
describe.each(BUFFER_FIRST.filter(([name]) => typeof fn(name) === 'function'))(
  '%s',
  (name, argName) => {
    it.each([
      ['null', null],
      ['a number', 5],
      ['an array', [1, 2, 3]],
      ['a Float64Array', new Float64Array(8)],
    ])('refuses %s as a TypeError naming %s', (_label, value) => {
      expect(() => fn(name)(value)).toThrow(TypeError);
      expect(() => fn(name)(value)).toThrow(`${name}: ${argName} must be a Float32Array`);
    });
  },
);

describe('every export', () => {
  it('leaves none failing on a property read of a wrong-typed first argument', () => {
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
