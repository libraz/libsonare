import { describe, expect, it } from 'vitest';
import type { Matrix2D } from '../../dist/index.js';
import * as api from '../../dist/index.js';
import { Audio, decodeChannels, ErrorCode, SonareError } from '../../dist/index.js';

const sampleRate = 22050;

function signal(): Float32Array {
  const out = new Float32Array(sampleRate / 2);
  for (let i = 0; i < out.length; i++) {
    const t = i / sampleRate;
    out[i] = 0.3 * Math.sin(2 * Math.PI * 220 * t) + 0.2 * Math.sin(2 * Math.PI * 440 * t);
    if (i % 5512 < 64) {
      out[i] += 0.4;
    }
  }
  return out;
}

/** Field-by-field comparison within a float tolerance. */
function expectClose(actual: unknown, expected: unknown, path = 'result'): void {
  if (typeof expected === 'number') {
    expect(typeof actual, path).toBe('number');
    if (Number.isNaN(expected)) {
      expect(Number.isNaN(actual as number), path).toBe(true);
    } else {
      const tolerance = 1e-5 * Math.max(1, Math.abs(expected));
      expect(Math.abs((actual as number) - expected), path).toBeLessThanOrEqual(tolerance);
    }
    return;
  }
  if (ArrayBuffer.isView(expected)) {
    expect(ArrayBuffer.isView(actual), path).toBe(true);
    const a = actual as unknown as ArrayLike<number>;
    const e = expected as unknown as ArrayLike<number>;
    expect(a.length, path).toBe(e.length);
    for (let i = 0; i < e.length; i++) {
      expectClose(a[i], e[i], `${path}[${i}]`);
    }
    return;
  }
  if (Array.isArray(expected)) {
    expect(Array.isArray(actual), path).toBe(true);
    expect((actual as unknown[]).length, path).toBe(expected.length);
    expected.forEach((value, i) => {
      expectClose((actual as unknown[])[i], value, `${path}[${i}]`);
    });
    return;
  }
  if (expected !== null && typeof expected === 'object') {
    expect(typeof actual, path).toBe('object');
    const keys = Object.keys(expected);
    expect(Object.keys(actual as object).sort(), path).toEqual([...keys].sort());
    for (const key of keys) {
      expectClose(
        (actual as Record<string, unknown>)[key],
        (expected as Record<string, unknown>)[key],
        `${path}.${key}`,
      );
    }
    return;
  }
  expect(actual, path).toEqual(expected);
}

const framing = { nFft: 1024, hopLength: 256 };
const pitchOptions = {
  frameLength: 1024,
  hopLength: 256,
  f0MinHz: 80,
  f0MaxHz: 1000,
  threshold: 0.15,
};

/** Audio method, module function it delegates to, and a non-default option bag. */
const delegations: Array<[string, Record<string, unknown>]> = [
  ['stft', framing],
  ['melSpectrogram', { ...framing, nMels: 40, fminHz: 50, fmaxHz: 8000, htk: true }],
  ['mfcc', { ...framing, nMels: 40, nMfcc: 13, fminHz: 50, fmaxHz: 8000, htk: true }],
  ['chroma', framing],
  ['nnlsChroma', {}],
  ['onsetStrength', { ...framing, nMels: 40 }],
  ['spectralCentroid', framing],
  ['spectralBandwidth', framing],
  ['spectralRolloff', { ...framing, rollPercent: 0.9 }],
  ['spectralFlatness', framing],
  ['zeroCrossingRate', { frameLength: 1024, hopLength: 256 }],
  ['rmsEnergy', { frameLength: 1024, hopLength: 256 }],
  ['pitchYin', pitchOptions],
  ['pitchPyin', { ...pitchOptions, fillNa: true }],
  ['detectOnsets', {}],
  ['chordFunctionalAnalysis', { keyRoot: 0, keyMode: 'major' }],
  ['analyzeBpm', {}],
  ['analyzeRhythm', {}],
  ['analyzeDynamics', {}],
  ['analyzeTimbre', {}],
  ['analyzeImpulseResponse', { nOctaveBands: 3 }],
  ['analyze', {}],
];

const optionlessDelegations = delegations.filter(
  ([method]) => method !== 'chordFunctionalAnalysis',
);

type Callable = (...args: unknown[]) => unknown;

function method(audio: Audio, name: string): Callable {
  return (audio as unknown as Record<string, Callable>)[name].bind(audio);
}

function moduleFunction(name: string): Callable {
  return (api as unknown as Record<string, Callable>)[name];
}

describe('Audio analysis methods', () => {
  it('exposes the same analysis method set and none of the removed names', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    for (const [name] of delegations) {
      expect(typeof (audio as unknown as Record<string, unknown>)[name], name).toBe('function');
    }
    for (const removed of ['analyzeWithProgress', 'onsetEnvelope']) {
      expect(removed in audio, removed).toBe(false);
    }
  });

  it.each(delegations)(
    'delegation equivalence: audio.%s(opts) equals the module function',
    (name, opts) => {
      const samples = signal();
      const audio = Audio.fromBuffer(samples, sampleRate);
      const viaMethod = method(audio, name)(opts);
      const viaFunction = moduleFunction(name)({ samples, sampleRate, ...opts });
      expectClose(viaMethod, viaFunction);
    },
  );

  it.each(optionlessDelegations)(
    'delegation equivalence: audio.%s() with no options uses the defaults',
    (name) => {
      const samples = signal();
      const audio = Audio.fromBuffer(samples, sampleRate);
      expectClose(method(audio, name)(), moduleFunction(name)({ samples, sampleRate }));
    },
  );

  it.each(optionlessDelegations)('treats null options of audio.%s as absent', (name) => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    expectClose(method(audio, name)(null), method(audio, name)());
  });

  it.each(delegations)('refuses a positional argument to audio.%s', (name) => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    expect(() => method(audio, name)(2048, 512)).toThrow(TypeError);
  });

  it.each(delegations)(
    'refuses samples or sampleRate inside the options of audio.%s',
    (name, opts) => {
      const audio = Audio.fromBuffer(signal(), sampleRate);
      expect(() => method(audio, name)({ ...opts, samples: new Float32Array(8) })).toThrow(
        TypeError,
      );
      expect(() => method(audio, name)({ ...opts, sampleRate: 44100 })).toThrow(TypeError);
    },
  );

  it.each(delegations)('refuses an unknown option key on audio.%s', (name, opts) => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    expect(() => method(audio, name)({ ...opts, notAnOption: 1 })).toThrow(TypeError);
  });

  it('requires options on chordFunctionalAnalysis because they carry keyRoot', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    expect(() => method(audio, 'chordFunctionalAnalysis')()).toThrow(TypeError);
    // @ts-expect-error options are required
    expect(() => audio.chordFunctionalAnalysis()).toThrow(TypeError);
  });

  it('refuses the removed positional parameters by type', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    // @ts-expect-error positional nFft is gone
    expect(() => audio.stft(1024, 256)).toThrow(TypeError);
    // @ts-expect-error positional fmin is gone
    expect(() => audio.pitchYin(1024, 256, 65, 2093)).toThrow(TypeError);
    // @ts-expect-error positional nMels is gone
    expect(() => audio.melSpectrogram(1024, 256, 40)).toThrow(TypeError);
  });

  it('refuses every read of an analysis method after destroy with InvalidState', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    audio.destroy();
    for (const [name, opts] of delegations) {
      let caught: unknown;
      try {
        method(audio, name)(opts);
      } catch (error) {
        caught = error;
      }
      expect(caught, name).toBeInstanceOf(SonareError);
      expect((caught as SonareError).code, name).toBe(ErrorCode.InvalidState);
    }
  });

  it('returns Matrix2D from nnlsChroma with 12 chroma rows', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    const result = audio.nnlsChroma();
    expect(result.rows).toBe(12);
    expect(result.cols).toBeGreaterThan(0);
    expect(result.data.length).toBe(result.rows * result.cols);
    expect('nChroma' in result).toBe(false);
    expect('nFrames' in result).toBe(false);
  });

  it('returns Matrix2D from stft db with bins as rows and frames as cols', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    const result = audio.stft({ nFft: 1024, hopLength: 256, db: true }).db as Matrix2D;
    expect(result).toBeDefined();
    expect(result.rows).toBe(513);
    expect(result.cols).toBeGreaterThan(0);
    expect(result.data.length).toBe(result.rows * result.cols);
    expect('nBins' in result).toBe(false);
  });

  it('reports analysis progress through analyze options and drops analyzeWithProgress', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    const seen: Array<[number, string]> = [];
    const withProgress = audio.analyze({
      onProgress: (progress: number, stage: string) => {
        seen.push([progress, stage]);
      },
    });
    expect(seen.length).toBeGreaterThan(0);
    for (const [progress, stage] of seen) {
      expect(typeof progress).toBe('number');
      expect(typeof stage).toBe('string');
    }
    expectClose(withProgress, audio.analyze());
  });

  it('resamples to targetSampleRate', () => {
    const samples = signal();
    const audio = Audio.fromBuffer(samples, sampleRate);
    const out = audio.resample(11025);
    expect(Math.abs(out.length - samples.length / 2)).toBeLessThanOrEqual(2);
    expectClose(out, api.resample(samples, sampleRate, 11025));
  });

  it('reads a spectrum frame from a startSample offset', () => {
    const audio = Audio.fromBuffer(signal(), sampleRate);
    const first = audio.spectrumFrame(0);
    const later = audio.spectrumFrame(1024);
    expect(later.magnitude.length).toBe(first.magnitude.length);
    expect(Array.from(later.magnitude)).not.toEqual(Array.from(first.magnitude));
  });
});

describe('decodeChannels input', () => {
  it('types its argument as Uint8Array and refuses other values', () => {
    // @ts-expect-error a string is not encoded audio bytes
    expect(() => decodeChannels('not bytes')).toThrow(TypeError);
    expect(() => decodeChannels(new Uint8Array(0))).toThrow(RangeError);
  });
});
