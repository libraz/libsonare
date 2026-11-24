import { describe, expect, expectTypeOf, it } from 'vitest';
import type {
  Matrix2D,
  MelSpectrogramResult,
  NoteSegment,
  StreamBarChord,
  StreamChordChange,
  StreamConfig,
  StreamFrames,
  StreamFramesI16,
  StreamFramesU8,
  StreamPatternScore,
  StreamProgressiveEstimate,
  StreamQuantizeConfig,
  StreamStats,
  WindowType,
} from '../../dist/index.js';
import * as sonare from '../../dist/index.js';
import { unknownKeyText } from '../_helpers.js';

const sampleRate = 22050;
// Any exported function by name, for tables that sweep many entry points.
const api = sonare as unknown as Record<string, (...args: unknown[]) => unknown>;

/** A short two-partial tone, about 0.25 s: enough for every framed transform. */
function tinySignal(seconds = 0.25, hz = 440): Float32Array {
  const out = new Float32Array(Math.round(sampleRate * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] =
      0.4 * Math.sin((2 * Math.PI * hz * i) / sampleRate) +
      0.2 * Math.sin((2 * Math.PI * hz * 2 * i) / sampleRate);
  }
  return out;
}

function errorOf(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function expectThrows(fn: () => unknown, cls: ErrorConstructor): void {
  expect(errorOf(fn)).toBeInstanceOf(cls);
}

function isMatrix2D(value: unknown): value is Matrix2D {
  const m = value as Matrix2D;
  return (
    typeof value === 'object' &&
    value !== null &&
    m.data instanceof Float32Array &&
    Number.isInteger(m.rows) &&
    Number.isInteger(m.cols) &&
    m.rows * m.cols === m.data.length
  );
}

const samples = tinySignal();
const small = { nFft: 512, hopLength: 256 };

/** Every function taking samples and a sample rate, with the options its tiny call needs. */
const SAMPLE_FUNCTIONS: ReadonlyArray<[string, Record<string, unknown>]> = [
  ['stft', small],
  ['melSpectrogram', { ...small, nMels: 16 }],
  ['mfcc', { ...small, nMels: 16, nMfcc: 8 }],
  ['reassignedSpectrogram', small],
  ['spectralCentroid', small],
  ['spectralContrast', { ...small, nBands: 4 }],
  ['polyFeatures', small],
  ['zeroCrossings', {}],
  ['spectralBandwidth', small],
  ['spectralRolloff', small],
  ['spectralFlatness', small],
  ['spectralFlux', small],
  ['zeroCrossingRate', { frameLength: 512, hopLength: 256 }],
  ['rmsEnergy', { frameLength: 512, hopLength: 256 }],
  ['chroma', small],
  ['chromaCens', { hopLength: 256 }],
  ['chromaCqt', { hopLength: 256 }],
  ['bassChroma', { hopLength: 256 }],
  ['cqt', { hopLength: 256, nBins: 24 }],
  ['pseudoCqt', { hopLength: 256, nBins: 24 }],
  ['hybridCqt', { hopLength: 256, nBins: 24 }],
  ['vqt', { hopLength: 256, nBins: 24 }],
  ['estimateTuning', small],
  ['piptrack', small],
  ['pitchYin', { frameLength: 1024, hopLength: 256 }],
  ['pitchPyin', { frameLength: 1024, hopLength: 256 }],
  ['onsetStrength', small],
  ['onsetStrengthMulti', small],
  ['lufs', {}],
  ['momentaryLufs', {}],
  ['shortTermLufs', {}],
  ['trimSilence', { frameLength: 512, hopLength: 256 }],
  ['splitSilence', { frameLength: 512, hopLength: 256 }],
  ['frameSignal', { frameLength: 256, hopLength: 128 }],
  ['preemphasis', {}],
  ['deemphasis', {}],
];

const RATE_INDEPENDENT = new Set([
  'preemphasis',
  'deemphasis',
  'trimSilence',
  'splitSilence',
  'frameSignal',
  'zeroCrossings',
]);

describe('sample-taking functions', () => {
  it.each(SAMPLE_FUNCTIONS)('%s requires sampleRate in both forms', (name, options) => {
    const fn = api[name] as (...args: unknown[]) => unknown;
    expect(typeof fn).toBe('function');
    expectThrows(() => fn(samples), TypeError);
    expectThrows(() => fn(samples, undefined, options), TypeError);
    expectThrows(() => fn({ samples, ...options }), TypeError);
    expectThrows(() => fn({ samples, sampleRate: '22050', ...options }), TypeError);
  });

  // The rate is held to the audio domain on every one, including those whose result does not read it.
  it.each(SAMPLE_FUNCTIONS)(
    '%s holds sampleRate to the audio domain in both forms',
    (name, options) => {
      const fn = api[name] as (...args: unknown[]) => unknown;
      const refusal = new RangeError(`${name}: sampleRate must be an integer in [8000, 384000]`);
      for (const rate of [7999, 384001, 0, 22050.5]) {
        expect(() => fn(samples, rate, options)).toThrow(refusal);
        expect(() => fn({ samples, sampleRate: rate, ...options })).toThrow(refusal);
      }
      // Only a computation that never reads the rate is certain to run at both ends.
      if (!RATE_INDEPENDENT.has(name)) {
        return;
      }
      for (const rate of [8000, 384000]) {
        expect(() => fn(samples, rate, options)).not.toThrow();
        expect(() => fn({ samples, sampleRate: rate, ...options })).not.toThrow();
      }
    },
  );

  it.each(SAMPLE_FUNCTIONS)('%s takes samples, sampleRate and an options bag', (name, options) => {
    const fn = api[name] as (...args: unknown[]) => unknown;
    const positional = fn(samples, sampleRate, options);
    const request = fn({ samples, sampleRate, ...options });
    expect(positional).toEqual(request);
  });

  it.each(SAMPLE_FUNCTIONS)(
    '%s refuses the removed positional overloads and extra arguments',
    (name, options) => {
      const fn = api[name] as (...args: unknown[]) => unknown;
      // The former (samples, sampleRate, nFft, hopLength, ...) tail is not an options bag.
      expectThrows(() => fn(samples, sampleRate, 1024, 256), TypeError);
      expectThrows(() => fn(samples, sampleRate, 1024), TypeError);
      // A request plus a positional argument is refused.
      expectThrows(() => fn({ samples, sampleRate, ...options }, sampleRate), TypeError);
      // `samples` / `sampleRate` inside the bag duplicate the positional ones.
      expectThrows(() => fn(samples, sampleRate, { sampleRate }), TypeError);
    },
  );

  it.each(SAMPLE_FUNCTIONS)('%s refuses an unknown request key by name', (name, options) => {
    const fn = api[name] as (...args: unknown[]) => unknown;
    expectThrows(() => fn({ samples, sampleRate, ...options, notAnOption: 1 }), TypeError);
    expectThrows(() => fn(samples, sampleRate, { notAnOption: 1 }), TypeError);
  });

  it.each([
    ['stft', { samples: new Float32Array(0), sampleRate }, RangeError],
    ['stft', { samples, sampleRate: 22050.5 }, RangeError],
    ['stft', { samples, sampleRate: 100 }, RangeError],
    ['stft', { samples: [1, 2, 3], sampleRate }, TypeError],
  ] as const)('%s refuses a bad audio input by class', (name, request, cls) => {
    expectThrows(() => api[name](request), cls);
  });

  it('stft without db returns no db plane and the same magnitude as with db', () => {
    const plain = sonare.stft(samples, sampleRate, small);
    const withDb = sonare.stft(samples, sampleRate, { ...small, db: true });
    expect('db' in plain).toBe(false);
    expect(Array.from(plain.magnitude.data)).toEqual(Array.from(withDb.magnitude.data));
    expect(Array.from(plain.power.data)).toEqual(Array.from(withDb.power.data));
    const db = withDb.db;
    expect(isMatrix2D(db)).toBe(true);
    expect(db?.rows).toBe(plain.magnitude.rows);
    expect(db?.rows).toBe(small.nFft / 2 + 1);
    expect(db?.cols).toBe(plain.magnitude.cols);
  });

  it('refuses a non-finite sample by class', () => {
    const bad = new Float32Array(samples);
    bad[10] = Number.NaN;
    expectThrows(() => sonare.stft({ samples: bad, sampleRate, ...small }), RangeError);
    expectThrows(() => sonare.zeroCrossingRate(bad, sampleRate), RangeError);
  });

  it.each(SAMPLE_FUNCTIONS)('%s refuses validate as an unknown request key', (name, options) => {
    const fn = api[name] as (...args: unknown[]) => unknown;
    const message = unknownKeyText(`${name}: unknown key validate`);
    expect(() => fn({ samples, sampleRate, ...options, validate: false })).toThrow(message);
    expect(() => fn(samples, sampleRate, { ...options, validate: true })).toThrow(
      unknownKeyText(`${name}: unknown key validate`),
    );
  });

  it.each(SAMPLE_FUNCTIONS)('%s refuses a non-finite sample', (name, options) => {
    const fn = api[name] as (...args: unknown[]) => unknown;
    const bad = new Float32Array(samples);
    bad[10] = Number.NaN;
    expectThrows(() => fn({ samples: bad, sampleRate, ...options }), RangeError);
  });

  it('splitSilenceCommon and its report variant take a request only', () => {
    const signals = [samples, samples];
    const request = { signals, topDb: 40, frameLength: 512, hopLength: 256 };
    expect(sonare.splitSilenceCommon(request)).toBeInstanceOf(Int32Array);
    const withReport = sonare.splitSilenceCommonWithReport(request);
    expect(withReport.intervals).toBeInstanceOf(Int32Array);
    expect(typeof withReport.report.silenceCeilingDb).toBe('number');
    for (const name of ['splitSilenceCommon', 'splitSilenceCommonWithReport']) {
      expectThrows(() => api[name](signals), TypeError);
      expectThrows(() => api[name](request, 1), TypeError);
      expectThrows(() => api[name]({ ...request, notAnOption: 1 }), TypeError);
    }
  });

  it('uses fminHz / fmaxHz / f0MinHz / f0MaxHz and refuses the old spellings', () => {
    const base = { samples, sampleRate };
    expect(() => sonare.cqt({ ...base, hopLength: 256, nBins: 24, fminHz: 65 })).not.toThrow();
    expect(() => sonare.vqt({ ...base, hopLength: 256, nBins: 24, fminHz: 65 })).not.toThrow();
    expect(() =>
      sonare.pseudoCqt({ ...base, hopLength: 256, nBins: 24, fminHz: 65 }),
    ).not.toThrow();
    expect(() =>
      sonare.hybridCqt({ ...base, hopLength: 256, nBins: 24, fminHz: 65 }),
    ).not.toThrow();
    expect(() => sonare.piptrack({ ...base, ...small, fminHz: 100, fmaxHz: 2000 })).not.toThrow();
    expect(() =>
      sonare.melSpectrogram({ ...base, ...small, nMels: 16, fminHz: 50, fmaxHz: 8000 }),
    ).not.toThrow();
    expect(() =>
      sonare.mfcc({ ...base, ...small, nMels: 16, nMfcc: 8, fminHz: 50, fmaxHz: 8000 }),
    ).not.toThrow();
    expect(() =>
      sonare.spectralContrast({ ...base, ...small, nBands: 4, fminHz: 200 }),
    ).not.toThrow();
    expect(() =>
      sonare.pitchYin({ ...base, frameLength: 1024, hopLength: 256, f0MinHz: 80, f0MaxHz: 1000 }),
    ).not.toThrow();
    expect(() =>
      sonare.pitchPyin({ ...base, frameLength: 1024, hopLength: 256, f0MinHz: 80, f0MaxHz: 1000 }),
    ).not.toThrow();
    for (const [name, key] of [
      ['cqt', 'fmin'],
      ['vqt', 'fmin'],
      ['pseudoCqt', 'fmin'],
      ['hybridCqt', 'fmin'],
      ['piptrack', 'fmin'],
      ['piptrack', 'fmax'],
      ['melSpectrogram', 'fmin'],
      ['melSpectrogram', 'fmax'],
      ['mfcc', 'fmin'],
      ['mfcc', 'fmax'],
      ['spectralContrast', 'fmin'],
      ['pitchYin', 'fmin'],
      ['pitchYin', 'fmax'],
      ['pitchPyin', 'fmin'],
      ['pitchPyin', 'fmax'],
    ] as const) {
      expectThrows(() => api[name]({ ...base, [key]: 100 }), TypeError);
    }
  });

  it('renames the generator fields to carry their unit', () => {
    expect(sonare.tone({ frequencyHz: 440, durationSec: 0.1, sampleRate }).length).toBe(2205);
    expect(sonare.chirp({ fminHz: 440, fmaxHz: 880, durationSec: 0.1, sampleRate }).length).toBe(
      2205,
    );
    expect(
      sonare.clicks({
        times: new Float32Array([0.01]),
        frequencyHz: 1000,
        clickDurationSec: 0.02,
        sampleRate,
      }).length,
    ).toBeGreaterThan(0);
    for (const [name, request] of [
      ['tone', { frequency: 440 }],
      ['tone', { duration: 0.1 }],
      ['chirp', { fmin: 440 }],
      ['chirp', { fmax: 880 }],
      ['chirp', { duration: 0.1 }],
      ['clicks', { times: new Float32Array([0.01]), frequency: 1000 }],
      ['clicks', { times: new Float32Array([0.01]), clickDuration: 0.02 }],
    ] as const) {
      expectThrows(() => api[name](request), TypeError);
    }
  });

  it('defaults chirp to an exponential sweep', () => {
    const request = { fminHz: 200, fmaxHz: 800, durationSec: 0.1, sampleRate };
    const exponential = sonare.chirp(request);
    expect(sonare.chirp({ ...request, linear: false })).toEqual(exponential);
    expect(sonare.chirp({ ...request, linear: true })).not.toEqual(exponential);
  });

  it('spells the absent click length as absence and refuses an explicit 0', () => {
    const times = new Float32Array([0.01, 0.05]);
    expect(sonare.clicks({ times, sampleRate }).length).toBeGreaterThan(0);
    expect(sonare.clicks({ times, sampleRate, length: null } as never).length).toBeGreaterThan(0);
    expect(sonare.clicks({ times, sampleRate, length: 4000 }).length).toBe(4000);
    expectThrows(() => sonare.clicks({ times, sampleRate, length: 0 }), RangeError);
    expectThrows(() => sonare.clicks({ times, sampleRate, length: '4000' } as never), TypeError);
  });

  it('refuses an explicit 0 centring window and reads absence as none', () => {
    expect(sonare.framesToSamples(10, 512)).toBe(5120);
    expect(sonare.samplesToFrames(5120, 512)).toBe(10);
    expect(sonare.framesToSamples(10, 512, 2048)).not.toBe(5120);
    expectThrows(() => sonare.framesToSamples(10, 512, 0), RangeError);
    expectThrows(() => sonare.samplesToFrames(5120, 512, 0), RangeError);
    expect(() => sonare.framesToSamples(10, 512, 0)).toThrow(
      'framesToSamples: nFft must not be 0; omit it for no centring',
    );
    expectThrows(() => sonare.framesToSamples(10, 512, -5), RangeError);
  });

  it('refuses a hop that is not a positive integer on every frame conversion', () => {
    const calls: Array<[string, (hop: number) => number]> = [
      ['framesToTime', (hop) => sonare.framesToTime(4, sampleRate, hop)],
      ['timeToFrames', (hop) => sonare.timeToFrames(1, sampleRate, hop)],
      ['framesToSamples', (hop) => sonare.framesToSamples(4, hop)],
      ['samplesToFrames', (hop) => sonare.samplesToFrames(2048, hop)],
    ];
    for (const [name, call] of calls) {
      for (const hop of [0, -5]) {
        expect(() => call(hop)).toThrow(`${name}: hopLength must be an integer in [1, 2147483647]`);
      }
      expectThrows(() => call(1.5), RangeError);
    }
  });

  it('takes the scalar unit conversions positionally only', () => {
    expect(sonare.framesToTime(43, sampleRate, 512)).toBeCloseTo((43 * 512) / sampleRate, 6);
    expect(sonare.timeToFrames(1, sampleRate, 512)).toBe(Math.floor(sampleRate / 512));
    for (const name of ['framesToTime', 'timeToFrames', 'framesToSamples', 'samplesToFrames']) {
      expectThrows(() => api[name]({ frames: 1, samples: 1, time: 1, timeSec: 1 }), TypeError);
    }
  });

  it('reports the tuning offset in cents and takes cents for the A4 converters', () => {
    const detuned = 440 * 2 ** (20 / 1200);
    const frequencies = new Float32Array(8).fill(detuned);
    expect(Math.abs(sonare.pitchTuning({ frequencies }) - 20)).toBeLessThan(3);
    const tuned = sonare.estimateTuning(tinySignal(0.5, detuned), sampleRate, {
      nFft: 2048,
      hopLength: 512,
    });
    expect(Math.abs(tuned - 20)).toBeLessThan(8);
    expect(sonare.tuningToReferenceHz(100)).toBeCloseTo(440 * 2 ** (1 / 12), 3);
    expect(sonare.tuningToReferenceHz(100, 432)).toBeCloseTo(432 * 2 ** (1 / 12), 3);
    expect(sonare.referenceHzToTuning(440 * 2 ** (1 / 12))).toBeCloseTo(100, 3);
    expect(sonare.referenceHzToTuning(440, 440)).toBeCloseTo(0, 6);
  });

  it('renames the note segment times and the loudness range', () => {
    const f0Hz = new Float32Array(60).fill(440);
    const voicedProb = new Float32Array(60).fill(1);
    const segments = sonare.noteSegments({ f0Hz, voicedProb, frameRate: 100, minNoteMs: 100 });
    expect(segments.length).toBeGreaterThan(0);
    const first = segments[0] as unknown as Record<string, unknown>;
    expect(typeof first.startSec).toBe('number');
    expect(typeof first.endSec).toBe('number');
    expect('startSeconds' in first).toBe(false);
    expect('endSeconds' in first).toBe(false);
    const loud = sonare.lufs(tinySignal(6), sampleRate) as unknown as Record<string, unknown>;
    expect(typeof loud.loudnessRangeLu).toBe('number');
    expect('loudnessRange' in loud).toBe(false);
  });

  it('keeps the note segment tuning fields flat and refuses the nested config', () => {
    const f0Hz = new Float32Array(60).fill(440);
    const voicedProb = new Float32Array(60).fill(1);
    const flat = sonare.noteSegments({
      f0Hz,
      voicedProb,
      frameRate: 100,
      segmentationThresholdCents: 80,
      minNoteMs: 50,
      referenceHz: 432,
      voicedThreshold: 0.4,
    });
    expect(flat.length).toBeGreaterThan(0);
    expectThrows(
      () => api.noteSegments({ f0Hz, voicedProb, frameRate: 100, config: { minNoteMs: 50 } }),
      TypeError,
    );
  });

  it('names the onset envelope function onsetStrength', () => {
    expect(sonare.onsetStrength(samples, sampleRate, small)).toBeInstanceOf(Float32Array);
    expect(api.onsetEnvelope).toBeUndefined();
  });

  it('searches 60-200 BPM in plp through bpmMin / bpmMax', () => {
    const onsetEnvelope = sonare.onsetStrength(tinySignal(2), sampleRate, small);
    const wide = sonare.plp({ onsetEnvelope, sampleRate, hopLength: 256, bpmMin: 30, bpmMax: 300 });
    const dflt = sonare.plp({ onsetEnvelope, sampleRate, hopLength: 256 });
    const explicit = sonare.plp({
      onsetEnvelope,
      sampleRate,
      hopLength: 256,
      bpmMin: 60,
      bpmMax: 200,
    });
    expect(dflt).toEqual(explicit);
    expect(dflt.length).toBe(wide.length);
    expectThrows(() => api.plp({ onsetEnvelope, sampleRate, tempoMin: 60 }), TypeError);
    expectThrows(() => api.plp({ onsetEnvelope, sampleRate, tempoMax: 200 }), TypeError);
  });

  it('drops the fourierTempogram norm field and the tempogram ratio window', () => {
    const onsetEnvelope = sonare.onsetStrength(tinySignal(2), sampleRate, small);
    expect(
      isMatrix2D(
        sonare.fourierTempogram({ onsetEnvelope, sampleRate, hopLength: 256, winLength: 64 }),
      ),
    ).toBe(true);
    expectThrows(
      () =>
        api.fourierTempogram({
          onsetEnvelope,
          sampleRate,
          hopLength: 256,
          winLength: 64,
          norm: true,
        }),
      TypeError,
    );
    const tempogram = sonare.tempogram({
      onsetEnvelope,
      sampleRate,
      hopLength: 256,
      winLength: 64,
    });
    expect(() => sonare.tempogramRatio({ tempogram, sampleRate, hopLength: 256 })).not.toThrow();
    expectThrows(() => api.tempogramRatio({ tempogram, sampleRate, winLength: 64 }), TypeError);
    expectThrows(
      () => api.tempogramRatio({ tempogramData: tempogram.data, sampleRate }),
      TypeError,
    );
  });
});

describe('Matrix2D results and inputs', () => {
  const stftResult = sonare.stft(samples, sampleRate, small);
  const mel = sonare.melSpectrogram(samples, sampleRate, { ...small, nMels: 16 });
  const mfccResult = sonare.mfcc(samples, sampleRate, { ...small, nMels: 16, nMfcc: 8 });
  const chromaResult = sonare.chroma(samples, sampleRate, small);
  const onset = sonare.onsetStrength(tinySignal(2), sampleRate, small);
  const features: Matrix2D = {
    data: new Float32Array(2 * 8).map((_, i) => (i * 7) % 5),
    rows: 2,
    cols: 8,
  };

  it.each([
    ['stft db', () => sonare.stft(samples, sampleRate, { ...small, db: true }).db as Matrix2D, 257],
    [
      'spectralContrast',
      () => sonare.spectralContrast(samples, sampleRate, { ...small, nBands: 4 }),
      5,
    ],
    ['polyFeatures', () => sonare.polyFeatures(samples, sampleRate, small), 2],
    [
      'nnlsChroma',
      () => sonare.nnlsChroma(samples, sampleRate, { hopLength: 256, stftBlendNFft: 1024 }),
      12,
    ],
    ['onsetStrengthMulti', () => sonare.onsetStrengthMulti(samples, sampleRate, small), 3],
    [
      'frameSignal',
      () => sonare.frameSignal(samples, sampleRate, { frameLength: 256, hopLength: 128 }),
      256,
    ],
    [
      'tempogram',
      () => sonare.tempogram({ onsetEnvelope: onset, sampleRate, hopLength: 256, winLength: 64 }),
      64,
    ],
    [
      'cyclicTempogram',
      () =>
        sonare.cyclicTempogram({ onsetEnvelope: onset, sampleRate, hopLength: 256, winLength: 64 }),
      60,
    ],
    ['melDelta', () => sonare.melDelta({ features, width: 3 }), 2],
    ['pcen', () => sonare.pcen({ values: stftResult.magnitude, sampleRate, hopLength: 256 }), 257],
    ['melToStft', () => sonare.melToStft({ power: mel.power, sampleRate, nFft: 512 }), 257],
    ['mfccToMel', () => sonare.mfccToMel({ coefficients: mfccResult.coefficients, nMels: 16 }), 16],
    ['tonnetz', () => sonare.tonnetz({ chromagram: chromaResult.features }), 6],
  ] as const)('returns Matrix2D from %s', (_name, call, rows) => {
    const result = call();
    expect(isMatrix2D(result)).toBe(true);
    expect((result as Matrix2D).rows).toBe(rows);
    expect(Object.keys(result as object).sort()).toEqual(['cols', 'data', 'rows']);
  });

  it('returns Matrix2D from fourierTempogram and the segment functions', () => {
    expect(
      isMatrix2D(
        sonare.fourierTempogram({
          onsetEnvelope: onset,
          sampleRate,
          hopLength: 256,
          winLength: 64,
        }),
      ),
    ).toBe(true);
    const data: Matrix2D = chromaResult.features;
    const recurrence = sonare.segmentRecurrenceMatrix({ data });
    expect(isMatrix2D(recurrence)).toBe(true);
    expect(recurrence.rows).toBe(recurrence.cols);
    expect(recurrence.rows).toBe(data.cols);
    const lag = sonare.segmentRecurrenceToLag({ recurrence });
    expect(isMatrix2D(lag)).toBe(true);
    expect(lag.cols).toBe(data.cols);
    expect(sonare.segmentLagToRecurrence({ lag }).rows).toBe(data.cols);
    expect(isMatrix2D(sonare.segmentPathEnhance({ recurrence, win: 3 }))).toBe(true);
    const cross = sonare.segmentCrossSimilarity({ x: data, y: data });
    expect(isMatrix2D(cross)).toBe(true);
    expect([cross.rows, cross.cols]).toEqual([data.cols, data.cols]);
    expect(sonare.segmentAgglomerative({ data, k: 2 }).length).toBe(data.cols);
    expect(
      sonare.segmentSubsegment({ data, boundaries: [0, data.cols], nSegments: 2 }),
    ).toBeInstanceOf(Int32Array);
  });

  it('segment similarity refuses a zero or negative k and takes the librosa default when it is omitted', () => {
    const data: Matrix2D = {
      data: Float32Array.from({ length: 32 }, (_, i) => Math.sin(i)),
      rows: 4,
      cols: 8,
    };
    expect(sonare.segmentRecurrenceMatrix({ data }).rows).toBe(8);
    expect(sonare.segmentRecurrenceMatrix({ data, k: 2 }).rows).toBe(8);
    for (const k of [0, -1]) {
      expect(() => sonare.segmentRecurrenceMatrix({ data, k })).toThrow(RangeError);
      expect(() => sonare.segmentCrossSimilarity({ x: data, y: data, k })).toThrow(RangeError);
    }
  });

  it('carries every 2-D result field as a Matrix2D and no loose dimensions', () => {
    const loose: ReadonlyArray<[unknown, string[]]> = [
      [stftResult, ['nBins', 'nFrames']],
      [mel, ['nMels', 'nFrames', 'isDb']],
      [mfccResult, ['nMfcc', 'nFrames', 'isDb']],
      [chromaResult, ['nChroma', 'nFrames']],
    ];
    for (const [result, keys] of loose) {
      for (const key of keys) {
        expect(key in (result as object)).toBe(false);
      }
    }
    expect(isMatrix2D(stftResult.magnitude)).toBe(true);
    expect(isMatrix2D(stftResult.power)).toBe(true);
    expect(stftResult.magnitude.rows).toBe(257);
    expect(isMatrix2D(mel.power) && isMatrix2D(mel.db)).toBe(true);
    expect(mel.power.rows).toBe(16);
    expect(isMatrix2D(mfccResult.coefficients)).toBe(true);
    expect(mfccResult.coefficients.rows).toBe(8);
    expect(isMatrix2D(chromaResult.features)).toBe(true);
    expect(chromaResult.features.rows).toBe(12);
    const cqt = sonare.cqt(samples, sampleRate, { hopLength: 256, nBins: 24 });
    expect(isMatrix2D(cqt.magnitude)).toBe(true);
    expect(cqt.magnitude.rows).toBe(24);
    expect(cqt.frequencies.length).toBe(24);
    const pip = sonare.piptrack(samples, sampleRate, small);
    expect(isMatrix2D(pip.pitches) && isMatrix2D(pip.magnitudes)).toBe(true);
    expect(pip.pitches.rows).toBe(257);
    const ra = sonare.reassignedSpectrogram(samples, sampleRate, small);
    expect(isMatrix2D(ra.magnitude) && isMatrix2D(ra.times) && isMatrix2D(ra.frequencies)).toBe(
      true,
    );
  });

  it('renames the mel and MFCC result band edges', () => {
    const result = mel as unknown as Record<string, unknown>;
    expect(typeof result.fminHz).toBe('number');
    expect(typeof result.fmaxHz).toBe('number');
    expect('fmin' in result).toBe(false);
    expect('fmax' in result).toBe(false);
    const m = mfccResult as unknown as Record<string, unknown>;
    expect(typeof m.fminHz).toBe('number');
    expect(typeof m.fmaxHz).toBe('number');
    expect('fmin' in m).toBe(false);
  });

  it('inverts a forward result through its Matrix2D fields', () => {
    const power = sonare.melToStft({ power: mel.power, sampleRate, nFft: 512 });
    expect(power.cols).toBe(mel.power.cols);
    expect(
      sonare.melToAudio({ power: mel.power, sampleRate, nFft: 512, hopLength: 256, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
    expect(
      sonare.griffinLim({
        magnitude: stftResult.magnitude,
        sampleRate,
        nFft: 512,
        hopLength: 256,
        nIter: 2,
      }),
    ).toBeInstanceOf(Float32Array);
    expect(() =>
      sonare.griffinLim({
        magnitude: stftResult.magnitude,
        sampleRate,
        nFft: 1024,
        hopLength: 256,
        nIter: 2,
      }),
    ).toThrow(
      new RangeError(
        `griffinLim: nFft must equal (magnitude.rows - 1) * 2, which is ${(stftResult.magnitude.rows - 1) * 2}`,
      ),
    );
    expect(
      sonare.mfccToAudio({
        coefficients: mfccResult.coefficients,
        nMels: 16,
        sampleRate,
        nFft: 512,
        hopLength: 256,
        nIter: 2,
      }),
    ).toBeInstanceOf(Float32Array);
    const cqt = sonare.cqt(samples, sampleRate, { hopLength: 256, nBins: 24 });
    expect(
      sonare.cqtToAudio({ magnitude: cqt.magnitude, sampleRate, hopLength: 256, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
    expect(
      sonare.vqtToAudio({ magnitude: cqt.magnitude, sampleRate, hopLength: 256, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
  });

  it('derives an omitted inverse hop as a quarter of nFft', () => {
    const common = { sampleRate, nFft: 512, nIter: 2 };
    const pairs: Array<[string, (hop?: number) => Float32Array]> = [
      ['melToAudio', (hop) => sonare.melToAudio({ power: mel.power, ...common, hopLength: hop })],
      [
        'mfccToAudio',
        (hop) =>
          sonare.mfccToAudio({
            coefficients: mfccResult.coefficients,
            nMels: 16,
            ...common,
            hopLength: hop,
          }),
      ],
      [
        'griffinLim',
        (hop) => sonare.griffinLim({ magnitude: stftResult.magnitude, ...common, hopLength: hop }),
      ],
    ];
    for (const [name, call] of pairs) {
      expect(Array.from(call()), name).toEqual(Array.from(call(128)));
      expect(Array.from(call()), name).not.toEqual(Array.from(call(256)));
    }
  });

  it.each([
    ['melDelta', 'features', { width: 3 }],
    ['pcen', 'values', {}],
    ['melToStft', 'power', { nFft: 512 }],
    ['melToAudio', 'power', { nFft: 512 }],
    ['griffinLim', 'magnitude', { sampleRate, nFft: 512 }],
    ['mfccToMel', 'coefficients', {}],
    ['mfccToAudio', 'coefficients', {}],
    ['cqtToAudio', 'magnitude', { sampleRate }],
    ['vqtToAudio', 'magnitude', { sampleRate }],
    ['tonnetz', 'chromagram', {}],
    ['segmentRecurrenceMatrix', 'data', {}],
    ['segmentAgglomerative', 'data', { k: 2 }],
    ['segmentSubsegment', 'data', { boundaries: [0, 8] }],
    ['segmentRecurrenceToLag', 'recurrence', {}],
    ['segmentPathEnhance', 'recurrence', { win: 3 }],
    ['segmentLagToRecurrence', 'lag', {}],
  ] as const)('%s takes its matrix as a Matrix2D', (name, field, rest) => {
    const raw = new Float32Array(16);
    // A bare buffer is the wrong type, a mismatched shape is out of domain.
    expectThrows(() => api[name]({ [field]: raw, ...rest }), TypeError);
    expectThrows(
      () => api[name]({ [field]: { data: raw, rows: 3, cols: 3 }, ...rest }),
      RangeError,
    );
    expectThrows(
      () => api[name]({ [field]: { data: raw, rows: 0, cols: 0 }, ...rest }),
      RangeError,
    );
    expectThrows(
      () => api[name]({ [field]: { data: [1, 2], rows: 1, cols: 2 }, ...rest }),
      TypeError,
    );
  });

  it('refuses the old dimension fields beside a matrix', () => {
    expectThrows(() => api.melDelta({ features, nFeatures: 2, nFrames: 8 }), TypeError);
    expectThrows(
      () => api.cqtToAudio({ magnitude: features, sampleRate, nBins: 2, nFrames: 8 }),
      TypeError,
    );
    expectThrows(() => api.mfccToMel({ coefficients: features, nMfcc: 2, nFrames: 8 }), TypeError);
    expectThrows(() => api.tonnetz({ chromagram: features, nChroma: 2, nFrames: 8 }), TypeError);
    expectThrows(
      () => api.segmentRecurrenceMatrix({ data: features, rows: 2, cols: 8 }),
      TypeError,
    );
  });

  it('requires a square matrix where the recurrence is square', () => {
    expectThrows(() => sonare.segmentRecurrenceToLag({ recurrence: features }), RangeError);
    expectThrows(() => sonare.segmentPathEnhance({ recurrence: features, win: 3 }), RangeError);
  });

  it('refuses the removed result-object request forms of the inverse transforms', () => {
    expectThrows(() => api.melToStft({ result: mel }), TypeError);
    expectThrows(() => api.melToAudio({ result: mel }), TypeError);
    expectThrows(() => api.mfccToMel({ result: mfccResult }), TypeError);
    expectThrows(() => api.mfccToAudio({ result: mfccResult }), TypeError);
  });

  it('takes boundaries and events as any array-like of numbers', () => {
    const data: Matrix2D = chromaResult.features;
    const asArray = sonare.segmentSubsegment({ data, boundaries: [0, data.cols], nSegments: 2 });
    const asTyped = sonare.segmentSubsegment({
      data,
      boundaries: new Int32Array([0, data.cols]),
      nSegments: 2,
    });
    expect(asArray).toEqual(asTyped);
    expect(() =>
      sonare.onsetBacktrack({ events: new Int32Array([3]), energy: new Float32Array(10).fill(1) }),
    ).not.toThrow();
    expectThrows(() => api.onsetBacktrack({ events: 3, energy: new Float32Array(10) }), TypeError);
  });
});

describe('request-only functions', () => {
  const values = new Float32Array([1, 0.5, 0.25, 0.1]);
  it.each([
    ['powerToDb', [values], { values }],
    ['amplitudeToDb', [values], { values }],
    ['dbToPower', [values], { values }],
    ['dbToAmplitude', [values], { values }],
    ['padCenter', [values, 8], { values, targetSize: 8 }],
    ['fixLength', [values, 8], { values, targetSize: 8 }],
    ['vectorNormalize', [values], { values }],
    ['fixFrames', [new Int32Array([1, 2])], { frames: new Int32Array([1, 2]) }],
    [
      'onsetBacktrack',
      [new Int32Array([1]), values],
      { events: new Int32Array([1]), energy: values },
    ],
    [
      'peakPick',
      [values, 1, 1, 1, 1, 0, 1],
      { values, preMax: 1, postMax: 1, preAvg: 1, postAvg: 1, delta: 0, wait: 1 },
    ],
    ['pitchTuning', [values], { frequencies: values }],
    ['tone', [440], { sampleRate }],
    ['chirp', [440, 880], { sampleRate }],
    ['clicks', [new Float32Array([0.1])], { times: new Float32Array([0.1]), sampleRate }],
  ] as const)('%s takes a request and refuses positional values', (name, positional, request) => {
    expect(() => api[name](request)).not.toThrow();
    expectThrows(() => api[name](...(positional as unknown as unknown[])), TypeError);
    expectThrows(() => api[name](request, 1), TypeError);
    expectThrows(() => api[name]({ ...request, notAnOption: 1 }), TypeError);
  });

  it('gives the request-only tempo functions no positional form', () => {
    const onsetEnvelope = new Float32Array(200).fill(0.5);
    for (const name of ['tempogram', 'cyclicTempogram', 'fourierTempogram', 'plp']) {
      expectThrows(() => api[name](onsetEnvelope, sampleRate), TypeError);
    }
  });

  it('keeps dB conversion defaults', () => {
    expect(sonare.powerToDb({ values })[0]).toBeCloseTo(0, 5);
    expect(sonare.dbToPower({ values: new Float32Array([0, 10]) })[1]).toBeCloseTo(10, 4);
    expect(sonare.dbToAmplitude({ values: new Float32Array([0, 20]), ref: 2 })[1]).toBeCloseTo(
      20,
      4,
    );
    expect(sonare.amplitudeToDb({ values, topDb: 80 })[0]).toBeCloseTo(0, 5);
  });
});

describe('closed sets refuse numbers and synonyms', () => {
  const values = new Float32Array([3, 4]);

  it.each([
    ['max', 0.75],
    ['l1', 3 / 7],
    ['l2', 0.6],
  ] as const)('vectorNormalize accepts the %s norm', (norm, first) => {
    const out = sonare.vectorNormalize({ values, norm });
    expect(out[0]).toBeCloseTo(first, 5);
  });

  it('vectorNormalize refuses a number (wrong type) and an unknown token (out of domain)', () => {
    expectThrows(() => api.vectorNormalize({ values, norm: 2 }), TypeError);
    expectThrows(() => api.vectorNormalize({ values, norm: 0 }), TypeError);
    expectThrows(() => api.vectorNormalize({ values, norm: 'euclidean' }), RangeError);
    expectThrows(() => api.vectorNormalize({ values, norm: 'L2' }), RangeError);
    expectThrows(() => api.vectorNormalize({ values, normType: 2 }), TypeError);
  });
});

describe('type shapes', () => {
  it('declares the streaming configuration with required sampleRate and unit-carrying names', () => {
    expectTypeOf<StreamConfig['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<StreamConfig['fminHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamConfig['fmaxHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamConfig['referenceHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamConfig['window']>().toEqualTypeOf<WindowType | undefined>();
    expectTypeOf<WindowType>().toEqualTypeOf<'hann' | 'hamming' | 'blackman' | 'rectangular'>();
    // @ts-expect-error removed field
    type NoTuningRef = StreamConfig['tuningRefHz'];
    // @ts-expect-error removed field
    type NoOutputFormat = StreamConfig['outputFormat'];
    // @ts-expect-error removed field
    type NoComputeMagnitude = StreamConfig['computeMagnitude'];
    // @ts-expect-error renamed
    type NoFmin = StreamConfig['fmin'];
    expectTypeOf<[NoTuningRef, NoOutputFormat, NoComputeMagnitude, NoFmin]>().not.toBeNever();
    expect(true).toBe(true);
  });

  it('declares StreamFrames with optional band-major features and seconds timestamps', () => {
    expectTypeOf<StreamFrames['timestampsSec']>().toEqualTypeOf<Float32Array>();
    expectTypeOf<StreamFrames['mel']>().toEqualTypeOf<Matrix2D | undefined>();
    expectTypeOf<StreamFrames['chroma']>().toEqualTypeOf<Matrix2D | undefined>();
    expectTypeOf<StreamFrames['onsetStrength']>().toEqualTypeOf<Float32Array | undefined>();
    expectTypeOf<StreamFrames['chordQuality']>().toEqualTypeOf<Int32Array | undefined>();
    // @ts-expect-error removed field
    type NoFlags = StreamFrames['featureFlags'];
    // @ts-expect-error removed field
    type NoMels = StreamFrames['nMels'];
    // @ts-expect-error renamed
    type NoTimestamps = StreamFrames['timestamps'];
    expectTypeOf<StreamFramesU8['timestampsSec']>().toEqualTypeOf<Float32Array>();
    expectTypeOf<StreamFramesU8['mel']>().toEqualTypeOf<Uint8Array | undefined>();
    expectTypeOf<StreamFramesI16['mel']>().toEqualTypeOf<Int16Array | undefined>();
    expectTypeOf<StreamFramesI16['onsetStrength']>().toEqualTypeOf<Int16Array | undefined>();
    // @ts-expect-error removed field
    type NoU8Flags = StreamFramesU8['featureFlags'];
    expectTypeOf<StreamQuantizeConfig['melDbMin']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<[NoFlags, NoMels, NoTimestamps, NoU8Flags]>().not.toBeNever();
    expect(true).toBe(true);
  });

  it('declares the streaming estimate with absent-until-estimated fields', () => {
    expectTypeOf<StreamProgressiveEstimate['bpm']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['bpmConfidence']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['key']>().toEqualTypeOf<sonare.Key | undefined>();
    expectTypeOf<StreamProgressiveEstimate['chord']>().toEqualTypeOf<
      StreamChordChange | undefined
    >();
    expectTypeOf<StreamProgressiveEstimate['currentBar']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['barDurationSec']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<StreamProgressiveEstimate['detectedPatternName']>().toEqualTypeOf<
      string | undefined
    >();
    expectTypeOf<StreamProgressiveEstimate['detectedPatternScore']>().toEqualTypeOf<
      number | undefined
    >();
    expectTypeOf<StreamProgressiveEstimate['accumulatedSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamStats['durationSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamChordChange['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamBarChord['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<StreamChordChange['root']>().toEqualTypeOf<sonare.PitchClass>();
    expectTypeOf<StreamChordChange['quality']>().toEqualTypeOf<sonare.ChordQuality>();
    expectTypeOf<StreamBarChord['quality']>().toEqualTypeOf<sonare.ChordQuality>();
    expectTypeOf<StreamPatternScore['score']>().toEqualTypeOf<number>();
    // @ts-expect-error carried by key?
    type NoKeyMinor = StreamProgressiveEstimate['keyMinor'];
    // @ts-expect-error carried by chord?
    type NoChordName = StreamProgressiveEstimate['chordName'];
    // @ts-expect-error carried by chord?.startSec
    type NoChordStart = StreamProgressiveEstimate['chordStartTime'];
    // @ts-expect-error renamed
    type NoAccumulated = StreamProgressiveEstimate['accumulatedSeconds'];
    // @ts-expect-error renamed
    type NoDuration = StreamStats['durationSeconds'];
    expectTypeOf<
      [NoKeyMinor, NoChordName, NoChordStart, NoAccumulated, NoDuration]
    >().not.toBeNever();
    expect(true).toBe(true);
  });

  it('declares result types without loose dimensions and with renamed fields', () => {
    expectTypeOf<NoteSegment['startSec']>().toEqualTypeOf<number>();
    expectTypeOf<NoteSegment['endSec']>().toEqualTypeOf<number>();
    expectTypeOf<MelSpectrogramResult['power']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<MelSpectrogramResult['fminHz']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.StftResult['magnitude']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.MfccResult['coefficients']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.ChromaResult['features']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.CqtResult['magnitude']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.PiptrackResult['pitches']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.ReassignedSpectrogramResult['times']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.LufsResult['loudnessRangeLu']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<
      sonare.SplitSilenceCommonWithReportResult['intervals']
    >().toEqualTypeOf<Int32Array>();
    expectTypeOf<sonare.TrimResult>().toHaveProperty('startSample');
    expectTypeOf<sonare.SpectralEditOptions['window']>().toEqualTypeOf<WindowType | undefined>();
    // @ts-expect-error folded into the Matrix2D
    type NoBins = sonare.StftResult['nBins'];
    // @ts-expect-error always false, removed
    type NoIsDb = MelSpectrogramResult['isDb'];
    // @ts-expect-error renamed
    type NoSeconds = NoteSegment['startSeconds'];
    expectTypeOf<sonare.EqBandInput['type']>().toEqualTypeOf<sonare.EqBandType | undefined>();
    expectTypeOf<sonare.EqBandInput['coeffMode']>().toEqualTypeOf<sonare.EqCoeffMode | undefined>();
    expectTypeOf<sonare.EqBandInput['phase']>().toEqualTypeOf<sonare.EqBandPhase | undefined>();
    // @ts-expect-error removed alias
    type NoLookahead = sonare.EqBandInput['lookaheadMs'];
    expectTypeOf<[NoBins, NoIsDb, NoSeconds, NoLookahead]>().not.toBeNever();
    expect(true).toBe(true);
  });

  it('declares sampleRate required on every sample request and splits the shared request types', () => {
    expectTypeOf<sonare.StftRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.LufsRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.TrimSilenceRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.FrameSignalRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.ZeroCrossingsRequest['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<sonare.CqtRequest['fminHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.PitchYinRequest['f0MinHz']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.ChromaCensRequest['binsPerOctave']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.OnsetStrengthRequest['nMels']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.PlpRequest['bpmMin']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.TempogramRatioRequest['tempogram']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.MelDeltaRequest['features']>().toEqualTypeOf<Matrix2D>();
    expectTypeOf<sonare.VectorNormalizeRequest['norm']>().toEqualTypeOf<
      sonare.NormType | undefined
    >();
    expectTypeOf<sonare.ToneRequest['durationSec']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.ClicksRequest['clickDurationSec']>().toEqualTypeOf<number | undefined>();
    expectTypeOf<sonare.NoteSegmentsRequest['referenceHz']>().toEqualTypeOf<number | undefined>();
    // @ts-expect-error removed catch-all request types
    type NoFeatureSamples = sonare.FeatureSamplesRequest;
    // @ts-expect-error removed catch-all request types
    type NoValues = sonare.ValuesRequest;
    // @ts-expect-error split into DeemphasisRequest / PreemphasisRequest
    type NoEmphasis = sonare.EmphasisRequest;
    // @ts-expect-error split into PitchYinRequest / PitchPyinRequest
    type NoPitch = sonare.PitchRequest;
    // @ts-expect-error flattened onto NoteSegmentsRequest
    type NoNoteConfig = sonare.NoteSegmentsConfig;
    // @ts-expect-error flattened onto PcenRequest
    type NoPcenOptions = sonare.PcenOptions;
    // @ts-expect-error carried by Matrix2D
    type NoSegmentMatrix = sonare.SegmentMatrix;
    // @ts-expect-error renamed OnsetStrengthRequest
    type NoOnsetEnvelope = sonare.OnsetEnvelopeRequest;
    // @ts-expect-error mfccToMel returns Matrix2D
    type NoInverseMel = sonare.InverseMelResult;
    // @ts-expect-error melToStft returns Matrix2D
    type NoInverseStft = sonare.InverseStftResult;
    // @ts-expect-error stft({ db: true }) carries db on StftResult
    type NoStftDb = sonare.StftDbResult;
    // @ts-expect-error renamed StreamConfig
    type NoStreamAnalyzerConfig = sonare.StreamAnalyzerConfig;
    // @ts-expect-error renamed StreamStats
    type NoStreamAnalyzerStats = sonare.StreamAnalyzerStats;
    // @ts-expect-error renamed StreamFrames
    type NoStreamFramesSoa = sonare.StreamFramesSoa;
    expectTypeOf<
      [
        NoFeatureSamples,
        NoValues,
        NoEmphasis,
        NoPitch,
        NoNoteConfig,
        NoPcenOptions,
        NoSegmentMatrix,
        NoOnsetEnvelope,
        NoInverseMel,
        NoInverseStft,
        NoStftDb,
        NoStreamAnalyzerConfig,
        NoStreamAnalyzerStats,
        NoStreamFramesSoa,
      ]
    >().not.toBeNever();
    expect(true).toBe(true);
  });
});

describe('noteSegments voicing input', () => {
  const f0Hz = new Float32Array(60).fill(440);
  const ones = new Float32Array(60).fill(1);
  const fromProb = () =>
    sonare.noteSegments({ f0Hz, voicedProb: ones, frameRate: 100, minNoteMs: 100 });

  it.each([
    ['Int32Array', new Int32Array(60).fill(1)],
    ['Uint8Array', new Uint8Array(60).fill(1)],
    ['Float32Array', new Float32Array(60).fill(1)],
    ['number[]', Array.from({ length: 60 }, () => 1)],
    ['boolean[]', Array.from({ length: 60 }, () => true)],
  ])('takes voiced as %s and equals the 0/1 voicedProb form', (_name, voiced) => {
    const fromVoiced = sonare.noteSegments({ f0Hz, voiced, frameRate: 100, minNoteMs: 100 });
    expect(fromVoiced.length).toBeGreaterThan(0);
    expect(fromVoiced).toEqual(fromProb());
  });

  it('refuses voiced and voicedProb given together, as two spellings of one input', () => {
    let caught: unknown;
    try {
      api.noteSegments({
        f0Hz,
        voiced: new Uint8Array(60).fill(1),
        voicedProb: new Float32Array(60),
        frameRate: 100,
      });
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(TypeError);
    expect((caught as Error).message).toBe(
      'noteSegments: voiced must be omitted when voicedProb is given',
    );
  });

  it('refuses a call with neither voiced nor voicedProb by name', () => {
    let caught: unknown;
    try {
      api.noteSegments({ f0Hz, frameRate: 100 } as never);
    } catch (error) {
      caught = error;
    }
    expect(caught).toBeInstanceOf(TypeError);
    expect((caught as Error).message).toBe(
      'noteSegments: voicedProb and voiced must not both be omitted',
    );
  });

  it('refuses the old voicedFlags key as unknown', () => {
    expect(() =>
      api.noteSegments({ f0Hz, voicedFlags: new Uint8Array(60).fill(1), frameRate: 100 } as never),
    ).toThrow(TypeError);
  });
});

describe('public request and constructor migration pins', () => {
  it('EqBandInput uses the current named token vocabulary', () => {
    expectTypeOf<sonare.EqBandInput['type']>().toEqualTypeOf<sonare.EqBandType | undefined>();
    expectTypeOf<sonare.EqBandInput['coeffMode']>().toEqualTypeOf<sonare.EqCoeffMode | undefined>();
    expectTypeOf<sonare.EqBandInput['phase']>().toEqualTypeOf<sonare.EqBandPhase | undefined>();
    expectTypeOf<sonare.EqBandInput['placement']>().toEqualTypeOf<
      sonare.EqStereoPlacement | undefined
    >();
    // @ts-expect-error lookaheadMs was removed from the band request
    type Removed = sonare.EqBandInput['lookaheadMs'];
    expectTypeOf<Removed>().not.toBeNever();
  });

  it('PlaybackTargetConfig uses speaker-role names as keys', () => {
    type SpeakerRole = keyof NonNullable<sonare.PlaybackTargetConfig['speakers']>;
    expectTypeOf<SpeakerRole>().toEqualTypeOf<
      | 'left'
      | 'right'
      | 'center'
      | 'leftSurround'
      | 'rightSurround'
      | 'leftSideSurround'
      | 'rightSideSurround'
    >();
  });

  it('RoomEstimateOptions uses named acoustic modes', () => {
    expectTypeOf<sonare.RoomEstimateOptions['mode']>().toEqualTypeOf<
      'auto' | 'blind' | 'impulseResponse' | undefined
    >();
    expect(() => sonare.estimateRoom({ samples, sampleRate, mode: 1 as never })).toThrow(TypeError);
  });

  it('StreamAnalyzer requires a StreamConfig at construction', () => {
    expectTypeOf<ConstructorParameters<typeof sonare.StreamAnalyzer>>().toEqualTypeOf<
      [StreamConfig]
    >();
    const noConfig = () => {
      // @ts-expect-error StreamAnalyzer no longer supplies a default config
      new sonare.StreamAnalyzer();
    };
    expect(typeof noConfig).toBe('function');
  });
});

describe('tempogram family on an empty onset envelope', () => {
  it('keeps the input-independent tempo axis and has no frames', () => {
    const onsetEnvelope = new Float32Array(0);
    const sampleRate = 22050;
    const shape = (m: { rows: number; cols: number; data: Float32Array }) => [
      m.rows,
      m.cols,
      m.data.length,
    ];
    expect(shape(sonare.tempogram({ onsetEnvelope, sampleRate, winLength: 64 }))).toEqual([
      64, 0, 0,
    ]);
    expect(shape(sonare.cyclicTempogram({ onsetEnvelope, sampleRate, nBins: 30 }))).toEqual([
      30, 0, 0,
    ]);
    expect(shape(sonare.fourierTempogram({ onsetEnvelope, sampleRate, winLength: 64 }))).toEqual([
      33, 0, 0,
    ]);
    expect(shape(sonare.fourierTempogram({ onsetEnvelope, sampleRate }))).toEqual([193, 0, 0]);
  });
});

describe('library-owned feature input domains', () => {
  it('lets PCEN own zero dimensions and keeps the matrix shape checks', () => {
    for (const [rows, cols, name] of [
      [0, 1, 'values.rows'],
      [-1, 1, 'values.rows'],
      [1, 0, 'values.cols'],
    ] as const) {
      const error = errorOf(() =>
        sonare.pcen({ values: { data: new Float32Array(0), rows, cols } }),
      );
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toBe(`pcen: ${name} must be an integer in [1, 2147483647]`);
    }
    expect(() =>
      sonare.pcen({ values: { data: new Float32Array([1]), rows: 1, cols: 2 } }),
    ).toThrow(new RangeError('pcen: values.data length must equal rows * cols'));
    expect(() => sonare.pcen({ values: [] as never })).toThrow(
      new TypeError('pcen: values must be a Matrix2D'),
    );
    for (const rows of [0.5, 2 ** 32]) {
      expect(() => sonare.pcen({ values: { data: new Float32Array(0), rows, cols: 1 } })).toThrow(
        new RangeError('pcen: values.rows must be an integer in [1, 2147483647]'),
      );
    }
    const accepted = sonare.pcen({ values: { data: new Float32Array([1, 1]), rows: 1, cols: 2 } });
    expect([accepted.rows, accepted.cols]).toEqual([1, 2]);
    const nonFinite = errorOf(() =>
      sonare.pcen({ values: { data: new Float32Array([1, Number.NaN]), rows: 1, cols: 2 } }),
    );
    expect(nonFinite).toBeInstanceOf(RangeError);
    expect((nonFinite as Error).message).toBe(
      'pcen: values contains a non-finite sample at index 1',
    );
  });
});
