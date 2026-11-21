/**
 * The per-call `validate` option is gone: every entry point that declared it refuses
 * the key by name, and refuses non-finite input because the scan always runs. The
 * roster is every function that declared the option, each with an input that reaches
 * the scan.
 */

import { describe, expect, it } from 'vitest';
import * as api from '../dist/index.js';
import { unknownKeyError, unknownKeyText } from './_helpers.js';

const SR = 22050;

function nanSamples(length = SR): Float32Array {
  const samples = new Float32Array(length).map((_, i) => 0.3 * Math.sin(i / 7));
  samples[Math.min(1000, length - 1)] = Number.NaN;
  return samples;
}

function nanMatrix(rows: number, cols: number) {
  const data = new Float32Array(rows * cols).fill(0.5);
  data[3] = Number.NaN;
  return { rows, cols, data };
}

const RECIPES = {
  mono: () => ({ samples: nanSamples(), sampleRate: SR }),
  stereo: () => ({ left: nanSamples(), right: nanSamples(), sampleRate: SR }),
  channels: () => ({ channels: [nanSamples(), nanSamples()], sampleRate: SR }),
  cqtMag: () => ({ magnitude: nanMatrix(84, 16), sampleRate: SR }),
  stftMag: () => ({ magnitude: nanMatrix(1025, 8), sampleRate: SR }),
  melPower: () => ({ power: nanMatrix(128, 8), sampleRate: SR }),
  mfccCoef: () => ({ coefficients: nanMatrix(20, 8) }),
  mfccCoefSr: () => ({ coefficients: nanMatrix(20, 8), sampleRate: SR }),
  features: () => ({ features: nanMatrix(20, 16) }),
  onsetEnvelope: () => ({ onsetEnvelope: nanSamples(800) }),
  tempogram: () => ({ tempogram: nanMatrix(384, 16) }),
  pair: () => ({ source: nanSamples(), reference: nanSamples(), sampleRate: SR }),
  pairStereo: () => ({
    sourceLeft: nanSamples(),
    sourceRight: nanSamples(),
    referenceLeft: nanSamples(),
    referenceRight: nanSamples(),
    sampleRate: SR,
  }),
  analyzePair: (): Record<string, unknown> => ({
    analysis: 'match.referenceLoudness',
    ...RECIPES.pair(),
  }),
  analyzeStereo: (): Record<string, unknown> => ({
    analysis: 'stereo.monoCompatCheck',
    ...RECIPES.stereo(),
  }),
  applyPair: (): Record<string, unknown> => ({
    processor: 'match.applyMatchEq',
    ...RECIPES.pair(),
  }),
  applyPairStereo: (): Record<string, unknown> => ({
    processor: 'match.applyMatchEq',
    ...RECIPES.pairStereo(),
  }),
  applyProcessor: (): Record<string, unknown> => ({
    processor: 'dynamics.compressor',
    ...RECIPES.mono(),
  }),
  applyProcessorStereo: (): Record<string, unknown> => ({
    processor: 'dynamics.compressor',
    ...RECIPES.stereo(),
  }),
  tracks: () => ({ tracks: [{ id: 'a', channels: [nanSamples()] }], sampleRate: SR }),
  phaseVocoder: (): Record<string, unknown> => ({ ...RECIPES.mono(), speedRatio: 1.1 }),
  peaks: () => ({ channels: [nanSamples()], samplesPerBucket: 256 }),
  pyramid: () => ({ channels: [nanSamples()], samplesPerBucketLevels: [256] }),
} satisfies Record<string, () => Record<string, unknown>>;

const ROSTER: Record<string, keyof typeof RECIPES> = {
  analyzeBpm: 'mono',
  analyzeDynamics: 'mono',
  analyzeImpulseResponse: 'mono',
  analyzePolyphonic: 'mono',
  analyzeRhythm: 'mono',
  analyzeSections: 'mono',
  analyzeTimbre: 'mono',
  autoTune: 'mono',
  bassChroma: 'mono',
  chroma: 'mono',
  chromaCens: 'mono',
  chromaCqt: 'mono',
  cqt: 'mono',
  cqtToAudio: 'cqtMag',
  createVocalEditSession: 'mono',
  decomposeStems: 'mono',
  decomposeStemsLinked: 'channels',
  deemphasis: 'mono',
  detectAcoustic: 'mono',
  detectBeats: 'mono',
  detectBoundaries: 'mono',
  detectBpm: 'mono',
  detectChords: 'mono',
  detectDownbeats: 'mono',
  ebur128LoudnessRangeLu: 'mono',
  estimateTuning: 'mono',
  extractNotes: 'mono',
  extractPercussiveEvents: 'mono',
  fourierTempogram: 'onsetEnvelope',
  frameSignal: 'mono',
  griffinLim: 'stftMag',
  harmonic: 'mono',
  hpss: 'mono',
  hybridCqt: 'mono',
  lufs: 'mono',
  lufsMultichannel: 'channels',
  lufsSeriesMultichannel: 'channels',
  masterAudio: 'mono',
  masterAudioAsync: 'mono',
  masterAudioStereo: 'stereo',
  masterAudioStereoAsync: 'stereo',
  masteringAbMatchLoudness: 'pair',
  masteringAbMatchLoudnessStereo: 'pairStereo',
  masteringAnalyzePair: 'analyzePair',
  masteringAnalyzeStereo: 'analyzeStereo',
  masteringApplyPairProcessor: 'applyPair',
  masteringApplyPairProcessorStereo: 'applyPairStereo',
  masteringApplyProcessor: 'applyProcessor',
  masteringApplyProcessorStereo: 'applyProcessorStereo',
  masteringAssistantSuggest: 'mono',
  masteringAssistantSuggestStereo: 'stereo',
  masteringAudioProfile: 'mono',
  masteringAudioProfileStereo: 'stereo',
  masteringNormalizeLoudness: 'mono',
  masteringRepairAnalyze: 'channels',
  masteringRepairApply: 'channels',
  masteringStreamingPreview: 'mono',
  masteringStreamingPreviewStereo: 'stereo',
  melDelta: 'features',
  melSpectrogram: 'mono',
  melToAudio: 'melPower',
  melToStft: 'melPower',
  mergeNotes: 'mono',
  meteringCrestFactorDb: 'mono',
  meteringCrestFactorDbStereo: 'stereo',
  meteringDcOffset: 'mono',
  meteringDetectClipping: 'mono',
  meteringDynamicRange: 'mono',
  meteringPeakDb: 'mono',
  meteringPhaseScope: 'stereo',
  meteringRmsDb: 'mono',
  meteringSilenceRatio: 'mono',
  meteringSpectrumFrame: 'mono',
  meteringStereoCorrelation: 'stereo',
  meteringStereoWidth: 'stereo',
  meteringTruePeakDbtp: 'mono',
  meteringVectorscope: 'stereo',
  mfcc: 'mono',
  mfccToAudio: 'mfccCoefSr',
  mfccToMel: 'mfccCoef',
  mixingAssistantSuggest: 'tracks',
  momentaryLufs: 'mono',
  nnlsChroma: 'mono',
  normalize: 'mono',
  normalizeStereo: 'stereo',
  noteMove: 'mono',
  noteStretch: 'mono',
  onsetStrength: 'mono',
  onsetStrengthMulti: 'mono',
  percussive: 'mono',
  phaseVocoder: 'phaseVocoder',
  piptrack: 'mono',
  pitchCorrectTimevarying: 'mono',
  pitchCorrectToMidi: 'mono',
  pitchPyin: 'mono',
  pitchShift: 'mono',
  pitchYin: 'mono',
  polyFeatures: 'mono',
  preemphasis: 'mono',
  pseudoCqt: 'mono',
  reassignedSpectrogram: 'mono',
  remix: 'mono',
  renderNotes: 'mono',
  renderPercussiveEvents: 'mono',
  restoreVocalEditSession: 'mono',
  rmsEnergy: 'mono',
  roomMorph: 'mono',
  shortTermLufs: 'mono',
  spectralBandwidth: 'mono',
  spectralCentroid: 'mono',
  spectralContrast: 'mono',
  spectralEdit: 'mono',
  spectralFlatness: 'mono',
  spectralFlux: 'mono',
  spectralRolloff: 'mono',
  splitNote: 'mono',
  splitSilence: 'mono',
  stft: 'mono',
  streamingLoudnessGain: 'mono',
  streamingLoudnessGainStereo: 'stereo',
  tempogramRatio: 'tempogram',
  timeStretch: 'mono',
  transcribe: 'mono',
  trim: 'mono',
  trimSilence: 'mono',
  voiceChange: 'mono',
  realtimeVoiceChangerRender: 'channels',
  vqt: 'mono',
  vqtToAudio: 'cqtMag',
  waveformPeakPyramid: 'pyramid',
  waveformPeaks: 'peaks',
  zeroCrossingRate: 'mono',
  zeroCrossings: 'mono',
};

type Entry = (request: Record<string, unknown>) => unknown;

function entry(name: string): Entry {
  const fn = (api as unknown as Record<string, unknown>)[name];
  if (typeof fn !== 'function') {
    throw new Error(`${name} is not exported`);
  }
  return fn as Entry;
}

async function errorOf(call: () => unknown): Promise<unknown> {
  try {
    await call();
  } catch (error) {
    return error;
  }
  return undefined;
}

describe('the validate option is refused', () => {
  it.each(Object.keys(ROSTER))('%s refuses validate as an unknown request key', async (name) => {
    const error = await errorOf(() => entry(name)({ validate: false }));
    expect(error).toBeInstanceOf(TypeError);
    expect((error as Error).message).toMatch(unknownKeyText(`${name}: unknown key validate`));
  });

  it.each(Object.entries(ROSTER).filter(([name]) => name !== 'mixingAssistantSuggest'))(
    '%s refuses non-finite input',
    async (name, recipe) => {
      const error = await errorOf(() => entry(name)(RECIPES[recipe]()));
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toMatch(new RegExp(`^${name}: .*(NaN or Inf|non-finite)`));
    },
  );

  it('mixingAssistantSuggest reports a non-finite track as excluded rather than refusing it', () => {
    const result = api.mixingAssistantSuggest({
      tracks: [{ id: 'a', channels: [nanSamples()] }],
      sampleRate: SR,
    });
    expect(result.tracks[0].usable).toBe(false);
    expect(result.tracks[0].exclusionReason).toBe('track has non-finite samples');
  });

  it('renderOffline refuses validate and scans every channel', () => {
    const engine = new api.RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      const block = new Float32Array(128);
      expect(() => engine.renderOffline({ channels: [block], validate: false } as never)).toThrow(
        unknownKeyError('RealtimeEngine.renderOffline: unknown key validate'),
      );
      const poisoned = new Float32Array(128);
      poisoned[5] = Number.NaN;
      expect(() => engine.renderOffline({ channels: [poisoned, block] })).toThrow(
        new RangeError(
          'RealtimeEngine.renderOffline: channels[0] contains a non-finite sample at index 5',
        ),
      );
    } finally {
      engine.destroy();
    }
  });
});
