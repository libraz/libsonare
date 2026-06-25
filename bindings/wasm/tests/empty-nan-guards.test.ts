import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  detectBpm,
  init,
  lufs,
  lufsInterleaved,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  meteringDcOffset,
  meteringPeakDb,
  meteringRmsDb,
  meteringStereoCorrelation,
  meteringTruePeakDb,
  pitchYin,
  voiceChange,
  voiceChangeRealtime,
} from '../src/index';

const SR = 22050;

beforeAll(async () => {
  await init();
});

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

describe('empty-sample guards (WASM)', () => {
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
  it('voiceChangeRealtime rejects invalid stereo length', () => {
    expect(() => voiceChangeRealtime(new Float32Array(3), { channels: 2 })).toThrow(
      /multiple of 2/,
    );
  });
});

describe('NaN/Inf guards (WASM)', () => {
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
  it('detectBpm rejects NaN before native analysis', () => {
    expect(() => detectBpm(withNaN(), SR)).toThrow(
      /detectBpm: samples contains NaN or Inf at index 100/,
    );
  });
  it('analyzeBpm rejects negative maxCandidates before native analysis', () => {
    expect(() => analyzeBpm(sine(SR), SR, { maxCandidates: -1 })).toThrow(
      /analyzeBpm: maxCandidates must be a non-negative integer/,
    );
  });
  it('lufsInterleaved rejects buffers whose length is not a multiple of channels', () => {
    expect(() => lufsInterleaved(new Float32Array([0.1, 0.2, 0.3]), 2, SR)).toThrow(
      /lufsInterleaved: samples length must be a multiple of channels/,
    );
  });
});

describe('validate=false skips the JS index scan but the native core still rejects (WASM)', () => {
  // { validate: false } only skips the JS-side O(n) scan that names the exact
  // index. The native core always re-validates (matching the C ABI / Node /
  // Python), so a non-finite buffer still throws — just without the indexed JS
  // message. This closes the former footgun where validate:false pushed NaN
  // straight into the DSP.
  it('lufs with validate=false no longer reports the JS index, but still throws natively', () => {
    expect(() => lufs(withNaN(), SR, { validate: false })).not.toThrow(
      /samples contains NaN or Inf at index/,
    );
    expect(() => lufs(withNaN(), SR, { validate: false })).toThrow();
  });
  it('masteringDynamicsCompressor with validate=false still throws natively on NaN', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR, { validate: false })).not.toThrow(
      /samples contains NaN or Inf at index/,
    );
    expect(() => masteringDynamicsCompressor(withNaN(), SR, { validate: false })).toThrow();
  });
});

describe('native backstop closes gaps the JS guards missed (WASM)', () => {
  // These wrappers previously had no JS guard (pitchYin) or guarded the samples
  // but not the sample rate (metering*), so out-of-range / non-finite input
  // diverged from the other surfaces. The shared native validation now rejects
  // them uniformly.
  it('pitchYin rejects an empty buffer', () => {
    expect(() => pitchYin(new Float32Array(0), SR)).toThrow();
  });
  it('pitchYin rejects NaN', () => {
    expect(() => pitchYin(withNaN(), SR)).toThrow();
  });
  it('pitchYin rejects an out-of-range sample rate', () => {
    expect(() => pitchYin(sine(), 100)).toThrow();
  });
  it('meteringDcOffset rejects an out-of-range sample rate', () => {
    expect(() => meteringDcOffset(sine(), 100)).toThrow();
  });
});

describe('positive smoke (WASM)', () => {
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
