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
  'onsetBacktrack',
]);

/** [entry point, name of its first (buffer) argument]. */
const BUFFER_FIRST: Array<[string, string]> = [
  ['analyze', 'samples'],
  ['analyzeBpm', 'samples'],
  ['analyzeDynamics', 'samples'],
  ['analyzeImpulseResponse', 'samples'],
  ['analyzeMelody', 'samples'],
  ['analyzePolyphonic', 'samples'],
  ['analyzeRhythm', 'samples'],
  ['analyzeSections', 'samples'],
  ['analyzeTimbre', 'samples'],
  ['autoTune', 'samples'],
  ['bassChroma', 'samples'],
  ['chordFunctionalAnalysis', 'samples'],
  ['chroma', 'samples'],
  ['chromaCens', 'samples'],
  ['chromaCqt', 'samples'],
  ['cqt', 'samples'],
  ['decomposeStems', 'samples'],
  ['deemphasis', 'samples'],
  ['detectAcoustic', 'samples'],
  ['detectBeats', 'samples'],
  ['detectBoundaries', 'samples'],
  ['detectBpm', 'samples'],
  ['detectChords', 'samples'],
  ['detectDownbeats', 'samples'],
  ['detectKey', 'samples'],
  ['detectKeyCandidates', 'samples'],
  ['detectOnsets', 'samples'],
  ['ebur128LoudnessRangeLu', 'samples'],
  ['estimateRoom', 'samples'],
  ['estimateTuning', 'samples'],
  ['extractNotes', 'samples'],
  ['extractPercussiveEvents', 'samples'],
  ['frameSignal', 'samples'],
  ['harmonic', 'samples'],
  ['hpss', 'samples'],
  ['hybridCqt', 'samples'],
  ['lufs', 'samples'],
  ['masterAudio', 'samples'],
  ['masteringApplyProcessor', 'samples'],
  ['masteringAssistantSuggest', 'samples'],
  ['masteringAudioProfile', 'samples'],
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
  ['masteringNormalizeLoudness', 'samples'],
  ['masteringStreamingPreview', 'samples'],
  ['melSpectrogram', 'samples'],
  ['mergeNotes', 'samples'],
  ['meteringCrestFactorDb', 'samples'],
  ['meteringDcOffset', 'samples'],
  ['meteringDetectClipping', 'samples'],
  ['meteringDynamicRange', 'samples'],
  ['meteringPeakDb', 'samples'],
  ['meteringRmsDb', 'samples'],
  ['meteringSilenceRatio', 'samples'],
  ['meteringSpectrum', 'samples'],
  ['meteringSpectrumFrame', 'samples'],
  ['meteringTruePeakDbtp', 'samples'],
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
  ['renderNotes', 'samples'],
  ['renderPercussiveEvents', 'samples'],
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
  ['splitNote', 'samples'],
  ['splitSilence', 'samples'],
  ['stft', 'samples'],
  ['streamingLoudnessGain', 'samples'],
  ['timeStretch', 'samples'],
  ['transcribe', 'samples'],
  ['trim', 'samples'],
  ['trimSilence', 'samples'],
  ['voiceChange', 'samples'],
  ['vqt', 'samples'],
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

/** Entry points that also take a plain number array as their audio, so an array reaches the next argument. */
const ARRAY_AUDIO = new Set(['extractPercussiveEvents', 'renderPercussiveEvents']);

const RAW_PROPERTY_READ = /Cannot read propert|Cannot destructure|undefined is not|is not iterable/;

const REQUEST_ONLY: string[] = [
  'alignTakeToReference',
  'amplitudeToDb',
  'assignNoteTargets',
  'chirp',
  'chordFunctions',
  'clicks',
  'cqtToAudio',
  'cyclicTempogram',
  'decompose',
  'decomposeNotePitch',
  'decomposeStemsLinked',
  'estimateMeter',
  'fixFrames',
  'fixLength',
  'fourierTempogram',
  'griffinLim',
  'masteringAbMatchLoudness',
  'masteringAbMatchLoudnessStereo',
  'masteringAnalyzePair',
  'masteringAnalyzeStereo',
  'masteringApplyPairProcessor',
  'masteringApplyPairProcessorStereo',
  'masteringApplyProcessorStereo',
  'masteringRepairAnalyze',
  'masteringRepairApply',
  'masteringRepairDeclickStereo',
  'masteringRepairDeclipStereo',
  'masteringRepairDecrackleStereo',
  'masteringRepairDehumStereo',
  'masteringRepairDenoiseClassicalLinked',
  'masteringRepairDenoiseClassicalStereo',
  'masteringRepairDereverbClassicalLinked',
  'masteringRepairDereverbClassicalStereo',
  'masteringRepairDetectTrimRangeStereo',
  'masteringRepairTrimSilenceStereo',
  'melDelta',
  'mixingAssistantSuggest',
  'lufsMultichannel',
  'lufsSeriesMultichannel',
  'meteringCrestFactorDbStereo',
  'meteringPhaseScope',
  'meteringStereoCorrelation',
  'meteringStereoWidth',
  'meteringVectorscope',
  'waveformPeakPyramid',
  'waveformPeaks',
  'mixStereo',
  'nnFilter',
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
  'streamingLoudnessGainStereo',
  'synthesizeRir',
  'tempogram',
  'tempogramRatio',
  'tone',
  'tonnetz',
  'vectorNormalize',
  'realtimeVoiceChangerRender',
  'vqtToAudio',
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
      const message =
        _label === 'an array' && ARRAY_AUDIO.has(name)
          ? `${name}: sampleRate must be an integer`
          : `${name}: ${argName} must be a Float32Array`;
      expect(() => fn(name)(value)).toThrow(TypeError);
      expect(() => fn(name)(value)).toThrow(message);
    });
  },
);

// Request-only entry points refuse a wrong-typed buffer field inside the request.
const REQUEST_FIELD = (name: string, field: string, bad: unknown, rest: object) => ({
  name,
  field,
  request: { ...rest, [field]: bad },
});
const F32 = new Float32Array(8);
const STEREO_REPAIR = [
  'masteringRepairDeclickStereo',
  'masteringRepairDeclipStereo',
  'masteringRepairDecrackleStereo',
  'masteringRepairDehumStereo',
  'masteringRepairDenoiseClassicalStereo',
  'masteringRepairDereverbClassicalStereo',
  'masteringRepairDetectTrimRangeStereo',
  'masteringRepairTrimSilenceStereo',
];
describe.each([
  ...STEREO_REPAIR.flatMap((name) => [
    REQUEST_FIELD(name, 'left', 5, { right: F32, sampleRate: 44100 }),
    REQUEST_FIELD(name, 'right', new Float64Array(8), { left: F32, sampleRate: 44100 }),
  ]),
])('$name request field $field', ({ name, field, request }) => {
  it('refuses a wrong-typed buffer as a TypeError naming the field', () => {
    expect(() => fn(name)(request)).toThrow(TypeError);
    expect(() => fn(name)(request)).toThrow(`${name}: ${field} must be a Float32Array`);
  });
});

describe.each([
  ['decompose', { s: 5 }, 'decompose: s must be a Matrix2D'],
  ['nnFilter', { s: 5 }, 'nnFilter: s must be a Matrix2D'],
  [
    'realtimeVoiceChangerRender',
    { channels: 5, sampleRate: 44100 },
    'realtimeVoiceChangerRender: channels must be an array of Float32Array',
  ],
  [
    'realtimeVoiceChangerRender',
    { channels: [new Float64Array(8)], sampleRate: 44100 },
    'realtimeVoiceChangerRender: channels[0] must be a Float32Array',
  ],
])('%s request field', (name, request, message) => {
  it(`refuses ${message}`, () => {
    expect(() => fn(name as string)(request)).toThrow(TypeError);
    expect(() => fn(name as string)(request)).toThrow(message as string);
  });
});

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
