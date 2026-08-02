import { describe, expect, it } from 'vitest';
import {
  detectKey,
  lufs,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  meteringDcOffset,
  meteringPeakDb,
  meteringRmsDb,
  meteringStereoCorrelation,
  meteringTruePeakDb,
  pitchYin,
  StreamAnalyzer,
  StreamingEqualizer,
  tempogramRatio,
  voiceChange,
  voiceChangeRealtime,
} from '../src/index';

const SR = 22050;

function withNaN(n = 1024): Float32Array {
  const buf = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    buf[i] = Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  buf[100] = Number.NaN;
  return buf;
}

function withInf(n = 1024): Float32Array {
  const buf = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    buf[i] = Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  buf[200] = Number.POSITIVE_INFINITY;
  return buf;
}

function sine(n = 1024): Float32Array {
  const buf = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    buf[i] = 0.5 * Math.sin((2 * Math.PI * 440 * i) / SR);
  }
  return buf;
}

describe('empty-sample guards (Node)', () => {
  it('lufs rejects empty', () => {
    expect(() => lufs(new Float32Array(0), SR)).toThrow(/lufs: samples must not be empty/);
  });
  it('meteringPeakDb rejects empty', () => {
    expect(() => meteringPeakDb(new Float32Array(0))).toThrow(
      /meteringPeakDb: samples must not be empty/,
    );
  });
  it('meteringRmsDb rejects empty', () => {
    expect(() => meteringRmsDb(new Float32Array(0))).toThrow(
      /meteringRmsDb: samples must not be empty/,
    );
  });
  it('meteringTruePeakDb rejects empty', () => {
    expect(() => meteringTruePeakDb(new Float32Array(0), SR)).toThrow(
      /meteringTruePeakDb: samples must not be empty/,
    );
  });
  it('meteringStereoCorrelation rejects empty left', () => {
    expect(() => meteringStereoCorrelation(new Float32Array(0), sine())).toThrow(
      /meteringStereoCorrelation: left must not be empty/,
    );
  });
  it('masteringDynamicsCompressor rejects empty', () => {
    expect(() => masteringDynamicsCompressor(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsCompressor: samples must not be empty/,
    );
  });
  it('masteringDynamicsGate rejects empty', () => {
    expect(() => masteringDynamicsGate(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsGate: samples must not be empty/,
    );
  });
  it('masteringDynamicsTransientShaper rejects empty', () => {
    expect(() => masteringDynamicsTransientShaper(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsTransientShaper: samples must not be empty/,
    );
  });
  it('voiceChange rejects empty', () => {
    expect(() => voiceChange(new Float32Array(0))).toThrow(
      /voiceChange: samples must not be empty/,
    );
  });
  it('voiceChangeRealtime rejects empty', () => {
    expect(() => voiceChangeRealtime(new Float32Array(0))).toThrow(
      /voiceChangeRealtime: samples must not be empty/,
    );
  });
});

describe('NaN/Inf guards (Node)', () => {
  it('StreamingEqualizer.match rejects a NaN reference', () => {
    const equalizer = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    expect(() => equalizer.match(sine(), withNaN(), { maxBands: 4 })).toThrow();
  });
  it('lufs rejects NaN with index', () => {
    expect(() => lufs(withNaN(), SR)).toThrow(/lufs: samples contains NaN or Inf at index 100/);
  });
  it('lufs rejects Inf with index', () => {
    expect(() => lufs(withInf(), SR)).toThrow(/lufs: samples contains NaN or Inf at index 200/);
  });
  it('masteringDynamicsCompressor rejects NaN', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR)).toThrow(
      /masteringDynamicsCompressor: samples contains NaN or Inf at index 100/,
    );
  });
  it('masteringDynamicsGate rejects Inf', () => {
    expect(() => masteringDynamicsGate(withInf(), SR)).toThrow(
      /masteringDynamicsGate: samples contains NaN or Inf at index 200/,
    );
  });
  it('meteringPeakDb rejects NaN', () => {
    expect(() => meteringPeakDb(withNaN())).toThrow(
      /meteringPeakDb: samples contains NaN or Inf at index 100/,
    );
  });
  it('pitchYin rejects NaN', () => {
    expect(() => pitchYin(withNaN(), SR)).toThrow(/samples contains NaN or Inf/);
  });
  it('meteringDcOffset rejects Inf', () => {
    expect(() => meteringDcOffset(withInf(), SR)).toThrow(/samples contains NaN or Inf/);
  });
});

describe('sample-rate guards (Node)', () => {
  it('pitchYin rejects an out-of-range sample rate', () => {
    expect(() => pitchYin(sine(), 0)).toThrow();
  });
  it('meteringDcOffset rejects an out-of-range sample rate', () => {
    expect(() => meteringDcOffset(sine(), 0)).toThrow();
  });
});

describe('addon input errors remain catchable (Node)', () => {
  it('converts option-parser errors into JavaScript exceptions', () => {
    expect(() => detectKey(sine(), SR, { profile: 'bogus' })).toThrow(/invalid key profile/);
    expect(() => detectKey(sine(), SR, { modes: ['major', 'nope'] })).toThrow(/invalid key mode/);
    expect(() => tempogramRatio(sine(), 384, SR, 512, 'nope' as unknown as Float32Array)).toThrow(
      /Expected Float32Array or number\[\]/,
    );
    expect(() => new StreamAnalyzer({ maxPendingFrames: 1.5 })).toThrow(
      /maxPendingFrames must be a non-negative integer/,
    );
  });
});

describe('validate=false still has the C-ABI NaN backstop (Node)', () => {
  it('lufs with validate=false rejects NaN in the C-ABI', () => {
    // `validate: false` bypasses only the TypeScript preflight. The C-ABI
    // repeats this safety check and must continue to reject non-finite audio.
    expect(() => lufs(withNaN(), SR, { validate: false })).toThrow();
  });
  it('masteringDynamicsCompressor with validate=false rejects NaN in the C-ABI', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR, { validate: false })).toThrow();
  });
});

describe('positive smoke (Node)', () => {
  it('meteringRmsDb returns finite number for short sine', () => {
    const v = meteringRmsDb(sine(1024));
    expect(Number.isFinite(v)).toBe(true);
  });
  it('masteringDynamicsCompressor returns Float32Array with same length', () => {
    const input = sine(SR);
    const r = masteringDynamicsCompressor(input, SR);
    expect(r.samples.length).toBe(input.length);
  });
});
