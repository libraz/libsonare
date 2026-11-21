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
  meteringTruePeakDbtp,
  pitchPyin,
  pitchYin,
  StreamAnalyzer,
  StreamingEqualizer,
  tempogramRatio,
  voiceChange,
  voiceChangeRealtime,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index.js';
import { unknownKeyText } from './_helpers.js';

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
    expect(() => meteringPeakDb(new Float32Array(0), SR)).toThrow(
      /meteringPeakDb: samples must not be empty/,
    );
  });
  it('meteringRmsDb rejects empty', () => {
    expect(() => meteringRmsDb(new Float32Array(0), SR)).toThrow(
      /meteringRmsDb: samples must not be empty/,
    );
  });
  it('meteringTruePeakDbtp rejects empty', () => {
    expect(() => meteringTruePeakDbtp(new Float32Array(0), SR)).toThrow(
      /meteringTruePeakDbtp: samples must not be empty/,
    );
  });
  it('meteringStereoCorrelation rejects empty left', () => {
    expect(() =>
      meteringStereoCorrelation({ left: new Float32Array(0), right: sine(), sampleRate: SR }),
    ).toThrow(/meteringStereoCorrelation: left must not be empty/);
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
    expect(() => voiceChange(new Float32Array(0), SR)).toThrow(
      /voiceChange: samples must not be empty/,
    );
  });
  it('voiceChangeRealtime rejects empty', () => {
    expect(() => voiceChangeRealtime({ channels: [new Float32Array(0)], sampleRate: SR })).toThrow(
      /voiceChangeRealtime: channels\[0\] must not be empty/,
    );
  });
});

describe('NaN/Inf guards (Node)', () => {
  it('StreamingEqualizer.match rejects a NaN reference', () => {
    const equalizer = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    expect(() => equalizer.match(sine(), withNaN(), { maxBands: 4 })).toThrow();
  });
  it('lufs rejects NaN with index', () => {
    expect(() => lufs(withNaN(), SR)).toThrow(
      /lufs: samples contains a non-finite sample at index 100/,
    );
  });
  it('lufs rejects Inf with index', () => {
    expect(() => lufs(withInf(), SR)).toThrow(
      /lufs: samples contains a non-finite sample at index 200/,
    );
  });
  it('masteringDynamicsCompressor rejects NaN', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR)).toThrow(
      /masteringDynamicsCompressor: samples contains a non-finite sample at index 100/,
    );
  });
  it('masteringDynamicsGate rejects Inf', () => {
    expect(() => masteringDynamicsGate(withInf(), SR)).toThrow(
      /masteringDynamicsGate: samples contains a non-finite sample at index 200/,
    );
  });
  it('meteringPeakDb rejects NaN', () => {
    expect(() => meteringPeakDb(withNaN(), SR)).toThrow(
      /meteringPeakDb: samples contains a non-finite sample at index 100/,
    );
  });
  it('pitchYin rejects NaN', () => {
    expect(() => pitchYin(withNaN(), SR)).toThrow(/samples contains a non-finite sample/);
  });
  // pitchPyin is pitchYin's twin and must reject identically — asserting only
  // the pitchYin side is how the two drifted apart in the first place.
  it.each([
    ['pitchYin', pitchYin],
    ['pitchPyin', pitchPyin],
  ])('%s rejects NaN with the same error shape', (name, fn) => {
    let caught: unknown;
    try {
      fn(withNaN(), SR);
    } catch (error) {
      caught = error;
    }
    expect(caught, `${name} should have thrown`).toBeInstanceOf(RangeError);
    expect((caught as Error).message).toMatch(
      new RegExp(`^${name}: samples contains a non-finite sample at index 100$`),
    );
  });
  it('meteringDcOffset rejects Inf', () => {
    expect(() => meteringDcOffset(withInf(), SR)).toThrow(/samples contains a non-finite sample/);
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
    // @ts-expect-error deliberately invalid key profile; the option parser must reject it.
    expect(() => detectKey(sine(), SR, { profile: 'bogus' })).toThrow(/profile must be one of/);
    // @ts-expect-error deliberately invalid key mode; the option parser must reject it.
    expect(() => detectKey(sine(), SR, { modes: ['major', 'nope'] })).toThrow(
      /modes\[1\] must be one of/,
    );
    expect(() =>
      tempogramRatio({
        tempogram: { data: sine(768), rows: 384, cols: 2 },
        sampleRate: SR,
        hopLength: 512,
        factors: 'nope' as unknown as Float32Array,
      }),
    ).toThrow(/^tempogramRatio: factors must be a Float32Array or number\[\]$/);
    expect(() => new StreamAnalyzer({ sampleRate: SR, maxPendingFrames: 1.5 })).toThrow(
      /^StreamAnalyzer: maxPendingFrames must be an integer in \[1, 1048576\]$/,
    );
  });
});

describe('non-finite audio is always refused (Node)', () => {
  it('lufs refuses NaN and a validate option', () => {
    expect(() => lufs(withNaN(), SR)).toThrow(RangeError);
    expect(() => lufs(withNaN(), SR, { validate: false } as never)).toThrow(
      unknownKeyText('lufs: unknown key validate'),
    );
  });
  it('masteringDynamicsCompressor refuses NaN and a validate option', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR)).toThrow(RangeError);
    expect(() => masteringDynamicsCompressor(withNaN(), SR, { validate: false } as never)).toThrow(
      /unknown key validate \(expected /,
    );
  });
  it('waveformPeaks refuses NaN rather than drawing it as silence', () => {
    // A silent buffer answers 0/0 and a non-finite one is refused, so the two
    // cannot arrive in the same shape.
    const quiet = waveformPeaks({ channels: [new Float32Array(1024)], samplesPerBucket: 256 });
    expect(quiet.min[0]?.[0]).toBe(0);
    expect(quiet.max[0]?.[0]).toBe(0);
    expect(() => waveformPeaks({ channels: [withNaN()], samplesPerBucket: 256 })).toThrow(
      RangeError,
    );
    expect(() =>
      waveformPeaks({ channels: [withNaN()], samplesPerBucket: 256, validate: false } as never),
    ).toThrow(/unknown key validate \(expected /);
  });
  it('waveformPeakPyramid refuses NaN and a validate option', () => {
    const request = { channels: [withNaN()], samplesPerBucketLevels: [256, 512] };
    expect(() => waveformPeakPyramid(request)).toThrow(RangeError);
    expect(() => waveformPeakPyramid({ ...request, validate: false } as never)).toThrow(
      /unknown key validate \(expected /,
    );
  });
});

describe('positive smoke (Node)', () => {
  it('meteringRmsDb returns finite number for short sine', () => {
    const v = meteringRmsDb(sine(1024), SR);
    expect(Number.isFinite(v)).toBe(true);
  });
  it('masteringDynamicsCompressor returns Float32Array with same length', () => {
    const input = sine(SR);
    const r = masteringDynamicsCompressor(input, SR);
    expect(r.samples.length).toBe(input.length);
  });
});
