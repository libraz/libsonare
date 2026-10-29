/**
 * Every audio-taking one-shot refuses its audio input the same way: empty or
 * non-finite samples and an out-of-range or non-integer sample rate are a
 * `RangeError`, a wrong-typed `samples` or `sampleRate` is a `TypeError`, and no
 * raw `SonareError` from the core is the first refusal. The suite is closed over
 * the package's exports, so a new audio one-shot without the preflight fails here.
 */

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { beforeAll, describe, expect, it } from 'vitest';
import * as sonare from '../src/index.js';
import * as nodeValidation from '../src/validation.js';

const SR = 44100;
const N = 22050;
const FRAMES = 44;

const tone = (): Float32Array =>
  Float32Array.from({ length: N }, (_, i) => 0.3 * Math.sin((2 * Math.PI * 220 * i) / SR));

/** Buffer slot `slot` of a call carries the probed `samples`; every other slot is valid audio. */
const slot = (which: number, index: number, samples: Float32Array): Float32Array =>
  which === index ? samples : tone();

type Fn = (request: unknown) => unknown;
type Call = (samples: Float32Array, sampleRate: number, which: number) => unknown;

interface Entry {
  name: string;
  call: Call;
  /** Number of audio buffers the entry takes; each is probed in turn. Default 1. */
  buffers?: number;
  /** The entry takes no sample rate. */
  noRate?: boolean;
  /** An empty or non-finite buffer is reported by the core as an excluded track, not refused. */
  reportsDegenerate?: boolean;
  /** No valid call is built for this entry (its other inputs need state this table cannot make). */
  noValid?: boolean;
}

const fn = (name: string): Fn => (sonare as unknown as Record<string, Fn>)[name];
const f0 = () => new Float32Array(FRAMES).fill(220);

/** `{ samples, sampleRate, ...extra }`. */
const mono = (name: string, extra: Record<string, unknown> = {}): Entry => ({
  name,
  call: (samples, sampleRate) => fn(name)({ samples, sampleRate, ...extra }),
});
/** `{ left, right, sampleRate, ...extra }`. */
const stereo = (name: string, extra: Record<string, unknown> = {}): Entry => ({
  name,
  buffers: 2,
  call: (s, sampleRate, w) =>
    fn(name)({ left: slot(w, 0, s), right: slot(w, 1, s), sampleRate, ...extra }),
});
/** `{ source, reference, sampleRate, ...extra }`. */
const pair = (name: string, extra: Record<string, unknown> = {}): Entry => ({
  name,
  buffers: 2,
  call: (s, sampleRate, w) =>
    fn(name)({ source: slot(w, 0, s), reference: slot(w, 1, s), sampleRate, ...extra }),
});
/** `{ channels: [..], sampleRate }`. */
const linked = (name: string, extra: Record<string, unknown> = {}): Entry => ({
  name,
  buffers: 2,
  call: (s, sampleRate, w) =>
    fn(name)({ channels: [slot(w, 0, s), slot(w, 1, s)], sampleRate, ...extra }),
});
const notes = (name: string, extra: Record<string, unknown> = {}): Entry =>
  mono(name, { f0Hz: f0(), frameRate: 86.13, ...extra });

const TABLE: Entry[] = [
  // Music analysis (quick_analysis, feature_music)
  ...[
    'detectBpm',
    'detectKey',
    'detectKeyCandidates',
    'detectOnsets',
    'detectBeats',
    'detectDownbeats',
    'detectChords',
    'analyze',
    'analyzeAsync',
    'analyzeWithProgress',
    'analyzeImpulseResponse',
    'detectAcoustic',
    'estimateRoom',
    'analyzeBpm',
    'analyzeRhythm',
    'analyzeDynamics',
    'analyzeTimbre',
    'nnlsChroma',
    'cqt',
    'pseudoCqt',
    'hybridCqt',
    'vqt',
    'analyzeSections',
    'detectBoundaries',
    'analyzeMelody',
    'onsetEnvelope',
    'onsetStrengthMulti',
    'lufs',
    'momentaryLufs',
    'shortTermLufs',
    'ebur128LoudnessRange',
  ].map((name) => mono(name, name === 'analyzeWithProgress' ? { onProgress: () => {} } : {})),
  mono('chordFunctionalAnalysis', { keyRoot: 0 }),
  mono('roomMorph'),
  stereo('roomMorphStereo'),
  // Spectral and pitch features
  ...[
    'trim',
    'stft',
    'stftDb',
    'chroma',
    'chromaCens',
    'chromaCqt',
    'bassChroma',
    'melSpectrogram',
    'mfcc',
    'reassignedSpectrogram',
    'spectralCentroid',
    'spectralContrast',
    'polyFeatures',
    'spectralBandwidth',
    'spectralRolloff',
    'spectralFlatness',
    'spectralFlux',
    'zeroCrossingRate',
    'rmsEnergy',
    'piptrack',
    'pitchYin',
    'pitchPyin',
    'estimateTuning',
    'phaseVocoder',
    'decomposeStems',
    'hpssWithResidual',
    'hpss',
    'harmonic',
    'percussive',
  ].map((name) => mono(name, name === 'phaseVocoder' ? { rate: 1.1 } : {})),
  mono('lufsInterleaved', { channels: 1 }),
  mono('lufsSeriesInterleaved', { channels: 1 }),
  mono('remix', { intervals: [0, 4096] }),
  mono('remixAlignedIntervals', { intervals: [0, 4096] }),
  linked('decomposeStemsLinked'),
  // Silence and waveform helpers without a sample rate
  ...['trimSilence', 'splitSilence'].map(
    (name): Entry => ({
      name,
      noRate: true,
      call: (samples) => fn(name)({ samples }),
    }),
  ),
  ...['splitSilenceCommon', 'splitSilenceCommonWithReport'].map(
    (name): Entry => ({
      name,
      buffers: 2,
      noRate: true,
      call: (s, _sr, w) => fn(name)({ signals: [slot(w, 0, s), slot(w, 1, s)] }),
    }),
  ),
  ...['waveformPeaks', 'waveformPeakPyramid'].map(
    (name): Entry => ({
      name,
      noRate: true,
      call: (samples) => fn(name)({ samples, channels: 1 }),
    }),
  ),
  // Effects
  mono('timeStretch', { rate: 1.1 }),
  mono('pitchShift', { semitones: 1 }),
  mono('pitchCorrectToMidi'),
  mono('pitchCorrectToMidiTimevarying', { f0Hz: f0(), targetMidi: 57 }),
  mono('pitchCorrectTimevarying', { f0Hz: f0() }),
  mono('autoTune'),
  mono('voiceChange'),
  mono('voiceChangeRealtime'),
  mono('spectralEdit'),
  mono('noteStretch'),
  mono('noteMove'),
  mono('extractPercussiveEvents'),
  mono('renderPercussiveEvents', { events: [] }),
  notes('extractNotes', { voicedProb: new Float32Array(FRAMES).fill(1) }),
  mono('renderNotes', { notes: [] }),
  ...(
    [
      ['splitNote', { index: 0, frame: 10 }],
      ['mergeNotes', { first: 0, last: 1 }],
    ] as [string, Record<string, unknown>][]
  ).map(
    ([name, extra]): Entry => ({
      name,
      noValid: true,
      call: (samples, sampleRate) =>
        fn(name)({
          samples,
          sampleRate,
          f0Hz: f0(),
          frameRate: 86.13,
          notes: [],
          ...extra,
        }),
    }),
  ),
  // Mastering chain and processors
  ...['masterAudio', 'masterAudioAsync'].map((name) => mono(name, { preset: 'pop' })),
  ...['masterAudioStereo', 'masterAudioStereoAsync'].map((name) => stereo(name, { preset: 'pop' })),
  mono('masteringChain', { onProgress: () => {} }),
  stereo('masteringChainStereo', { onProgress: () => {} }),
  mono('streamingLoudnessGain'),
  stereo('streamingLoudnessGainStereo'),
  mono('normalize'),
  stereo('normalizeStereo'),
  mono('mastering'),
  mono('masteringProcess', { processorName: 'dynamics.compressor' }),
  stereo('masteringProcessStereo', { processorName: 'dynamics.compressor' }),
  pair('masteringPairProcess', { processorName: 'match.abSwitch' }),
  {
    name: 'masteringPairProcessStereo',
    buffers: 4,
    call: (s, sampleRate, w) =>
      fn('masteringPairProcessStereo')({
        processorName: 'match.abCrossfade',
        sourceLeft: slot(w, 0, s),
        sourceRight: slot(w, 1, s),
        referenceLeft: slot(w, 2, s),
        referenceRight: slot(w, 3, s),
        sampleRate,
      }),
  },
  pair('masteringPairAnalyze', { analysisName: 'match.referenceLoudness' }),
  pair('masteringAbMatchLoudness'),
  {
    name: 'masteringAbMatchLoudnessStereo',
    buffers: 4,
    call: (s, sampleRate, w) =>
      fn('masteringAbMatchLoudnessStereo')({
        sourceLeft: slot(w, 0, s),
        sourceRight: slot(w, 1, s),
        referenceLeft: slot(w, 2, s),
        referenceRight: slot(w, 3, s),
        sampleRate,
      }),
  },
  stereo('masteringStereoAnalyze', { analysisName: 'stereo.monoCompatCheck' }),
  ...[
    'masteringAssistantSuggest',
    'masteringAssistantSuggestChain',
    'masteringAudioProfile',
    'masteringStreamingPreview',
    'masteringDynamicsCompressor',
    'masteringDynamicsGate',
    'masteringDynamicsTransientShaper',
  ].map((name) => mono(name)),
  ...[
    'masteringAssistantSuggestStereo',
    'masteringAssistantSuggestChainStereo',
    'masteringAudioProfileStereo',
    'masteringStreamingPreviewStereo',
  ].map((name) => stereo(name)),
  // Repair
  ...[
    'masteringRepairDeclick',
    'masteringRepairDeclip',
    'masteringRepairDecrackle',
    'masteringRepairDetectClicks',
    'masteringRepairDetectClipping',
    'masteringRepairDetectCrackle',
    'masteringRepairDenoiseClassical',
    'masteringRepairDehum',
    'masteringRepairDetectNoiseFloor',
    'masteringRepairDetectHum',
    'masteringRepairDereverbClassical',
    'masteringRepairDetectReverb',
    'masteringRepairTrimSilence',
    'masteringRepairDetectTrimRange',
  ].map((name) => mono(name)),
  ...[
    'masteringRepairDeclickStereo',
    'masteringRepairDeclipStereo',
    'masteringRepairDecrackleStereo',
    'masteringRepairDenoiseClassicalStereo',
    'masteringRepairDehumStereo',
    'masteringRepairDereverbClassicalStereo',
    'masteringRepairTrimSilenceStereo',
    'masteringRepairDetectTrimRangeStereo',
  ].map((name) => stereo(name)),
  linked('masteringRepairDenoiseClassicalLinked'),
  linked('masteringRepairDereverbClassicalLinked'),
  linked('masteringRepairAnalyze'),
  linked('masteringRepairApply', { stages: [] }),
  // Metering
  ...[
    'meteringPeakDb',
    'meteringRmsDb',
    'meteringSilenceRatio',
    'meteringCrestFactorDb',
    'meteringDcOffset',
    'meteringTruePeakDb',
    'meteringDetectClipping',
    'meteringDynamicRange',
    'meteringSpectrum',
  ].map((name) => mono(name)),
  mono('meteringSpectrumFrame'),
  ...[
    'meteringCrestFactorDbStereo',
    'meteringStereoCorrelation',
    'meteringStereoWidth',
    'meteringVectorscope',
    'meteringPhaseScope',
  ].map((name) => stereo(name)),
  // Mixing, playback, alignment, transcription, sessions
  {
    name: 'mixStereo',
    buffers: 2,
    call: (s, sampleRate, w) =>
      fn('mixStereo')({
        leftChannels: [slot(w, 0, s)],
        rightChannels: [slot(w, 1, s)],
        sampleRate,
      }),
  },
  ...['suggestMixScene', 'suggestMixSceneJson'].map(
    (name): Entry => ({
      name,
      reportsDegenerate: true,
      call: (samples, sampleRate) => fn(name)({ tracks: [{ id: 'a', left: samples }], sampleRate }),
    }),
  ),
  mono('renderPlayback', {
    channels: 1,
    config: { input: { layout: 'mono' }, target: { kind: 'speakers', layout: 'stereo' } },
  }),
  {
    name: 'alignTakeToReference',
    buffers: 2,
    call: (s, sampleRate, w) =>
      fn('alignTakeToReference')({
        reference: slot(w, 0, s),
        take: slot(w, 1, s),
        sampleRate,
      }),
  },
  mono('analyzePolyphonic'),
  mono('transcribe'),
  mono('createVocalEditSession'),
  { ...mono('restoreVocalEditSession', { state: new Uint8Array(0) }), noValid: true },
];

/**
 * Exports that take no audio, with the reason. Everything else exported as a
 * function must be in the table.
 */
const NOT_AUDIO: string[] = [
  // Module state, version and capability queries.
  ...[
    'synthEngineParamInfo',
    'synthPatchParamInfo',
    'version',
    'capabilities',
    'capabilityCatalog',
    'abiVersion',
    'engineAbiVersion',
    'projectAbiVersion',
    'voiceChangerAbiVersion',
    'vocalEditAvailable',
    'vocalEditApiVersion',
    'hasFfmpegSupport',
    'isSonareError',
  ],
  // Analysis-to-annotation conversions over a chord or a mode name.
  ...['chordSymbolFromAnalysis', 'keyModeFromAnalysis'],
  // Name, catalog, preset and enum lookups.
  ...[
    'controllerProfileNames',
    'masteringAmpPresetCatalog',
    'masteringInsertNames',
    'masteringInsertParamInfo',
    'masteringInsertParamNames',
    'masteringInsertTiming',
    'masteringPairAnalysisNames',
    'masteringPairProcessorNames',
    'masteringPlatformNames',
    'masteringPresetNames',
    'masteringPresetParams',
    'masteringProcessorCatalog',
    'masteringProcessorNames',
    'masteringStereoAnalysisNames',
    'mixSourceClassFromName',
    'mixSourceClassNames',
    'mixingScenePresetJson',
    'mixingScenePresetNames',
    'realtimeVoiceChangerPresetConfig',
    'realtimeVoiceChangerPresetJson',
    'realtimeVoiceChangerPresetNames',
    'synthEnumTables',
    'synthGsDrumKitIsVoicedApart',
    'synthGsDrumKitName',
    'synthGsVariationIsVoicedApart',
    'synthPresetNames',
    'synthPresetPatch',
    'validateRealtimeVoiceChangerPresetJson',
    'voiceCharacterPresetId',
    'masteringRepairDereverbConfigForRoom',
    'masteringRepairNoiseBandBins',
  ],
  // Generators, conversions and scalar helpers over numbers or note names.
  ...[
    'tone',
    'chirp',
    'synthesizeRir',
    'hzToMel',
    'melToHz',
    'hzToMidi',
    'midiToHz',
    'hzToNote',
    'noteToHz',
    'framesToTime',
    'timeToFrames',
    'framesToSamples',
    'samplesToFrames',
    'pitchTuning',
    'tuningToReferenceHz',
    'referenceHzToTuning',
    'scaleCorrectionSemitones',
    'scaleMaskForMode',
    'scalePitchClassEnabled',
    'scaleQuantizeMidi',
    'roomGeometryFromEstimate',
  ],
  // Array utilities over values, envelopes or tracks; they carry no audio signal.
  ...[
    'amplitudeToDb',
    'dbToAmplitude',
    'dbToPower',
    'powerToDb',
    'vectorNormalize',
    'peakPick',
    'padCenter',
    'fixLength',
    'fixFrames',
    'frameSignal',
    'clicks',
    'deemphasis',
    'preemphasis',
    'zeroCrossings',
    'onsetBacktrack',
    'pcen',
    'tonnetz',
    'melDelta',
    'nnFilter',
    'decompose',
    'downmix',
  ],
  // Input is an onset envelope, a spectrogram, a chroma matrix or an F0 track rather than audio.
  ...[
    'tempogram',
    'cyclicTempogram',
    'fourierTempogram',
    'plp',
    'tempogramRatio',
    'cqtToAudio',
    'vqtToAudio',
    'griffinLim',
    'melToAudio',
    'mfccToAudio',
    'melToStft',
    'mfccToMel',
    'segmentAgglomerative',
    'segmentCrossSimilarity',
    'segmentLagToRecurrence',
    'segmentPathEnhance',
    'segmentRecurrenceMatrix',
    'segmentRecurrenceToLag',
    'segmentSubsegment',
    'chordFunctions',
    'estimateMeter',
    'decomposeNotePitch',
    'noteSegments',
    'assignNoteTargets',
    'noteTargetsFromSmf',
  ],
  // A rate converter: a zero-length conversion stays legal and its two rates keep their own checks.
  'resample',
  // Container decoding.
  'decodeChannels',
];

const PROBES = TABLE.flatMap((entry) =>
  Array.from({ length: entry.buffers ?? 1 }, (_, which) => [entry.name, entry, which] as const),
);
const RATE_PROBES = TABLE.filter((entry) => !entry.noRate).map((entry) => [entry.name, entry]);

/** Runs `run`, which may throw or return a promise, and resolves to the error or `undefined`. */
async function refusal(run: () => unknown): Promise<unknown> {
  try {
    await run();
    return undefined;
  } catch (error) {
    return error;
  }
}

function expectError(error: unknown, type: ErrorConstructor): void {
  expect(error, 'expected a refusal').toBeDefined();
  expect(error).toBeInstanceOf(type);
  expect((error as Error).constructor, String(error)).toBe(type);
}

describe('audio preflight contract', () => {
  it('classifies every exported function', () => {
    const exported = Object.entries(sonare)
      .filter(([name, value]) => typeof value === 'function' && /^[a-z]/.test(name))
      .map(([name]) => name);
    const known = new Set([...TABLE.map((e) => e.name), ...NOT_AUDIO]);
    expect(exported.filter((name) => !known.has(name))).toEqual([]);
    expect([...known].filter((name) => !exported.includes(name))).toEqual([]);
    expect(TABLE.length).toBe(new Set(TABLE.map((e) => e.name)).size);
    expect(NOT_AUDIO.length).toBe(new Set(NOT_AUDIO).size);
  });

  it.each(TABLE.filter((entry) => !entry.noValid).map((entry) => [entry.name, entry]))(
    '%s accepts valid audio',
    async (_name, entry) => {
      const error = await refusal(() => (entry as Entry).call(tone(), SR, 0));
      if (error instanceof Error && /unavailable/.test(error.message)) {
        return;
      }
      expect(error).toBeUndefined();
    },
  );

  it.each(PROBES)('%s refuses empty audio in buffer %#', async (_name, entry, which) => {
    const error = await refusal(() => entry.call(new Float32Array(0), SR, which));
    if (entry.reportsDegenerate) {
      expect(error).toBeUndefined();
      return;
    }
    expectError(error, RangeError);
  });

  it.each(PROBES)('%s refuses non-finite audio in buffer %#', async (_name, entry, which) => {
    const bad = tone();
    bad[10] = Number.NaN;
    const error = await refusal(() => entry.call(bad, SR, which));
    if (entry.reportsDegenerate) {
      expect(error).toBeUndefined();
      return;
    }
    expectError(error, RangeError);
  });

  it.each(PROBES)('%s refuses a non-audio samples value in buffer %#', async (_n, entry, which) => {
    const error = await refusal(() => entry.call(5 as unknown as Float32Array, SR, which));
    expectError(error, TypeError);
  });

  it.each(RATE_PROBES)('%s refuses an out-of-range or fractional rate', async (_name, entry) => {
    expectError(await refusal(() => (entry as Entry).call(tone(), 1000, 0)), RangeError);
    expectError(await refusal(() => (entry as Entry).call(tone(), 44100.5, 0)), RangeError);
  });

  it.each(RATE_PROBES)('%s refuses a non-numeric rate', async (_name, entry) => {
    const error = await refusal(() =>
      (entry as Entry).call(tone(), '44100' as unknown as number, 0),
    );
    expectError(error, TypeError);
  });
});

describe('wrong-typed arguments are TypeErrors that name the argument', () => {
  const x = tone();

  it('detectBpm names samples and sampleRate', () => {
    expect(() => sonare.detectBpm(5 as never, SR)).toThrow(
      new TypeError('detectBpm: samples must be a Float32Array'),
    );
    expect(() => sonare.detectBpm(x, '44100' as never)).toThrow(
      new TypeError('detectBpm: sampleRate must be a number'),
    );
  });

  it('Mixer.fromSceneJson refuses a non-string scene as a TypeError', () => {
    expect(() => sonare.Mixer.fromSceneJson(null as never, SR, 512)).toThrow(TypeError);
  });
});

/**
 * The runtime-independent validators exist once per package, because the Node
 * and WASM packages compile separately from their own `src/`. One boundary
 * corpus runs through both copies and each answer -- the value returned, or the
 * error class, message and code -- must be identical. A validator that both
 * packages export and the corpus does not drive fails the closure check, so a
 * new duplicate cannot be added without joining the corpus.
 */
describe('Node and WASM validators answer one boundary corpus identically', () => {
  // Request-object key checking reads each package's own declared request
  // shapes, which stay surface-local by design.
  const SURFACE_LOCAL = new Set(['optionsBag', 'requestObject']);
  const f32 = (...values: number[]): Float32Array => Float32Array.from(values);
  const fn = 'probe';

  const CORPUS: Record<string, unknown[][]> = {
    assertSamples: [
      [fn, f32(), true],
      [fn, f32(1, Number.NaN), true],
      [fn, f32(Number.NaN), false],
      [fn, [1, Number.POSITIVE_INFINITY], true, 'left'],
      [fn, f32(1, 2), true],
    ],
    assertSamplesInWindow: [
      [fn, f32(0, Number.NaN, 0, 0), true, 0, 1],
      [fn, f32(0, Number.NaN, 0, 0), true, 0, 2],
      [fn, f32(0, Number.NaN, 0, 0), true, 1.5, 1],
      [fn, f32(0, Number.NaN, 0, 0), true, -3, 10],
      [fn, f32(0, Number.NaN), false, 0, 2],
      [fn, f32(), true, 0, 1],
    ],
    assertFiniteScalar: [
      [fn, Number.NaN, 'gain'],
      [fn, Number.NEGATIVE_INFINITY, 'gain'],
      [fn, -1, 'gain'],
    ],
    assertSampleRate: [
      [fn, '44100'],
      [fn, 44100.5],
      [fn, 7999],
      [fn, 8000],
      [fn, 384000],
      [fn, 384001],
      [fn, Number.NaN, 'targetSampleRate'],
    ],
    assertAudioInput: [
      [fn, [1, 2], 44100],
      [fn, f32(1), 44100.5],
      [fn, f32(), 44100],
      [fn, f32(Number.NaN), 44100, { validate: false }],
      [fn, f32(Number.NaN), 44100, {}, 'reference'],
      [fn, f32(1), 44100],
    ],
    assertAudioSamples: [
      [fn, null],
      [fn, f32(Number.NaN), {}, 'left'],
      [fn, f32(1)],
    ],
    assertAudioChannels: [
      [fn, 'x', 44100],
      [fn, [], 44100],
      [fn, [f32(1), f32()], 44100],
      [fn, [f32(1)], 44100.5],
      [fn, [f32(1), f32(2)], 44100],
    ],
    assertInt32: [
      [fn, 2 ** 31, 'kernel'],
      [fn, -(2 ** 31), 'kernel'],
      [fn, 2 ** 31 - 1, 'kernel'],
      [fn, 1.5, 'kernel'],
      [fn, Number.NaN, 'kernel'],
    ],
    assertHpssKernels: [
      [fn, 31, 31],
      [fn, 2 ** 32 + 1, 31],
      [fn, 31, 0.5],
    ],
    assertPositiveInteger: [
      [fn, 0, 'hopLength'],
      [fn, -1, 'hopLength'],
      [fn, 1.5, 'hopLength'],
      [fn, 2 ** 31, 'hopLength'],
      [fn, 1, 'hopLength'],
    ],
    assertIntegerValue: [
      [fn, '3', 'count'],
      [fn, 3.5, 'count'],
      [fn, 3, 'count'],
    ],
    assertEvenIntegerAtLeast: [
      [fn, 3, 'nFft', 4, 2 ** 30],
      [fn, 5, 'nFft', 4, 2 ** 30],
      [fn, 2 ** 31, 'nFft', 4, 2 ** 30],
      [fn, 4.5, 'nFft', 4, 2 ** 30],
      [fn, 8, 'nFft', 4, 2 ** 30],
    ],
    assertBoundedInteger: [
      [fn, 5, 'mode', 0, 4],
      [fn, -1, 'mode', 0, 4],
      [fn, 2.5, 'mode', 0, 4],
      [fn, 2, 'mode', 0, 4],
    ],
    assertU7: [
      [fn, 128, 'note'],
      [fn, -1, 'note'],
      [fn, 0.5, 'note'],
      [fn, 127, 'note'],
    ],
    assertNibble: [
      [fn, 16, 'channel'],
      [fn, -1, 'channel'],
      [fn, 15, 'channel'],
    ],
    assertU32: [
      [fn, 2 ** 32, 'id'],
      [fn, -1, 'id'],
      [fn, 1.5, 'id'],
      [fn, 2 ** 32 - 1, 'id'],
    ],
    assertInterleavedSamples: [
      [fn, f32(1, 2, 3), 2, true],
      [fn, f32(1, 2), 2.5, true],
      [fn, [1, 2], 2, true],
      [fn, f32(Number.NaN, 1), 2, true],
      [fn, f32(Number.NaN, 1), 2, false],
      [fn, f32(), 1, true],
      [fn, f32(1, 2), 2, true],
    ],
    secondsToSampleCount: [
      [fn, '1', 48000, 'startSec'],
      [fn, Number.NaN, 48000, 'startSec'],
      [fn, 1e300, 48000, 'startSec'],
      [fn, 0.5, 48000, 'startSec'],
      [fn, -0.25, 44100, 'offsetSec'],
    ],
    resolveSampleBound: [
      [fn, 5, 0.1, 48000, 'start', 'startSec'],
      [fn, undefined, undefined, 48000, 'start', 'startSec'],
      [fn, null, 0.5, 48000, 'start', 'startSec'],
      [fn, -1, undefined, 48000, 'start', 'startSec'],
      [fn, -1, undefined, 48000, 'offset', 'offsetSec', 'signed'],
      [fn, undefined, -0.5, 48000, 'start', 'startSec'],
      [fn, Number.NaN, undefined, 48000, 'start', 'startSec'],
      [fn, 'x', undefined, 48000, 'start', 'startSec'],
      [fn, 12, undefined, 48000, 'start', 'startSec'],
    ],
    resolveOptionalNonNegative: [
      [fn, undefined, 'gamma'],
      [fn, null, 'gamma'],
      [fn, '1', 'gamma'],
      [fn, -1, 'gamma'],
      [fn, Number.NaN, 'gamma'],
      [fn, 0, 'gamma'],
      [fn, 2.5, 'gamma'],
    ],
    resolveRenderFrame: [
      [fn, undefined],
      [fn, null],
      [fn, -1],
      [fn, '5'],
      [fn, Number.POSITIVE_INFINITY],
      [fn, 5],
    ],
  };

  type Answer = { value: unknown } | { error: string; message: string; code: unknown };

  const answer = (validator: unknown, args: unknown[]): Answer => {
    try {
      return { value: (validator as (...a: unknown[]) => unknown)(...args) };
    } catch (error) {
      const e = error as Error & { code?: unknown };
      return { error: e.constructor.name, message: e.message, code: e.code };
    }
  };

  const node = nodeValidation as unknown as Record<string, unknown>;
  // Loaded through a computed path so this package's typecheck does not pull in the WASM sources.
  const wasmValidationPath = fileURLToPath(
    new URL('../../wasm/src/validation.ts', import.meta.url),
  );
  let wasm: Record<string, unknown> = {};
  let shared: string[] = [];
  beforeAll(async () => {
    wasm = (await import(/* @vite-ignore */ wasmValidationPath)) as Record<string, unknown>;
    shared = Object.keys(node).filter((name) => name in wasm);
  });

  it('drives every validator both packages export', () => {
    expect(typeof wasm.assertSamples).toBe('function');
    const functions = shared.filter((name) => typeof node[name] === 'function');
    const uncovered = functions.filter((name) => !SURFACE_LOCAL.has(name) && !(name in CORPUS));
    expect(uncovered).toEqual([]);
  });

  it('keeps the FFT option resolver byte-identical, since it calls only these validators', () => {
    const source = (pkg: string): string =>
      readFileSync(new URL(`../../${pkg}/src/internal/fft_options.ts`, import.meta.url), 'utf8');
    expect(source('wasm')).toBe(source('node'));
  });

  it('shares the same bounds', () => {
    for (const name of shared.filter((key) => typeof node[key] === 'number')) {
      expect(wasm[name], name).toBe(node[name]);
    }
  });

  it.each(Object.entries(CORPUS))('%s answers the same on both surfaces', (name, cases) => {
    expect(typeof node[name], name).toBe('function');
    expect(typeof wasm[name], name).toBe('function');
    for (const args of cases) {
      expect(answer(wasm[name], args), `${name}(${String(args.slice(1))})`).toEqual(
        answer(node[name], args),
      );
    }
  });
});
