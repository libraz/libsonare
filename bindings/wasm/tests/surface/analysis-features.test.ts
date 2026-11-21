import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import type { Matrix2D } from '../../dist/index.js';
import {
  amplitudeToDb,
  bassChroma,
  chirp,
  chroma,
  chromaCens,
  chromaCqt,
  clicks,
  cyclicTempogram,
  dbToAmplitude,
  dbToPower,
  deemphasis,
  estimateTuning,
  fixFrames,
  fixLength,
  frameSignal,
  framesToSamples,
  framesToTime,
  griffinLim,
  init,
  lufsMultichannel,
  melDelta,
  melSpectrogram,
  melToAudio,
  melToStft,
  mfcc,
  mfccToAudio,
  mfccToMel,
  noteSegments,
  onsetBacktrack,
  padCenter,
  pcen,
  peakPick,
  piptrack,
  pitchPyin,
  pitchTuning,
  pitchYin,
  plp,
  polyFeatures,
  powerToDb,
  preemphasis,
  reassignedSpectrogram,
  referenceHzToTuning,
  resample,
  rmsEnergy,
  samplesToFrames,
  segmentAgglomerative,
  segmentCrossSimilarity,
  segmentLagToRecurrence,
  segmentPathEnhance,
  segmentRecurrenceMatrix,
  segmentRecurrenceToLag,
  segmentSubsegment,
  spectralBandwidth,
  spectralCentroid,
  spectralContrast,
  spectralFlatness,
  spectralFlux,
  spectralRolloff,
  splitSilence,
  splitSilenceCommon,
  splitSilenceCommonWithReport,
  stft,
  tempogram,
  timeToFrames,
  tone,
  tonnetz,
  trimSilence,
  tuningToReferenceHz,
  vectorNormalize,
  zeroCrossingRate,
  zeroCrossings,
} from '../../dist/index.js';

// biome-ignore lint/suspicious/noExplicitAny: the table drives many differently typed entry points
type AnyFn = (...args: any[]) => unknown;

const sr = 22050;

function sine(frequency: number, seconds: number): Float32Array {
  const out = new Float32Array(Math.floor(sr * seconds));
  for (let i = 0; i < out.length; i++) {
    out[i] = 0.4 * Math.sin((2 * Math.PI * frequency * i) / sr);
  }
  return out;
}

const samples = sine(440, 0.5);
const envelope = Float32Array.from({ length: 64 }, (_, i) => (i % 8 === 0 ? 1 : 0.1));

function matrix(rows: number, cols: number, fill = 0.5): Matrix2D {
  return { data: new Float32Array(rows * cols).fill(fill), rows, cols };
}

function caught(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

function isMatrix(value: unknown): value is Matrix2D {
  const m = value as Matrix2D;
  return (
    m !== null &&
    typeof m === 'object' &&
    m.data instanceof Float32Array &&
    Number.isInteger(m.rows) &&
    Number.isInteger(m.cols) &&
    m.data.length === m.rows * m.cols
  );
}

beforeAll(async () => {
  await init();
});

describe('analysis features: sample-taking one-shots', () => {
  const sampleTaking: Array<[string, AnyFn, Record<string, unknown>]> = [
    ['stft', stft, {}],
    ['melSpectrogram', melSpectrogram, {}],
    ['mfcc', mfcc, {}],
    ['chroma', chroma, {}],
    ['chromaCens', chromaCens, {}],
    ['chromaCqt', chromaCqt, {}],
    ['bassChroma', bassChroma, {}],
    ['reassignedSpectrogram', reassignedSpectrogram, {}],
    ['spectralCentroid', spectralCentroid, {}],
    ['spectralBandwidth', spectralBandwidth, {}],
    ['spectralRolloff', spectralRolloff, {}],
    ['spectralFlatness', spectralFlatness, {}],
    ['spectralFlux', spectralFlux, {}],
    ['spectralContrast', spectralContrast, {}],
    ['polyFeatures', polyFeatures, {}],
    ['zeroCrossingRate', zeroCrossingRate, {}],
    ['zeroCrossings', zeroCrossings, {}],
    ['rmsEnergy', rmsEnergy, {}],
    ['piptrack', piptrack, {}],
    ['pitchYin', pitchYin, {}],
    ['pitchPyin', pitchPyin, {}],
    ['estimateTuning', estimateTuning, {}],
    ['preemphasis', preemphasis, {}],
    ['deemphasis', deemphasis, {}],
    ['splitSilence', splitSilence, {}],
    ['trimSilence', trimSilence, {}],
    ['frameSignal', frameSignal, { frameLength: 1024, hopLength: 512 }],
  ];

  it.each(sampleTaking)('%s requires sampleRate in both forms', (_name, fn, extra) => {
    expect(caught(() => fn(samples))).toBeInstanceOf(TypeError);
    expect(caught(() => fn({ samples, ...extra }))).toBeInstanceOf(TypeError);
    expect(caught(() => fn(samples, undefined, extra))).toBeInstanceOf(TypeError);
  });

  it.each(sampleTaking)(
    '%s refuses a non-integer sample rate with RangeError',
    (_name, fn, extra) => {
      expect(caught(() => fn({ samples, sampleRate: 22050.5, ...extra }))).toBeInstanceOf(
        RangeError,
      );
      expect(caught(() => fn(samples, 0, extra))).toBeInstanceOf(RangeError);
    },
  );

  it.each(sampleTaking)('%s refuses the old positional tail', (_name, fn, extra) => {
    expect(caught(() => fn(samples, sr, 1024, 256))).toBeInstanceOf(TypeError);
    expect(caught(() => fn({ samples, sampleRate: sr, ...extra }, sr))).toBeInstanceOf(TypeError);
  });

  it.each(sampleTaking)('%s accepts the request and positional forms alike', (_name, fn, extra) => {
    const viaRequest = fn({ samples, sampleRate: sr, ...extra });
    const viaPositional = fn(samples, sr, extra);
    expect(viaPositional).toEqual(viaRequest);
  });

  it.each(sampleTaking)('%s refuses validate as an unknown option', (_name, fn, extra) => {
    expect(() => fn(samples, sr, { ...extra, validate: false } as never)).toThrow(
      /unknown key validate \(expected /,
    );
  });

  it.each(sampleTaking)('%s refuses non-finite samples with RangeError', (_name, fn, extra) => {
    const bad = Float32Array.from(samples);
    bad[10] = Number.NaN;
    expect(caught(() => fn(bad, sr, extra))).toBeInstanceOf(RangeError);
    expect(caught(() => fn({ samples: bad, sampleRate: sr, ...extra }))).toBeInstanceOf(RangeError);
  });

  it('refuses an unknown option by name in the positional form', () => {
    expect(caught(() => stft(samples, sr, { notAnOption: 1 } as never))).toBeInstanceOf(TypeError);
  });
});

describe('analysis features: renamed unit fields', () => {
  it.each([
    ['melSpectrogram fmin', () => melSpectrogram({ samples, sampleRate: sr, fmin: 0 } as never)],
    ['melSpectrogram fmax', () => melSpectrogram({ samples, sampleRate: sr, fmax: 8000 } as never)],
    ['mfcc fmin', () => mfcc({ samples, sampleRate: sr, fmin: 0 } as never)],
    ['piptrack fmin', () => piptrack({ samples, sampleRate: sr, fmin: 100 } as never)],
    ['pitchYin fmin', () => pitchYin({ samples, sampleRate: sr, fmin: 65 } as never)],
    ['pitchPyin fmax', () => pitchPyin({ samples, sampleRate: sr, fmax: 2000 } as never)],
    [
      'spectralContrast fmin',
      () => spectralContrast({ samples, sampleRate: sr, fmin: 200 } as never),
    ],
    ['tone frequency', () => tone({ frequency: 440 } as never)],
    ['tone duration', () => tone({ duration: 1 } as never)],
    ['chirp fmin', () => chirp({ fmin: 100 } as never)],
    ['chirp duration', () => chirp({ duration: 1 } as never)],
    [
      'clicks frequency',
      () => clicks({ times: new Float32Array([0.1]), frequency: 1000 } as never),
    ],
    [
      'clicks clickDuration',
      () => clicks({ times: new Float32Array([0.1]), clickDuration: 0.1 } as never),
    ],
    ['plp tempoMin', () => plp({ onsetEnvelope: envelope, tempoMin: 60 } as never)],
    ['plp tempoMax', () => plp({ onsetEnvelope: envelope, tempoMax: 200 } as never)],
    ['resample srcSr', () => resample({ samples, srcSr: sr, targetSr: 16000 } as never)],
    ['vectorNormalize normType', () => vectorNormalize({ values: envelope, normType: 2 } as never)],
  ])('refuses the old %s name', (_label, call) => {
    expect(caught(call)).toBeInstanceOf(TypeError);
  });

  it('takes the *Hz and *Sec names', () => {
    const mel = melSpectrogram({
      samples,
      sampleRate: sr,
      nMels: 20,
      fminHz: 100,
      fmaxHz: 8000,
    });
    expect(mel.fminHz).toBe(100);
    expect(mel.fmaxHz).toBe(8000);
    expect(mel).not.toHaveProperty('fmin');
    expect(mel).not.toHaveProperty('fmax');
    expect(tone({ frequencyHz: 440, durationSec: 0.1, sampleRate: sr }).length).toBe(2205);
    expect(chirp({ fminHz: 220, fmaxHz: 440, durationSec: 0.1, sampleRate: sr }).length).toBe(2205);
    expect(piptrack({ samples, sampleRate: sr, fminHz: 100, fmaxHz: 2000 })).toBeDefined();
    expect(pitchYin({ samples, sampleRate: sr, f0MinHz: 100, f0MaxHz: 1000 })).toBeDefined();
    expect(spectralContrast({ samples, sampleRate: sr, fminHz: 200 })).toBeDefined();
    const click = clicks({
      times: new Float32Array([0.05]),
      sampleRate: sr,
      length: 4410,
      frequencyHz: 1000,
      clickDurationSec: 0.01,
    });
    expect(click.length).toBe(4410);
  });

  it('defaults the chirp sweep to exponential', () => {
    const base = { fminHz: 220, fmaxHz: 880, durationSec: 0.1, sampleRate: sr };
    expect(chirp(base)).toEqual(chirp({ ...base, linear: false }));
    expect(chirp(base)).not.toEqual(chirp({ ...base, linear: true }));
  });

  it('reports note segment bounds in seconds under the *Sec names', () => {
    const frames = 40;
    const f0Hz = new Float32Array(frames).fill(440);
    const voicedProb = new Float32Array(frames).fill(1);
    const segments = noteSegments({ f0Hz, voicedProb, frameRate: 100 });
    expect(segments.length).toBeGreaterThan(0);
    const first = segments[0] as unknown as Record<string, unknown>;
    expect(typeof first.startSec).toBe('number');
    expect(typeof first.endSec).toBe('number');
    expect(first).not.toHaveProperty('startSeconds');
    expect(first).not.toHaveProperty('endSeconds');
  });

  it('names the loudness range in LU', () => {
    const plane = new Float32Array(sr).map((_, i) => 0.2 * Math.sin(i * 0.05));
    const result = lufsMultichannel({ channels: [plane, plane], sampleRate: sr });
    expect(typeof result.loudnessRangeLu).toBe('number');
    expect(result).not.toHaveProperty('loudnessRange');
  });
});

describe('analysis features: request-only one-shots', () => {
  const requestOnly: Array<[string, AnyFn, unknown[]]> = [
    ['tone', tone, [440]],
    ['chirp', chirp, [220, 440]],
    ['clicks', clicks, [new Float32Array([0.1])]],
    ['powerToDb', powerToDb, [envelope]],
    ['amplitudeToDb', amplitudeToDb, [envelope]],
    ['dbToPower', dbToPower, [envelope]],
    ['dbToAmplitude', dbToAmplitude, [envelope]],
    ['padCenter', padCenter, [envelope, 70]],
    ['fixLength', fixLength, [envelope, 70]],
    ['fixFrames', fixFrames, [new Int32Array([1, 2, 3])]],
    ['onsetBacktrack', onsetBacktrack, [new Int32Array([3]), envelope]],
    ['peakPick', peakPick, [envelope, 1, 1, 1, 1, 0.1, 1]],
    ['vectorNormalize', vectorNormalize, [envelope]],
    ['pcen', pcen, [envelope, 8, 8]],
    ['tonnetz', tonnetz, [envelope, 12, 5]],
    ['tempogram', tempogram, [envelope]],
    ['cyclicTempogram', cyclicTempogram, [envelope]],
    ['plp', plp, [envelope]],
    ['pitchTuning', pitchTuning, [new Float32Array([440, 442])]],
    ['melDelta', melDelta, [envelope, 8, 8]],
    ['griffinLim', griffinLim, [envelope, 8, 8]],
    ['melToStft', melToStft, [envelope, 8, 8]],
    ['melToAudio', melToAudio, [envelope, 8, 8]],
    ['mfccToMel', mfccToMel, [envelope, 8, 8]],
    ['mfccToAudio', mfccToAudio, [envelope, 8, 8]],
    ['splitSilenceCommon', splitSilenceCommon, [[samples]]],
    ['splitSilenceCommonWithReport', splitSilenceCommonWithReport, [[samples]]],
  ];

  it.each(requestOnly)('%s refuses the removed positional form', (_name, fn, args) => {
    expect(caught(() => fn(...args))).toBeInstanceOf(TypeError);
  });

  it.each(requestOnly)('%s refuses a request with an extra positional argument', (_name, fn) => {
    expect(caught(() => fn({}, 1))).toBeInstanceOf(TypeError);
  });

  it('accepts the request form of each', () => {
    expect(tone({ frequencyHz: 440, durationSec: 0.05, sampleRate: sr }).length).toBe(1102);
    expect(powerToDb({ values: envelope }).length).toBe(envelope.length);
    expect(amplitudeToDb({ values: envelope, topDb: 60 }).length).toBe(envelope.length);
    expect(dbToPower({ values: envelope, ref: 1 }).length).toBe(envelope.length);
    expect(dbToAmplitude({ values: envelope, ref: 1 }).length).toBe(envelope.length);
    expect(padCenter({ values: envelope, targetSize: 70 }).length).toBe(70);
    expect(fixLength({ values: envelope, targetSize: 70 }).length).toBe(70);
    expect(fixFrames({ frames: [1, 2, 3], xMax: 10 }).length).toBeGreaterThan(0);
    expect(onsetBacktrack({ events: [3], energy: envelope }).length).toBe(1);
    expect(
      peakPick({
        values: envelope,
        preMax: 1,
        postMax: 1,
        preAvg: 1,
        postAvg: 1,
        delta: 0.1,
        wait: 1,
      }).length,
    ).toBeGreaterThan(0);
  });

  it('refuses an explicit zero clicks length', () => {
    const times = new Float32Array([0.1]);
    expect(caught(() => clicks({ times, length: 0 }))).toBeInstanceOf(RangeError);
    expect(clicks({ times, sampleRate: sr }).length).toBeGreaterThan(0);
  });
});

describe('analysis features: scalar conversions are positional only', () => {
  it('refuses a request object', () => {
    expect(caught(() => framesToTime({ frames: 4 } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => timeToFrames({ time: 1 } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => framesToSamples({ frames: 4 } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => samplesToFrames({ samples: 4 } as never))).toBeInstanceOf(TypeError);
  });

  it('keeps the positional results', () => {
    expect(framesToTime(43, sr, 512)).toBeCloseTo((43 * 512) / sr, 5);
    expect(timeToFrames(1, sr, 512)).toBe(Math.floor(sr / 512));
    expect(framesToSamples(4, 512)).toBe(2048);
    expect(samplesToFrames(2048, 512)).toBe(4);
  });

  it('refuses a hop that is not a positive integer on every frame conversion', () => {
    const calls: Array<[string, (hop: number) => number]> = [
      ['framesToTime', (hop) => framesToTime(4, sr, hop)],
      ['timeToFrames', (hop) => timeToFrames(1, sr, hop)],
      ['framesToSamples', (hop) => framesToSamples(4, hop)],
      ['samplesToFrames', (hop) => samplesToFrames(2048, hop)],
    ];
    for (const [name, call] of calls) {
      for (const hop of [0, -5]) {
        expect(() => call(hop)).toThrow(`${name}: hopLength must be an integer in [1, 2147483647]`);
      }
      expect(caught(() => call(1.5))).toBeInstanceOf(RangeError);
    }
  });

  it('treats an absent nFft as no centring and refuses an explicit zero or negative', () => {
    expect(framesToSamples(4, 512, 2048)).toBe(4 * 512 + 1024);
    expect(caught(() => framesToSamples(4, 512, 0))).toBeInstanceOf(RangeError);
    expect(() => samplesToFrames(2048, 512, 0)).toThrow(
      'samplesToFrames: nFft must not be 0; omit it for no centring',
    );
    expect(caught(() => framesToSamples(4, 512, -5))).toBeInstanceOf(RangeError);
    expect(caught(() => samplesToFrames(2048, 512, -5))).toBeInstanceOf(RangeError);
  });
});

describe('analysis features: stft db', () => {
  it('stft without db returns no db plane and the same magnitude as with db', () => {
    const plain = stft(samples, sr);
    const withDb = stft(samples, sr, { db: true });
    expect('db' in plain).toBe(false);
    expect(plain.magnitude).toEqual(withDb.magnitude);
    expect(plain.power).toEqual(withDb.power);
    const db = withDb.db as Matrix2D;
    expect(isMatrix(db)).toBe(true);
    expect(db.rows).toBe(withDb.magnitude.rows);
    expect(db.cols).toBe(withDb.magnitude.cols);
    expect(db.rows).toBe(plain.nFft / 2 + 1);
  });
});

describe('analysis features: Matrix2D results', () => {
  it.each([
    ['stft magnitude', () => stft(samples, sr).magnitude],
    ['stft power', () => stft(samples, sr).power],
    ['stft db', () => stft(samples, sr, { db: true }).db],
    ['melSpectrogram power', () => melSpectrogram(samples, sr).power],
    ['melSpectrogram db', () => melSpectrogram(samples, sr).db],
    ['mfcc coefficients', () => mfcc(samples, sr).coefficients],
    ['chroma features', () => chroma(samples, sr).features],
    ['chromaCens features', () => chromaCens(samples, sr).features],
    ['chromaCqt features', () => chromaCqt(samples, sr).features],
    ['bassChroma features', () => bassChroma(samples, sr).features],
    ['piptrack pitches', () => piptrack(samples, sr).pitches],
    ['piptrack magnitudes', () => piptrack(samples, sr).magnitudes],
    ['reassigned magnitude', () => reassignedSpectrogram(samples, sr).magnitude],
    ['reassigned times', () => reassignedSpectrogram(samples, sr).times],
    ['reassigned frequencies', () => reassignedSpectrogram(samples, sr).frequencies],
    ['frameSignal', () => frameSignal(samples, sr, { frameLength: 1024, hopLength: 512 })],
    ['spectralContrast', () => spectralContrast(samples, sr)],
    ['polyFeatures', () => polyFeatures(samples, sr)],
    ['tempogram', () => tempogram({ onsetEnvelope: envelope })],
    ['cyclicTempogram', () => cyclicTempogram({ onsetEnvelope: envelope, sampleRate: sr })],
    ['tonnetz', () => tonnetz({ chromagram: matrix(12, 5) })],
    ['pcen', () => pcen({ values: matrix(8, 6) })],
    ['melDelta', () => melDelta({ features: matrix(4, 12) })],
    ['melToStft', () => melToStft({ power: matrix(20, 6), nFft: 1024 })],
    ['mfccToMel', () => mfccToMel({ coefficients: matrix(13, 6), nMels: 20 })],
  ])('%s is a row-major Matrix2D', (_name, call) => {
    const value = call();
    expect(isMatrix(value)).toBe(true);
    expect(value).not.toHaveProperty('nFrames');
    expect(value).not.toHaveProperty('nBins');
  });

  it('puts bins on rows and frames on columns', () => {
    const result = stft(samples, sr, { nFft: 1024, hopLength: 256 });
    expect(result.magnitude.rows).toBe(513);
    expect(result.power.rows).toBe(513);
    expect(result.magnitude.cols).toBe(result.power.cols);
    expect(result).not.toHaveProperty('nBins');
    expect(result).not.toHaveProperty('nFrames');
    const mel = melSpectrogram(samples, sr, { nMels: 20 });
    expect(mel.power.rows).toBe(20);
    expect(mel).not.toHaveProperty('nMels');
    expect(mel).not.toHaveProperty('isDb');
    const coefficients = mfcc(samples, sr, { nMfcc: 13 });
    expect(coefficients.coefficients.rows).toBe(13);
    expect(coefficients).not.toHaveProperty('nMfcc');
    expect(coefficients).not.toHaveProperty('isDb');
    expect(chroma(samples, sr).features.rows).toBe(12);
    const framed = frameSignal(samples, sr, { frameLength: 1024, hopLength: 512 });
    expect(framed.rows).toBe(1024);
    for (let i = 0; i < 1024; i++) {
      expect(framed.data[i * framed.cols + 1]).toBe(samples[512 + i]);
    }
    expect(tonnetz({ chromagram: matrix(12, 5) }).rows).toBe(6);
  });

  it('takes Matrix2D inputs, refusing a flat array with a dimension', () => {
    const flat = new Float32Array(48).fill(0.5);
    expect(caught(() => melDelta({ features: flat } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => tonnetz({ chromagram: flat } as never))).toBeInstanceOf(TypeError);
    expect(caught(() => pcen({ values: flat } as never))).toBeInstanceOf(TypeError);
    const ragged = { data: new Float32Array(10), rows: 4, cols: 4 };
    expect(caught(() => melDelta({ features: ragged }))).toBeInstanceOf(RangeError);
  });

  it('refuses the removed dimension fields and the nested pcen options', () => {
    expect(
      caught(() => melDelta({ features: matrix(4, 12), nFeatures: 4, nFrames: 12 } as never)),
    ).toBeInstanceOf(TypeError);
    expect(
      caught(() => pcen({ values: matrix(8, 6), options: { gain: 0.5 } } as never)),
    ).toBeInstanceOf(TypeError);
    expect(
      caught(() => tonnetz({ chromagram: matrix(12, 5), nChroma: 12, nFrames: 5 } as never)),
    ).toBeInstanceOf(TypeError);
  });

  it('keeps the cqt-free spectral summaries as Float32Array series', () => {
    expect(spectralCentroid(samples, sr)).toBeInstanceOf(Float32Array);
    expect(rmsEnergy(samples, sr)).toBeInstanceOf(Float32Array);
  });
});

describe('analysis features: inverse transforms', () => {
  it('round-trips through Matrix2D without the result form', () => {
    const mel = melSpectrogram(samples, sr, { nMels: 20 });
    const magnitude = melToStft({ power: mel.power, sampleRate: sr, nFft: mel.nFft });
    expect(isMatrix(magnitude)).toBe(true);
    expect(magnitude.rows).toBe(mel.nFft / 2 + 1);
    expect(magnitude.cols).toBe(mel.power.cols);
    const audio = melToAudio({ power: mel.power, sampleRate: sr, nFft: mel.nFft, nIter: 2 });
    expect(audio).toBeInstanceOf(Float32Array);
    const coefficients = mfcc(samples, sr, { nMfcc: 13, nMels: 20 });
    const back = mfccToMel({ coefficients: coefficients.coefficients, nMels: 20 });
    expect(back.rows).toBe(20);
    expect(back.cols).toBe(coefficients.coefficients.cols);
    expect(
      mfccToAudio({ coefficients: coefficients.coefficients, nMels: 20, sampleRate: sr, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
    expect(
      griffinLim({ magnitude: stft(samples, sr).magnitude, sampleRate: sr, nIter: 2 }),
    ).toBeInstanceOf(Float32Array);
    expect(() =>
      griffinLim({ magnitude: stft(samples, sr).magnitude, sampleRate: sr, nFft: 1024, nIter: 2 }),
    ).toThrow(
      new RangeError('griffinLim: nFft must equal (magnitude.rows - 1) * 2, which is 2048'),
    );
  });

  it('derives an omitted inverse hop as a quarter of nFft', () => {
    const nFft = 512;
    const mel = melSpectrogram(samples, sr, { nMels: 20, nFft });
    const cepstrum = mfcc(samples, sr, { nMfcc: 13, nMels: 20, nFft });
    const spectrum = stft(samples, sr, { nFft, hopLength: 128 });
    const common = { sampleRate: sr, nFft, nIter: 2 };
    const pairs: Array<[string, (hop?: number) => Float32Array]> = [
      ['melToAudio', (hop) => melToAudio({ power: mel.power, ...common, hopLength: hop })],
      [
        'mfccToAudio',
        (hop) =>
          mfccToAudio({
            coefficients: cepstrum.coefficients,
            nMels: 20,
            ...common,
            hopLength: hop,
          }),
      ],
      [
        'griffinLim',
        (hop) => griffinLim({ magnitude: spectrum.magnitude, ...common, hopLength: hop }),
      ],
    ];
    for (const [name, call] of pairs) {
      expect(Array.from(call()), name).toEqual(Array.from(call(128)));
      expect(Array.from(call()), name).not.toEqual(Array.from(call(256)));
    }
  });

  it.each([
    ['melToStft', (result: unknown) => melToStft({ result } as never)],
    ['melToAudio', (result: unknown) => melToAudio({ result } as never)],
    ['mfccToMel', (result: unknown) => mfccToMel({ result } as never)],
    ['mfccToAudio', (result: unknown) => mfccToAudio({ result } as never)],
  ])('%s no longer takes a forward result', (_name, call) => {
    expect(caught(() => call(melSpectrogram(samples, sr)))).toBeInstanceOf(TypeError);
  });

  it.each([
    ['fmin', () => melToStft({ power: matrix(20, 6), fmin: 0 } as never)],
    ['nMels', () => melToStft({ power: matrix(20, 6), nMels: 20 } as never)],
    ['nFrames', () => melToAudio({ power: matrix(20, 6), nFrames: 6 } as never)],
  ])('refuses the removed %s field', (_name, call) => {
    expect(caught(call)).toBeInstanceOf(TypeError);
  });

  it('takes fminHz and fmaxHz', () => {
    expect(melToStft({ power: matrix(20, 6), nFft: 1024, fminHz: 100, fmaxHz: 8000 }).rows).toBe(
      513,
    );
  });
});

describe('analysis features: closed sets', () => {
  it.each([
    ['number 0', 0],
    ['number 1', 1],
    ['synonym ac', 'ac'],
    ['synonym auto', 'auto'],
    ['unknown', 'fourier'],
  ])('tempogram refuses mode %s', (_label, mode) => {
    const error = caught(() => tempogram({ onsetEnvelope: envelope, mode } as never));
    expect(error).toBeInstanceOf(Error);
    expect([TypeError, RangeError].some((ctor) => error instanceof ctor)).toBe(true);
  });

  it.each(['autocorrelation', 'cosine'] as const)('tempogram accepts mode %s', (mode) => {
    expect(isMatrix(tempogram({ onsetEnvelope: envelope, mode }))).toBe(true);
  });

  it.each(['max', 'l1', 'l2', 'power'] as const)('vectorNormalize accepts norm %s', (norm) => {
    expect(vectorNormalize({ values: envelope, norm }).length).toBe(envelope.length);
  });

  it.each([
    ['number', 2],
    ['unknown token', 'l3'],
  ])('vectorNormalize refuses a %s norm', (_label, norm) => {
    const error = caught(() => vectorNormalize({ values: envelope, norm } as never));
    expect([TypeError, RangeError].some((ctor) => error instanceof ctor)).toBe(true);
  });

  it('vectorNormalize l2 matches the unit-norm result', () => {
    const out = vectorNormalize({ values: new Float32Array([3, 4]), norm: 'l2' });
    expect(out[0]).toBeCloseTo(0.6, 5);
    expect(out[1]).toBeCloseTo(0.8, 5);
  });
});

describe('analysis features: tuning in cents', () => {
  it('converts a cent offset to a reference frequency and back', () => {
    expect(tuningToReferenceHz(100)).toBeCloseTo(440 * 2 ** (1 / 12), 3);
    expect(tuningToReferenceHz(0, 432)).toBeCloseTo(432, 5);
    expect(referenceHzToTuning(440 * 2 ** (1 / 12))).toBeCloseTo(100, 3);
    expect(referenceHzToTuning(432, 432)).toBeCloseTo(0, 5);
  });

  it('reports estimateTuning and pitchTuning in cents', () => {
    const detuned = sine(440 * 2 ** (0.3 / 12), 1);
    const cents = estimateTuning(detuned, sr);
    expect(Math.abs(cents - 30)).toBeLessThan(12);
    expect(
      Math.abs(pitchTuning({ frequencies: new Float32Array([440 * 2 ** (0.3 / 12)]) }) - 30),
    ).toBeLessThan(2);
  });

  it('refuses the old a4 / hz names', () => {
    expect(() => tuningToReferenceHz(0, 'x' as never)).toThrow();
  });
});

describe('analysis features: plp tempo range', () => {
  it('defaults to 60-200 BPM, passed explicitly', () => {
    const tracked = plp({ onsetEnvelope: envelope });
    expect(plp({ onsetEnvelope: envelope, bpmMin: 60, bpmMax: 200 })).toEqual(tracked);
    expect(plp({ onsetEnvelope: envelope, bpmMin: 30, bpmMax: 300 })).not.toEqual(tracked);
  });
});

describe('analysis features: resample and silence', () => {
  it('resamples by source and target rate, request and positional', () => {
    const viaRequest = resample({ samples, sampleRate: sr, targetSampleRate: 16000 });
    expect(resample(samples, sr, 16000)).toEqual(viaRequest);
    expect(
      caught(() => resample({ samples, sourceSampleRate: sr, targetSampleRate: 16000 } as never)),
    ).toBeInstanceOf(TypeError);
    expect(viaRequest.length).toBe(Math.round((samples.length * 16000) / sr));
    expect(
      caught(() => resample({ samples, sampleRate: 22050.5, targetSampleRate: 16000 })),
    ).toBeInstanceOf(RangeError);
    expect(caught(() => resample({ samples, sampleRate: sr } as never))).toBeInstanceOf(TypeError);
  });

  it('returns a declared trim result', () => {
    const padded = new Float32Array(sr);
    padded.set(samples, 4000);
    const trimmed = trimSilence(padded, sr);
    expect(Object.keys(trimmed).sort()).toEqual(['audio', 'endSample', 'startSample']);
  });

  it('splits the common silence of several signals by request', () => {
    const intervals = splitSilenceCommon({ signals: [samples, samples] });
    expect(intervals).toBeInstanceOf(Int32Array);
    expect(splitSilenceCommonWithReport({ signals: [samples] }).report).toBeDefined();
  });
});

describe('analysis features: segmentation matrices', () => {
  const features = matrix(4, 12, 0);
  for (let i = 0; i < features.data.length; i++) {
    features.data[i] = Math.sin(i * 0.7);
  }

  it('takes and returns Matrix2D', () => {
    const recurrence = segmentRecurrenceMatrix({ data: features });
    expect(isMatrix(recurrence)).toBe(true);
    expect(recurrence.rows).toBe(12);
    expect(recurrence.cols).toBe(12);
    expect(recurrence).not.toHaveProperty('values');
    const lag = segmentRecurrenceToLag({ recurrence });
    expect(isMatrix(lag)).toBe(true);
    expect(isMatrix(segmentLagToRecurrence({ lag }))).toBe(true);
    expect(isMatrix(segmentPathEnhance({ recurrence, win: 3 }))).toBe(true);
    const cross = segmentCrossSimilarity({ x: features, y: matrix(4, 7, 0.3) });
    expect(isMatrix(cross)).toBe(true);
    expect(segmentAgglomerative({ data: features, k: 2 }).length).toBe(12);
    expect(
      segmentSubsegment({ data: features, boundaries: [0, 6], nSegments: 2 }).length,
    ).toBeGreaterThan(0);
  });

  it('segment similarity refuses a zero or negative k and takes the librosa default when it is omitted', () => {
    const data: Matrix2D = {
      data: Float32Array.from({ length: 32 }, (_, i) => Math.sin(i)),
      rows: 4,
      cols: 8,
    };
    expect(segmentRecurrenceMatrix({ data }).rows).toBe(8);
    expect(segmentRecurrenceMatrix({ data, k: 2 }).rows).toBe(8);
    for (const k of [0, -1]) {
      expect(caught(() => segmentRecurrenceMatrix({ data, k }))).toBeInstanceOf(RangeError);
      expect(caught(() => segmentCrossSimilarity({ x: data, y: data, k }))).toBeInstanceOf(
        RangeError,
      );
    }
  });

  it('refuses the flat array with explicit dimensions', () => {
    expect(
      caught(() => segmentRecurrenceMatrix({ data: features.data, rows: 4, cols: 12 } as never)),
    ).toBeInstanceOf(TypeError);
    expect(caught(() => segmentRecurrenceToLag({ recurrence: features }))).toBeInstanceOf(
      RangeError,
    );
  });
});

describe('analysis features: types', () => {
  it('declares the shapes the table names', () => {
    expectTypeOf(stft(samples, sr).magnitude).toEqualTypeOf<Matrix2D>();
    expectTypeOf(stft(samples, sr, { db: true }).db).toEqualTypeOf<Matrix2D | undefined>();
    expectTypeOf(melToStft({ power: matrix(1, 1) })).toEqualTypeOf<Matrix2D>();
    expectTypeOf(mfccToMel({ coefficients: matrix(1, 1) })).toEqualTypeOf<Matrix2D>();
    expectTypeOf(frameSignal).returns.toEqualTypeOf<Matrix2D>();
    expectTypeOf(segmentRecurrenceMatrix).returns.toEqualTypeOf<Matrix2D>();
    expectTypeOf(estimateTuning).returns.toEqualTypeOf<number>();
    const typeOnly = () => {
      // @ts-expect-error samples without a rate
      stft(samples);
      // @ts-expect-error scalar conversions are positional only
      framesToTime({ frames: 1 });
      // @ts-expect-error the old flat-array form
      melDelta(new Float32Array(4), 2, 2);
    };
    expect(typeof typeOnly).toBe('function');
  });
});

describe('analysis features: note segments voicing', () => {
  const frames = 40;
  const f0Hz = new Float32Array(frames).fill(440);
  const viaProb = () =>
    noteSegments({ f0Hz, voicedProb: new Float32Array(frames).fill(1), frameRate: 100 });

  it.each([
    ['Int32Array', new Int32Array(frames).fill(1)],
    ['Uint8Array', new Uint8Array(frames).fill(1)],
    ['Float32Array', new Float32Array(frames).fill(1)],
    ['number[]', Array.from({ length: frames }, () => 1)],
    ['boolean[]', Array.from({ length: frames }, () => true)],
  ])('takes voiced as %s in place of a probability', (_name, voiced) => {
    const viaVoiced = noteSegments({ f0Hz, voiced, frameRate: 100 });
    expect(viaVoiced.length).toBeGreaterThan(0);
    expect(viaVoiced).toEqual(viaProb());
  });

  it('lets voiced take precedence over voicedProb', () => {
    const silent = new Float32Array(frames);
    const both = noteSegments({
      f0Hz,
      voiced: new Uint8Array(frames).fill(1),
      voicedProb: silent,
      frameRate: 100,
    });
    expect(both).toEqual(viaProb());
    expect(noteSegments({ f0Hz, voicedProb: silent, frameRate: 100 })).toEqual([]);
  });

  it('refuses a call with neither voiced nor voicedProb by name', () => {
    expect(() => noteSegments({ f0Hz, frameRate: 100 } as never)).toThrow(
      /give voiced or voicedProb/,
    );
    expect(() => noteSegments({ f0Hz, frameRate: 100 } as never)).toThrow(TypeError);
  });

  it('refuses the old voicedFlags key as unknown', () => {
    expect(() =>
      noteSegments({ f0Hz, voicedFlags: new Uint8Array(frames).fill(1), frameRate: 100 } as never),
    ).toThrow(TypeError);
  });

  it('segments a pitchPyin track through its voicedFlag', () => {
    const sr = 22050;
    const samples = new Float32Array(sr).map(
      (_, i) => 0.4 * Math.sin((2 * Math.PI * 440 * i) / sr),
    );
    const track = pitchPyin(samples, sr);
    const segments = noteSegments({
      f0Hz: track.f0,
      voiced: track.voicedFlag,
      frameRate: sr / 512,
    });
    expect(segments.length).toBeGreaterThan(0);
  });
});
