import { beforeAll, describe, expect, it } from 'vitest';
import {
  analyzeBpm,
  analyzeDynamics,
  analyzeRhythm,
  analyzeTimbre,
  cyclicTempogram,
  detectBpm,
  init,
  lufs,
  lufsMultichannel,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  meteringDcOffset,
  meteringPeakDb,
  meteringRmsDb,
  meteringStereoCorrelation,
  meteringTruePeakDbtp,
  pcen,
  pitchYin,
  plp,
  realtimeVoiceChangerRender,
  resample,
  StreamingEqualizer,
  tempogram,
  tempogramRatio,
  voiceChange,
  waveformPeakPyramid,
  waveformPeaks,
} from '../src/index';
import { getSonareModule } from '../src/module_state';
import { expectRefusalOf, unknownKeyText } from './_helpers';

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

function m() {
  return getSonareModule();
}

describe('empty-sample guards (WASM)', () => {
  it('lufs rejects empty', () => {
    expect(() => lufs(new Float32Array(0), SR)).toThrow(/lufs: samples must be a non-empty buffer/);
  });
  it('meteringPeakDb rejects empty', () => {
    expect(() => meteringPeakDb(new Float32Array(0), SR)).toThrow(
      /meteringPeakDb: samples must be a non-empty buffer/,
    );
  });
  it('meteringRmsDb rejects empty', () => {
    expect(() => meteringRmsDb(new Float32Array(0), SR)).toThrow(
      /meteringRmsDb: samples must be a non-empty buffer/,
    );
  });
  it('meteringTruePeakDbtp rejects empty', () => {
    expect(() => meteringTruePeakDbtp(new Float32Array(0), SR)).toThrow(
      /meteringTruePeakDbtp: samples must be a non-empty buffer/,
    );
  });
  it('meteringStereoCorrelation rejects empty left', () => {
    expect(() =>
      meteringStereoCorrelation({ left: new Float32Array(0), right: sine(), sampleRate: SR }),
    ).toThrow(/meteringStereoCorrelation: left must be a non-empty buffer/);
  });
  it('masteringDynamicsCompressor rejects empty', () => {
    expect(() => masteringDynamicsCompressor(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsCompressor: samples must be a non-empty buffer/,
    );
  });
  it('masteringDynamicsGate rejects empty', () => {
    expect(() => masteringDynamicsGate(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsGate: samples must be a non-empty buffer/,
    );
  });
  it('masteringDynamicsTransientShaper rejects empty', () => {
    expect(() => masteringDynamicsTransientShaper(new Float32Array(0), SR)).toThrow(
      /masteringDynamicsTransientShaper: samples must be a non-empty buffer/,
    );
  });
  it('voiceChange rejects empty', () => {
    expect(() => voiceChange(new Float32Array(0), SR)).toThrow(
      /voiceChange: samples must be a non-empty buffer/,
    );
  });
  it('realtimeVoiceChangerRender rejects unequal channel lengths', () => {
    expect(() =>
      realtimeVoiceChangerRender({
        channels: [new Float32Array(3), new Float32Array(2)],
        sampleRate: 48000,
        config: 'neutralMonitor',
      }),
    ).toThrow(/equal lengths/);
  });
});

describe('NaN/Inf guards (WASM)', () => {
  it('resample rejects invalid source/target sample rates before allocation', () => {
    expect(expectRefusalOf(() => resample(sine(), 0, SR), RangeError).message).toBe(
      'resample: src_sr must be within [8000, 384000]',
    );
    expect(expectRefusalOf(() => resample(sine(), SR, 2_000_000_000), RangeError).message).toBe(
      'resample: target_sr must be within [8000, 384000]',
    );
  });
  it('feature-core wrappers reject NaN and inconsistent matrix dimensions', () => {
    expect(
      expectRefusalOf(
        () => pcen({ values: { data: new Float32Array([1, Number.NaN, 3, 4]), rows: 2, cols: 2 } }),
        RangeError,
      ).message,
    ).toBe('pcen: values.data contains a non-finite sample at index 1');
    expect(
      expectRefusalOf(
        () => pcen({ values: { data: new Float32Array([1, 2, 3]), rows: 2, cols: 2 } }),
        RangeError,
      ).message,
    ).toBe('pcen: values.data length must equal rows * cols');
    expect(
      expectRefusalOf(() => tempogram({ onsetEnvelope: withNaN(), sampleRate: SR }), RangeError)
        .message,
    ).toBe('tempogram: onsetEnvelope contains a non-finite value at index 100');
    expect(
      expectRefusalOf(() => plp({ onsetEnvelope: withNaN(), sampleRate: SR }), RangeError).message,
    ).toBe('plp: onsetEnvelope contains a non-finite value at index 100');
    // The cyclic and ratio tempogram wrappers previously skipped finiteness
    // validation and silently returned NaN-filled arrays (unlike the C ABI).
    expect(() => cyclicTempogram({ onsetEnvelope: withNaN(), sampleRate: SR })).toThrow(
      /^cyclicTempogram: onsetEnvelope contains a non-finite value at index \d+$/,
    );
    expect(() =>
      tempogramRatio({ tempogram: { data: withNaN(), rows: 64, cols: 16 }, sampleRate: SR }),
    ).toThrow(/non-finite/);
  });
  it('StreamingEqualizer.match rejects non-finite reference audio', () => {
    const equalizer = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    expect(
      expectRefusalOf(() => equalizer.match(sine(), withNaN(), { maxBands: 4 }), RangeError)
        .message,
    ).toBe('StreamingEqualizer.match: reference contains a non-finite sample at index 100');
    equalizer.destroy();
  });
  it('StreamingEqualizer.match refuses a silent source or reference by name', () => {
    const equalizer = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    const silent = new Float32Array(sine().length);
    expect(
      expectRefusalOf(() => equalizer.match(silent, sine(), { maxBands: 4 }), RangeError).message,
    ).toBe('StreamingEqualizer.match: source has no measurable level');
    expect(
      expectRefusalOf(() => equalizer.match(sine(), silent, { maxBands: 4 }), RangeError).message,
    ).toBe('StreamingEqualizer.match: reference has no measurable level');
    equalizer.destroy();
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
  it('detectBpm rejects NaN before native analysis', () => {
    expect(() => detectBpm(withNaN(), SR)).toThrow(
      /detectBpm: samples contains a non-finite sample at index 100/,
    );
  });
  it('analyzeBpm rejects negative maxCandidates before native analysis', () => {
    expect(() => analyzeBpm(sine(SR), SR, { maxCandidates: -1 })).toThrow(
      /analyzeBpm: maxCandidates must be an integer in \[0, 9007199254740991\]$/,
    );
  });
  it('analyzeBpm refuses a maxCandidates past the safe-integer range', () => {
    expect(() => analyzeBpm(sine(SR), SR, { maxCandidates: 2 ** 60 })).toThrow(
      /analyzeBpm: maxCandidates must be an integer in \[0, 9007199254740991\]$/,
    );
  });
  it('lufsMultichannel rejects planes of unequal length', () => {
    expect(() =>
      lufsMultichannel({ channels: [new Float32Array(3), new Float32Array(2)], sampleRate: SR }),
    ).toThrow(/lufsMultichannel: channels must all have the same length/);
  });
});

describe('non-finite samples are always refused and validate is not an option (WASM)', () => {
  it('lufs refuses NaN with the indexed message and refuses a validate option', () => {
    expect(() => lufs(withNaN(), SR)).toThrow(/samples contains a non-finite sample at index/);
    expect(() => lufs(withNaN(), SR, { validate: false } as never)).toThrow(
      unknownKeyText('lufs: unknown key validate'),
    );
  });
  it('masteringDynamicsCompressor refuses NaN and a validate option', () => {
    expect(() => masteringDynamicsCompressor(withNaN(), SR)).toThrow(RangeError);
    expect(() => masteringDynamicsCompressor(withNaN(), SR, { validate: false } as never)).toThrow(
      unknownKeyText('masteringDynamicsCompressor: unknown key validate'),
    );
  });
  it('waveformPeaks rejects NaN rather than drawing it as silence', () => {
    const quiet = waveformPeaks({
      channels: [new Float32Array(1024)],
      samplesPerBucket: 256,
    });
    expect(quiet.min[0]?.[0]).toBe(0);
    expect(quiet.max[0]?.[0]).toBe(0);
    expect(() => waveformPeaks({ channels: [withNaN()], samplesPerBucket: 256 })).toThrow(
      RangeError,
    );
    expect(() =>
      waveformPeaks({ channels: [withNaN()], samplesPerBucket: 256, validate: false } as never),
    ).toThrow(unknownKeyText('waveformPeaks: unknown key validate'));
  });
  it('waveformPeakPyramid rejects NaN and a validate key', () => {
    expect(() =>
      waveformPeakPyramid({ channels: [withNaN()], samplesPerBucketLevels: [256, 512] }),
    ).toThrow(RangeError);
    expect(() =>
      waveformPeakPyramid({
        channels: [withNaN()],
        samplesPerBucketLevels: [256, 512],
        validate: false,
      } as never),
    ).toThrow(unknownKeyText('waveformPeakPyramid: unknown key validate'));
  });
});

describe('native backstop closes gaps the JS guards missed (WASM)', () => {
  // These wrappers previously had no JS guard (pitchYin) or guarded the samples
  // but not the sample rate (metering*), so out-of-range / non-finite input
  // diverged from the other surfaces. The shared native validation now rejects
  // them uniformly.
  it('pitchYin rejects an empty buffer', () => {
    expect(expectRefusalOf(() => pitchYin(new Float32Array(0), SR), RangeError).message).toBe(
      'pitchYin: samples must be a non-empty buffer',
    );
  });
  it('pitchYin rejects NaN', () => {
    expect(expectRefusalOf(() => pitchYin(withNaN(), SR), RangeError).message).toBe(
      'pitchYin: samples contains a non-finite sample at index 100',
    );
  });
  it('pitchYin rejects an out-of-range sample rate', () => {
    expect(expectRefusalOf(() => pitchYin(sine(), 100), RangeError).message).toBe(
      'pitchYin: sampleRate must be an integer in [8000, 384000]',
    );
  });
  it('meteringDcOffset rejects an out-of-range sample rate', () => {
    expect(expectRefusalOf(() => meteringDcOffset(sine(), 100), RangeError).message).toBe(
      'meteringDcOffset: sampleRate must be an integer in [8000, 384000]',
    );
  });
});

describe('detailed-analysis config geometry matches the C ABI (WASM)', () => {
  // The detailed analyzers call the C++ analyzer classes directly (no C ABI
  // round-trip), so the WASM bindings must enforce the same config contract the
  // flat C ABI does instead of letting the analyzer silently clamp.
  const audio = sine(SR);
  it('analyzeBpm rejects an inverted BPM range', () => {
    expect(
      expectRefusalOf(() => analyzeBpm(audio, SR, { bpmMin: 200, bpmMax: 100 }), RangeError)
        .message,
    ).toBe('analyzeBpm: bpmMax must be finite and greater than bpmMin');
  });
  it('analyzeBpm rejects a non-positive bpmMin', () => {
    expect(expectRefusalOf(() => analyzeBpm(audio, SR, { bpmMin: 0 }), RangeError).message).toBe(
      'analyzeBpm: bpmMin must be finite and greater than 0',
    );
  });
  it('analyzeRhythm rejects an inverted BPM range', () => {
    expect(
      expectRefusalOf(() => analyzeRhythm(audio, SR, { bpmMin: 200, bpmMax: 100 }), RangeError)
        .message,
    ).toBe('analyzeRhythm: bpmMax must be finite and greater than bpmMin');
  });
  it('analyzeDynamics rejects a non-positive window', () => {
    expect(
      expectRefusalOf(() => analyzeDynamics(audio, SR, { windowSec: 0 }), RangeError).message,
    ).toBe('analyzeDynamics: windowSec must be finite and greater than 0');
  });
  it('analyzeDynamics rejects a negative compression threshold', () => {
    expect(
      expectRefusalOf(() => analyzeDynamics(audio, SR, { compressionThresholdDb: -1 }), RangeError)
        .message,
    ).toBe('analyzeDynamics: compressionThresholdDb must be finite and at least 0');
  });
  it('analyzeTimbre rejects non-positive nMels/nMfcc', () => {
    expect(expectRefusalOf(() => analyzeTimbre(audio, SR, { nMels: 0 }), RangeError).message).toBe(
      'analyzeTimbre: nMels must be an integer in [1, 2147483647]',
    );
    expect(expectRefusalOf(() => analyzeTimbre(audio, SR, { nMfcc: 0 }), RangeError).message).toBe(
      'analyzeTimbre: nMfcc must be an integer in [1, 2147483647]',
    );
  });
});

describe('plain embind float configs refuse a non-finite value (WASM)', () => {
  // Driven off the module rather than the TS facades, which run their own
  // assertFiniteScalar and would answer every case here before the WASM
  // boundary under test. Each field below is a plain embind `float`: embind's
  // own glue converts it, so it never passes through checkedFloatFromVal and
  // the native guard is the only thing in front of the config. The `val`
  // fields of these entry points are covered by quick-detailed-float-guards
  // and features-float-guards, which assert checkedFloatFromVal's refusal.
  const tone = sine(SR);
  const nonFinite = [Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY];
  // A finite number wider than FLT_MAX: embind's float glue saturates it onto
  // an infinity, so it reaches the guard as one without ever looking non-finite
  // to the caller.
  const SATURATES_ONTO_AN_INFINITY = 3.5e38;
  // Short enough for the CQT/VQT controls below to run a real transform cheaply.
  const cqtTone = sine(4096);
  const CQT_FMIN = 130.8128;

  interface NonFiniteField {
    entry: string;
    key: string;
    /** Refusal for a NaN or infinite value. */
    nonFinite: string;
    /** Refusal for a finite value that saturates onto an infinity. */
    saturated: string;
    run: (value: number) => unknown;
  }

  const fields: NonFiniteField[] = [
    {
      entry: 'analyzeMelody',
      key: 'fmin',
      nonFinite: 'analyzeMelody: f0MinHz must be finite and greater than 0',
      saturated: 'analyzeMelody: f0MinHz must be finite and in [-3.4028235e38, 3.4028235e38]',
      run: (v) => m().analyzeMelody(tone, SR, v, 2093, 2048, 256, 0.1, false, true),
    },
    {
      entry: 'cqt',
      key: 'fmin',
      nonFinite: 'cqt: fminHz must be finite and in [-3.4028235e38, 3.4028235e38]',
      saturated: 'cqt: fminHz must be finite and in [-3.4028235e38, 3.4028235e38]',
      run: (v) => m().cqt(cqtTone, SR, 512, v, 12, 12, undefined),
    },
    {
      entry: 'vqt',
      key: 'fmin',
      nonFinite: 'vqt: fminHz must be finite and in [-3.4028235e38, 3.4028235e38]',
      saturated: 'vqt: fminHz must be finite and in [-3.4028235e38, 3.4028235e38]',
      run: (v) => m().vqt(cqtTone, SR, 512, v, 12, 12, undefined, 0),
    },
  ];

  const cases = fields.flatMap((field) => nonFinite.map((value) => ({ ...field, value })));

  it.each(cases)('$entry refuses $key = $value', ({ run, value, nonFinite }) => {
    expect(expectRefusalOf(() => run(value), RangeError).message).toBe(nonFinite);
  });

  it.each(fields)('$entry refuses a $key that saturates onto an infinity', ({ run, saturated }) => {
    expect(expectRefusalOf(() => run(SATURATES_ONTO_AN_INFINITY), RangeError).message).toBe(
      saturated,
    );
  });

  it('vqt refuses an infinite gamma', () => {
    expect(
      expectRefusalOf(
        () => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined, Number.POSITIVE_INFINITY),
        RangeError,
      ).message,
    ).toBe('vqt: gamma must be finite and at least 0');
    expect(
      expectRefusalOf(
        () => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined, Number.NEGATIVE_INFINITY),
        RangeError,
      ).message,
    ).toBe('vqt: gamma must be finite and at least 0');
    expect(
      expectRefusalOf(
        () => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined, SATURATES_ONTO_AN_INFINITY),
        RangeError,
      ).message,
    ).toBe('vqt: gamma must be finite and in [-3.4028235e38, 3.4028235e38]');
  });

  it('vqt reads an omitted gamma as the automatic bandwidth and refuses a written NaN', () => {
    // Absent is the C ABI's NaN, so omitting gamma must agree exactly with giving the
    // bandwidth the transform resolved, while gamma = 0 (the CQT-equivalent one) must not.
    const vqt = (gamma?: number) => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined, gamma);
    const omitted = vqt(undefined);
    expect(omitted.gamma).toBeGreaterThan(0);
    expect(Array.from(vqt(omitted.gamma).magnitude)).toEqual(Array.from(omitted.magnitude));
    expect(Array.from(vqt(0).magnitude)).not.toEqual(Array.from(omitted.magnitude));
    expect(() => vqt(Number.NaN)).toThrow('vqt: gamma must be finite and at least 0');
    expect(() => vqt(-1)).toThrow('vqt: gamma must be finite and at least 0');
  });

  it('refuses an inverted but finite ordering pair', () => {
    expect(
      expectRefusalOf(() => m().analyzeBpm(tone, SR, 200, 100, 120, 2048, 512, 5), RangeError)
        .message,
    ).toBe('analyzeBpm: bpmMax must be finite and greater than bpmMin');
    expect(
      expectRefusalOf(() => m().analyzeRhythm(tone, SR, 200, 100, 120, 2048, 512), RangeError)
        .message,
    ).toBe('analyzeRhythm: bpmMax must be finite and greater than bpmMin');
    expect(
      expectRefusalOf(
        () => m().analyzeMelody(tone, SR, 400, 200, 2048, 256, 0.1, false, true),
        RangeError,
      ).message,
    ).toBe('analyzeMelody: f0MaxHz must be finite and greater than f0MinHz');
  });

  // Controls: the same entry points accept a finite configuration and return a
  // value, so a refusal above cannot be an entry point rejecting everything.
  it('analyzeBpm accepts a finite configuration', () => {
    expect(Number.isFinite(m().analyzeBpm(tone, SR, 30, 300, 120, 2048, 512, 5).bpm)).toBe(true);
  });
  it('analyzeRhythm accepts a finite configuration', () => {
    expect(Number.isFinite(m().analyzeRhythm(tone, SR, 60, 200, 120, 2048, 512).bpm)).toBe(true);
  });
  it('analyzeMelody accepts a finite configuration', () => {
    const melody = m().analyzeMelody(tone, SR, 65, 2093, 2048, 256, 0.1, false, true);
    expect(Number.isFinite(melody.pitchStability)).toBe(true);
  });
  it('cqt accepts a finite fmin', () => {
    const result = m().cqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined);
    expect(result.nBins).toBe(12);
    expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
  });
  it('vqt accepts a finite fmin and gamma', () => {
    const result = m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, undefined, 0);
    expect(result.nBins).toBe(12);
    expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
  });
});

describe('positive smoke (WASM)', () => {
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
