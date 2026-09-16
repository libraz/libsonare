import { describe, expect, it } from 'vitest';
import {
  chroma,
  melSpectrogram,
  mfcc,
  pitchPyin,
  pitchYin,
  rmsEnergy,
  spectralBandwidth,
  spectralCentroid,
  spectralFlatness,
  spectralRolloff,
  stft,
  stftDb,
  zeroCrossingRate,
} from '../src/index.js';

const SR = 22050;

function sine(n = 2048): Float32Array {
  const buf = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    buf[i] = Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  return buf;
}

function withNaN(n = 2048): Float32Array {
  const buf = sine(n);
  buf[100] = Number.NaN;
  return buf;
}

// Every feature extractor funnels a raw Float32Array straight into the core.
// Before the guard was added these Node functions skipped the finite/empty/
// sample-rate validation the C ABI performs and returned an all-NaN result
// instead of a clear error. Each must now throw, matching Python and the CLI.
const featureCalls: Array<[string, (samples: Float32Array, sr: number) => unknown]> = [
  ['stft', (s, sr) => stft(s, sr)],
  ['stftDb', (s, sr) => stftDb(s, sr)],
  ['melSpectrogram', (s, sr) => melSpectrogram(s, sr)],
  ['mfcc', (s, sr) => mfcc(s, sr)],
  ['chroma', (s, sr) => chroma(s, sr)],
  ['spectralCentroid', (s, sr) => spectralCentroid(s, sr)],
  ['spectralBandwidth', (s, sr) => spectralBandwidth(s, sr)],
  ['spectralRolloff', (s, sr) => spectralRolloff(s, sr)],
  ['spectralFlatness', (s, sr) => spectralFlatness(s, sr)],
  ['zeroCrossingRate', (s, sr) => zeroCrossingRate(s, sr)],
  ['rmsEnergy', (s, sr) => rmsEnergy(s, sr)],
  ['pitchYin', (s, sr) => pitchYin(s, sr)],
  ['pitchPyin', (s, sr) => pitchPyin(s, sr)],
];

describe('feature extractors validate offline audio input', () => {
  for (const [name, call] of featureCalls) {
    it(`${name} rejects NaN samples`, () => {
      expect(() => call(withNaN(), SR)).toThrow();
    });

    it(`${name} rejects an empty buffer`, () => {
      expect(() => call(new Float32Array(0), SR)).toThrow();
    });

    it(`${name} rejects an out-of-range sample rate`, () => {
      expect(() => call(sine(), 0)).toThrow();
    });

    it(`${name} still succeeds on a valid signal`, () => {
      expect(() => call(sine(), SR)).not.toThrow();
    });
  }
});

/**
 * The core refuses an odd `nFft`, a non-positive `hopLength` and a sample rate
 * outside [8000, 384000] on its own, so the only value that can reach it wrongly
 * is a fraction the addon's narrowing lands on a LEGAL one: `2048.9` arrives as
 * `2048` and answers as the transform nobody asked for. That is the whole hole,
 * and it is why the cases below drive fractions rather than out-of-range values.
 *
 * The length control comes first at each entry point. Refusals alone would pass
 * on a facade that ignored the argument entirely and threw for some other reason.
 */
const N = 4096;
const frameCount = (frames: { length: number } | { nFrames: number }): number =>
  'nFrames' in frames ? frames.nFrames : frames.length;

type FftCall = (opts: { nFft?: number; hopLength?: number; sampleRate?: number }) => unknown;

const fftCalls: Array<[string, FftCall]> = [
  ['stft', (o) => stft({ samples: sine(N), sampleRate: SR, ...o })],
  ['stftDb', (o) => stftDb({ samples: sine(N), sampleRate: SR, ...o })],
  ['melSpectrogram', (o) => melSpectrogram({ samples: sine(N), sampleRate: SR, ...o })],
  ['mfcc', (o) => mfcc({ samples: sine(N), sampleRate: SR, ...o })],
  ['spectralCentroid', (o) => spectralCentroid({ samples: sine(N), sampleRate: SR, ...o })],
  ['spectralBandwidth', (o) => spectralBandwidth({ samples: sine(N), sampleRate: SR, ...o })],
  ['spectralRolloff', (o) => spectralRolloff({ samples: sine(N), sampleRate: SR, ...o })],
  ['spectralFlatness', (o) => spectralFlatness({ samples: sine(N), sampleRate: SR, ...o })],
];

describe('an STFT-backed entry point refuses a fraction its narrowing would fold', () => {
  for (const [name, call] of fftCalls) {
    it(`${name} reads the hop it is given`, () => {
      const wide = call({ nFft: 512, hopLength: 256 }) as { length: number } | { nFrames: number };
      const narrow = call({ nFft: 512, hopLength: 128 }) as
        | { length: number }
        | { nFrames: number };
      expect(frameCount(narrow)).toBeGreaterThan(frameCount(wide));
    });

    it(`${name} refuses a fractional nFft that truncates onto a legal size`, () => {
      expect(() => call({ nFft: 2048.9 })).toThrow(/nFft must be an integer/);
    });

    it(`${name} refuses a fractional hopLength`, () => {
      expect(() => call({ hopLength: 512.7 })).toThrow(/hopLength must be an integer/);
    });

    it(`${name} refuses a fractional sample rate without calling it out of range`, () => {
      expect(() => call({ sampleRate: 22050.7 })).toThrow(/sampleRate must be an integer/);
    });
  }
});

describe('a framing entry point refuses a fractional window', () => {
  const framingCalls: Array<
    [string, (o: { frameLength?: number; hopLength?: number; sampleRate?: number }) => Float32Array]
  > = [
    ['zeroCrossingRate', (o) => zeroCrossingRate({ samples: sine(N), sampleRate: SR, ...o })],
    ['rmsEnergy', (o) => rmsEnergy({ samples: sine(N), sampleRate: SR, ...o })],
  ];

  for (const [name, call] of framingCalls) {
    it(`${name} reads the hop it is given`, () => {
      expect(call({ hopLength: 128 }).length).toBeGreaterThan(call({ hopLength: 256 }).length);
    });

    it(`${name} accepts an odd frame length, which is not a transform size`, () => {
      expect(() => call({ frameLength: 2047 })).not.toThrow();
    });

    it(`${name} refuses a fractional frame length`, () => {
      expect(() => call({ frameLength: 2048.5 })).toThrow(/frameLength must be an integer/);
    });

    it(`${name} refuses a fractional sample rate`, () => {
      expect(() => call({ sampleRate: 22050.7 })).toThrow(/sampleRate must be an integer/);
    });
  }
});
