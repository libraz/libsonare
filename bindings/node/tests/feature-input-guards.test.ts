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
