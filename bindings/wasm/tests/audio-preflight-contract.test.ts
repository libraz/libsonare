/**
 * Every audio-taking one-shot refuses its audio input the same way: empty or
 * non-finite samples and an out-of-range or non-integer sample rate are a
 * `RangeError`, a wrong-typed `samples` or `sampleRate` is a `TypeError`, and no
 * raw `SonareError` from the core is the first refusal. The suite is closed over
 * the package's exports, so a new audio one-shot without the preflight fails here.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import * as sonare from '../src/index';

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
  mono('roomMorph', { targetRt60: 0.4 }),
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
  ...['splitNote', 'mergeNotes'].map(
    (name): Entry => ({
      name,
      noValid: true,
      call: (samples, sampleRate) =>
        fn(name)({
          samples,
          sampleRate,
          f0Hz: f0(),
          frameRate: 86.13,
          notes: [],
          index: 0,
          frame: 10,
          first: 0,
          last: 1,
        }),
    }),
  ),
  // Mastering chain and processors
  ...['masterAudio', 'masterAudioWithProgress'].map((name) =>
    mono(name, { preset: 'pop', onProgress: () => {} }),
  ),
  ...['masterAudioStereo', 'masterAudioStereoWithProgress'].map((name) =>
    stereo(name, { preset: 'pop', onProgress: () => {} }),
  ),
  ...['masteringChain', 'masteringChainWithProgress'].map((name) =>
    mono(name, { onProgress: () => {} }),
  ),
  ...['masteringChainStereo', 'masteringChainStereoWithProgress'].map((name) =>
    stereo(name, { onProgress: () => {} }),
  ),
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
    'meteringVectorscopeDecimated',
    'meteringPhaseScope',
    'meteringPhaseScopeDecimated',
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
    'init',
    'isInitialized',
    'version',
    'capabilities',
    'capabilityCatalog',
    'abiVersion',
    'engineAbiVersion',
    'engineCapabilities',
    'projectAbiVersion',
    'voiceChangerAbiVersion',
    'vocalEditAvailable',
    'vocalEditApiVersion',
    'hasFfmpegSupport',
    'isSonareError',
    'streamAnalyzerConfigDefaults',
  ],
  // Analysis-to-annotation conversions over a chord or a key mode.
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
    'decomposeWithInit',
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
  // Container decoding, storage and host-device bindings.
  ...[
    'decodeChannels',
    'decodeChannelsWithBrowserFallback',
    'attachOpfsClipStream',
    'createOpfsClipPageProvider',
    'createOpfsClipPageWorker',
    'importOpfsClip',
    'writeOpfsClip',
    'bindMicrophoneInput',
    'bindWebMidi',
    'isWebMidiAvailable',
  ],
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
  beforeAll(async () => {
    await sonare.init();
  });

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
  beforeAll(async () => {
    await sonare.init();
  });

  const x = tone();

  it('detectBpm names samples and sampleRate', () => {
    expect(() => sonare.detectBpm(5 as never, SR)).toThrow(
      new TypeError('detectBpm: samples must be a Float32Array'),
    );
    expect(() => sonare.detectBpm(x, '44100' as never)).toThrow(
      new TypeError('detectBpm: sampleRate must be a number'),
    );
  });

  it('a string the native layer reads is refused before embind sees it', () => {
    expect(() => sonare.Mixer.fromSceneJson(null as never, SR, 512)).toThrow(TypeError);
    expect(() => sonare.masteringPresetParams(5 as never)).toThrow(
      new TypeError('masteringPresetParams: preset must be a string'),
    );
    expect(() => sonare.masterAudio({ samples: x, sampleRate: SR, preset: 5 as never })).toThrow(
      new TypeError('masterAudio: preset must be a string'),
    );
    expect(() =>
      sonare.voiceChange({ samples: x, sampleRate: SR, formantMode: 5 as never }),
    ).toThrow(new TypeError('voiceChange: formantMode must be a string'));
  });

  it('suggestMixScene refuses an empty or missing track id as a TypeError', () => {
    for (const id of ['', undefined]) {
      expect(() =>
        sonare.suggestMixScene({ tracks: [{ id: id as never, left: x }], sampleRate: SR }),
      ).toThrow(TypeError);
    }
  });
});
