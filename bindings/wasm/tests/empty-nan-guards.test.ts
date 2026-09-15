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
  lufsInterleaved,
  masteringDynamicsCompressor,
  masteringDynamicsGate,
  masteringDynamicsTransientShaper,
  meteringDcOffset,
  meteringPeakDb,
  meteringRmsDb,
  meteringStereoCorrelation,
  meteringTruePeakDb,
  pcen,
  pitchYin,
  plp,
  resample,
  StreamingEqualizer,
  tempogram,
  tempogramRatio,
  voiceChange,
  voiceChangeRealtime,
} from '../src/index';
import { getSonareModule } from '../src/module_state';

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
    expect(() =>
      voiceChangeRealtime(new Float32Array(3), 48000, 'neutral-monitor', { channels: 2 }),
    ).toThrow(/multiple of 2/);
  });
});

describe('NaN/Inf guards (WASM)', () => {
  it('resample rejects invalid source/target sample rates before allocation', () => {
    expect(() => resample(sine(), 0, SR)).toThrow();
    expect(() => resample(sine(), SR, 2_000_000_000)).toThrow();
  });
  it('feature-core wrappers reject NaN and inconsistent matrix dimensions', () => {
    expect(() => pcen(new Float32Array([1, Number.NaN, 3, 4]), 2, 2)).toThrow();
    expect(() => pcen(new Float32Array([1, 2, 3]), 2, 2)).toThrow();
    expect(() => tempogram(withNaN(), SR)).toThrow();
    expect(() => plp(withNaN(), SR)).toThrow();
    // The cyclic and ratio tempogram wrappers previously skipped finiteness
    // validation and silently returned NaN-filled arrays (unlike the C ABI).
    expect(() => cyclicTempogram(withNaN(), SR)).toThrow(/NaN|Inf/);
    expect(() => tempogramRatio(withNaN(), 64, SR)).toThrow(/NaN|Inf/);
  });
  it('StreamingEqualizer.match rejects non-finite reference audio', () => {
    const equalizer = new StreamingEqualizer({ sampleRate: SR, maxBlockSize: 128 });
    expect(() => equalizer.match(sine(), withNaN(), { maxBands: 4 })).toThrow();
    equalizer.delete();
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

describe('detailed-analysis config geometry matches the C ABI (WASM)', () => {
  // The detailed analyzers call the C++ analyzer classes directly (no C ABI
  // round-trip), so the WASM bindings must enforce the same config contract the
  // flat C ABI does instead of letting the analyzer silently clamp.
  const audio = sine(SR);
  it('analyzeBpm rejects an inverted BPM range', () => {
    expect(() => analyzeBpm(audio, SR, { bpmMin: 200, bpmMax: 100 })).toThrow();
  });
  it('analyzeBpm rejects a non-positive bpmMin', () => {
    expect(() => analyzeBpm(audio, SR, { bpmMin: 0 })).toThrow();
  });
  it('analyzeRhythm rejects an inverted BPM range', () => {
    expect(() => analyzeRhythm(audio, SR, { bpmMin: 200, bpmMax: 100 })).toThrow();
  });
  it('analyzeDynamics rejects a non-positive window', () => {
    expect(() => analyzeDynamics(audio, SR, { windowSec: 0 })).toThrow();
  });
  it('analyzeDynamics rejects a negative compression threshold', () => {
    expect(() => analyzeDynamics(audio, SR, { compressionThreshold: -1 })).toThrow();
  });
  it('analyzeTimbre rejects non-positive nMels/nMfcc', () => {
    expect(() => analyzeTimbre(audio, SR, { nMels: 0 })).toThrow();
    expect(() => analyzeTimbre(audio, SR, { nMfcc: 0 })).toThrow();
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
    run: (value: number) => unknown;
  }

  const fields: NonFiniteField[] = [
    {
      entry: 'analyzeMelody',
      key: 'fmin',
      run: (v) => m().analyzeMelody(tone, SR, v, 2093, 2048, 256, 0.1, false, true),
    },
    {
      entry: 'cqt',
      key: 'fmin',
      run: (v) => m().cqt(cqtTone, SR, 512, v, 12, 12),
    },
    {
      entry: 'vqt',
      key: 'fmin',
      run: (v) => m().vqt(cqtTone, SR, 512, v, 12, 12, 0),
    },
  ];

  const cases = fields.flatMap((field) => nonFinite.map((value) => ({ ...field, value })));

  it.each(cases)('$entry refuses $key = $value', ({ run, value }) => {
    expect(() => run(value)).toThrow();
  });

  it.each(fields)('$entry refuses a $key that saturates onto an infinity', ({ run }) => {
    expect(() => run(SATURATES_ONTO_AN_INFINITY)).toThrow();
  });

  it('vqt refuses an infinite gamma', () => {
    expect(() => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, Number.POSITIVE_INFINITY)).toThrow();
    expect(() => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, Number.NEGATIVE_INFINITY)).toThrow();
    expect(() => m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, SATURATES_ONTO_AN_INFINITY)).toThrow();
  });

  it('vqt reads a NaN gamma as the automatic-bandwidth sentinel', () => {
    // resolve_vqt_gamma (src/feature/vqt.cpp) resolves NaN and any negative
    // gamma to the same ERB-derived value, so the two must agree exactly while
    // gamma = 0 (the CQT-equivalent bandwidth) must not.
    const auto = Array.from(m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, Number.NaN).magnitude);
    const negative = Array.from(m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, -1).magnitude);
    const cqtEquivalent = Array.from(m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, 0).magnitude);
    expect(auto).toEqual(negative);
    expect(auto).not.toEqual(cqtEquivalent);
  });

  it('refuses an inverted but finite ordering pair', () => {
    expect(() => m().analyzeBpm(tone, SR, 200, 100, 120, 2048, 512, 5)).toThrow();
    expect(() => m().analyzeRhythm(tone, SR, 200, 100, 120, 2048, 512)).toThrow();
    expect(() => m().analyzeMelody(tone, SR, 400, 200, 2048, 256, 0.1, false, true)).toThrow();
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
    const result = m().cqt(cqtTone, SR, 512, CQT_FMIN, 12, 12);
    expect(result.nBins).toBe(12);
    expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
  });
  it('vqt accepts a finite fmin and gamma', () => {
    const result = m().vqt(cqtTone, SR, 512, CQT_FMIN, 12, 12, 0);
    expect(result.nBins).toBe(12);
    expect(result.magnitude.length).toBe(result.nBins * result.nFrames);
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
