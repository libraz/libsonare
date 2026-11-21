/**
 * Smoke tests for the 12 functions newly exposed to the WASM binding
 * (spectralContrast / polyFeatures / zeroCrossings / pitchTuning /
 * estimateTuning / decompose / nnFilter / remix / phaseVocoder /
 * hpssWithResidual / lufsMultichannel / ebur128LoudnessRangeLu). These guard the
 * TS facade wiring; the DSP itself is validated at the C ABI / C++ layer.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  decompose,
  decomposeStems,
  ebur128LoudnessRangeLu,
  estimateTuning,
  hpssWithResidual,
  init,
  lufsMultichannel,
  lufsSeriesMultichannel,
  momentaryLufs,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  polyFeatures,
  realtimeVoiceChangerPresetConfig,
  remix,
  remixAlignedIntervals,
  spectralContrast,
  spectralEdit,
  voiceCharacterPresetId,
  zeroCrossings,
} from '../src/index';
import { sine as genSine } from './_helpers';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  return genSine(freqHz, durationSec, { amp: 0.5, sampleRate: SR });
}

/** Deterministic white noise; a different `seed` is an uncorrelated realization. */
function noise(durationSec: number, seed: number, amplitude = 0.25): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  let state = seed >>> 0;
  for (let i = 0; i < n; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = amplitude * ((2 * (state >>> 8)) / (1 << 24) - 1);
  }
  return out;
}

/** Single-frequency power estimate (Goertzel), used to probe a tone's energy. */
function goertzelPower(x: ArrayLike<number>, freqHz: number): number {
  const w = (2 * Math.PI * freqHz) / SR;
  const cw = Math.cos(w);
  const sw = Math.sin(w);
  const coeff = 2 * cw;
  let s1 = 0;
  let s2 = 0;
  for (let i = 0; i < x.length; i++) {
    const s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const real = s1 - s2 * cw;
  const imag = s2 * sw;
  return real * real + imag * imag;
}

function allFinite(arr: ArrayLike<number>): boolean {
  for (let i = 0; i < arr.length; i++) {
    if (!Number.isFinite(arr[i])) {
      return false;
    }
  }
  return arr.length > 0;
}

describe('newly exposed WASM functions', () => {
  beforeAll(async () => {
    await init();
  });

  it('spectralContrast returns an (nBands+1) x nFrames matrix', () => {
    const r = spectralContrast(makeSine(1, 440), SR, { nFft: 2048, hopLength: 512, nBands: 6 });
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);
    expect(r.data.length).toBe(r.rows * r.cols);
    expect(allFinite(r.data)).toBe(true);
  });

  it('spectralContrast reports a band narrower than one bin as unmeasurable', () => {
    // At a 64-point transform the bin is 344.5 Hz, so band 0 (below fmin = 200 Hz)
    // is emptied by the overlap trim and has nothing to average. That reaches the
    // caller as NaN rather than a level borrowed from the bins around it.
    const r = spectralContrast(makeSine(1, 440), SR, {
      nFft: 64,
      hopLength: 16,
      nBands: 6,
      fminHz: 200.0,
    });
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);

    for (let t = 0; t < r.cols; t++) {
      expect(Number.isNaN(r.data[t])).toBe(true);
    }
    // Every band wide enough to hold a bin still carries a number, so the
    // assertion above cannot be passing on an all-NaN matrix.
    for (let b = 1; b < r.rows; b++) {
      expect(allFinite(r.data.subarray(b * r.cols, (b + 1) * r.cols))).toBe(true);
    }
  });

  it('polyFeatures returns an (order+1) x nFrames matrix', () => {
    const r = polyFeatures(makeSine(1, 440), SR, { nFft: 2048, hopLength: 512, order: 1 });
    expect(r.rows).toBe(2);
    expect(r.cols).toBeGreaterThan(0);
    expect(allFinite(r.data)).toBe(true);
  });

  it('zeroCrossings returns sorted in-range indices', () => {
    const x = makeSine(0.05, 440);
    const idx = zeroCrossings(x, SR);
    expect(idx.length).toBeGreaterThan(0);
    for (let i = 1; i < idx.length; i++) {
      expect(idx[i]).toBeGreaterThan(idx[i - 1]);
      expect(idx[i]).toBeLessThan(x.length);
    }
  });

  it('pitchTuning returns a finite deviation', () => {
    const t = pitchTuning({ frequencies: new Float32Array([440, 880, 660]) });
    expect(Number.isFinite(t)).toBe(true);
    expect(t).toBeGreaterThanOrEqual(-50);
    expect(t).toBeLessThan(50);
  });

  it('estimateTuning returns a finite deviation', () => {
    const t = estimateTuning(makeSine(1, 440), SR);
    expect(Number.isFinite(t)).toBe(true);
  });

  it('decompose factorizes a non-negative spectrogram', () => {
    const nFeatures = 16;
    const nFrames = 24;
    const nComponents = 3;
    const s = new Float32Array(nFeatures * nFrames);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.sin(i));
    }
    const r = decompose({
      s: { data: s, rows: nFeatures, cols: nFrames },
      nComponents,
      nIter: 20,
      beta: 2.0,
    });
    expect(r.w.rows).toBe(nFeatures);
    expect(r.w.cols).toBe(nComponents);
    expect(r.h.rows).toBe(nComponents);
    expect(r.h.cols).toBe(nFrames);
    expect(r.w.data.length).toBe(nFeatures * nComponents);
    expect(r.h.data.length).toBe(nComponents * nFrames);
    expect(allFinite(r.w.data)).toBe(true);
    expect(allFinite(r.h.data)).toBe(true);
  });

  it('nnFilter preserves the spectrogram shape', () => {
    const nFeatures = 12;
    const nFrames = 20;
    const s = new Float32Array(nFeatures * nFrames);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.cos(i));
    }
    const r = nnFilter({ s: { data: s, rows: nFeatures, cols: nFrames } });
    expect(r.rows).toBe(nFeatures);
    expect(r.cols).toBe(nFrames);
    expect(allFinite(r.data)).toBe(true);
  });

  it('decompose and nnFilter reject invalid dimensions', () => {
    const small = new Float32Array([1, 2, 3, 4]);
    const s2 = { data: small, rows: 2, cols: 2 };
    const huge = { data: small, rows: 1000, cols: 1000 };
    expect(() => decompose({ s: s2, nComponents: 0, nIter: 20, beta: 2.0 })).toThrow();
    expect(() => decompose({ s: s2, nComponents: 2, nIter: 0, beta: 2.0 })).toThrow();
    expect(() => decompose({ s: huge, nComponents: 2, nIter: 20, beta: 2.0 })).toThrow();
    expect(() => nnFilter({ s: huge })).toThrow();
  });

  it('remix concatenates interval slices', () => {
    const x = makeSine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix({
      samples: x,
      sampleRate: SR,
      intervals: new Int32Array([half, x.length, 0, half]),
      alignZeros: false,
    });
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('remixAlignedIntervals resolves one clamped pair per interval', () => {
    const x = makeSine(0.5, 440);
    const pairs = remixAlignedIntervals({
      samples: x,
      sampleRate: SR,
      intervals: new Int32Array([0, 1000, 5000, 5500]),
    });
    expect(pairs.length).toBe(4);
    expect(pairs[0]).toBeGreaterThanOrEqual(0);
    expect(pairs[1]).toBeGreaterThan(pairs[0]);
    expect(pairs[3]).toBeGreaterThan(pairs[2]);
    expect(pairs[3]).toBeLessThanOrEqual(x.length);
  });

  it('remix and remixAlignedIntervals refuse an odd-length interval list as RangeError', () => {
    const x = makeSine(0.5, 440);
    const odd = new Int32Array([0, 1000, 2000]);
    expect(() => remix({ samples: x, sampleRate: SR, intervals: odd })).toThrow(RangeError);
    expect(() => remixAlignedIntervals({ samples: x, sampleRate: SR, intervals: odd })).toThrow(
      RangeError,
    );
  });

  it('remixAlignedIntervals leaves a signal with no sign change unsnapped', () => {
    const flat = new Float32Array(4096).fill(0.25);
    const pairs = remixAlignedIntervals({
      samples: flat,
      sampleRate: SR,
      intervals: new Int32Array([100, 200]),
    });
    expect(Array.from(pairs)).toEqual([100, 200]);
  });

  it('decomposeStems components carry phase and sum back to the input', () => {
    const n = 8192;
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      x[i] = 0.5 * Math.sin((2 * Math.PI * (i < n / 2 ? 220 : 880) * i) / SR);
    }
    const r = decomposeStems({
      samples: x,
      sampleRate: SR,
      nComponents: 2,
      nFft: 1024,
      hopLength: 256,
      nIter: 30,
    });
    expect(r.components.length).toBe(2);
    expect(r.components[0].length).toBe(n);
    expect(r.w.length).toBe((1024 / 2 + 1) * 2);
    expect(r.h.length % 2).toBe(0);
    expect(r.sampleRate).toBe(SR);
    let err = 0;
    let ref = 0;
    for (let i = 1024; i < n - 1024; i++) {
      const sum = r.components[0][i] + r.components[1][i];
      err += (sum - x[i]) ** 2;
      ref += x[i] ** 2;
    }
    expect(Math.sqrt(err / ref)).toBeLessThan(0.05);
  });

  it('decomposeStems rejects an out-of-range mask power', () => {
    expect(() =>
      decomposeStems({ samples: makeSine(0.2, 440), sampleRate: SR, maskPower: 0.5 }),
    ).toThrow();
  });

  it('remix slices at an exact large integer boundary (>2^24)', () => {
    // A boundary above 2^24 (16,777,216) is not representable losslessly as a
    // float32. The signal is a counter (sample i has value i) so the first
    // remixed sample reveals the exact start index the native side used. The
    // Int32 path must preserve `boundary` exactly; the old float32 path would
    // round it (16,777,217 -> 16,777,216) and slice one sample early.
    const boundary = 16_777_217; // 2^24 + 1
    const length = boundary + 4;
    const x = new Float32Array(length);
    // Beyond 2^24, consecutive integers are not all representable as float32, so
    // only mark the boundary-relevant samples with sentinel values that round-
    // trip exactly (they are small integers stored at large indices).
    x[boundary] = 1; // first sample of the [boundary, length) slice
    x[boundary - 1] = 2; // last sample that must be excluded
    const out = remix({
      samples: x,
      sampleRate: SR,
      intervals: new Int32Array([boundary, length]),
      alignZeros: false,
    });
    expect(out.length).toBe(length - boundary);
    // Exact boundary => first output sample is x[boundary] (1), not x[boundary-1] (2).
    expect(out[0]).toBe(1);
  });

  it('voiceCharacterPresetId maps known values and handles unknown values', () => {
    // Ordinal 1 == SONARE_VC_PRESET_BRIGHT_IDOL.
    expect(voiceCharacterPresetId(1)).toBe('brightIdol');
    expect(voiceCharacterPresetId(0)).toBe('neutralMonitor');
    expect(() => voiceCharacterPresetId(999)).toThrow(RangeError);
    expect(() => voiceCharacterPresetId('not-a-preset' as never)).toThrow(TypeError);
    expect(() => realtimeVoiceChangerPresetConfig(-1 as never)).toThrow(TypeError);
    expect(() => realtimeVoiceChangerPresetConfig('not-a-preset' as never)).toThrow(RangeError);
  });

  it('realtimeVoiceChangerPresetConfig returns a POD object with expected fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('brightIdol');
    expect(cfg).not.toBeNull();
    if (cfg === null) {
      return;
    }
    const expectedFields = [
      'inputGainDb',
      'outputGainDb',
      'wetMix',
      'retunePitchSemitones',
      'retuneMix',
      'formantFactor',
      'eqHighPassHz',
      'gateThresholdDb',
      'compressorThresholdDb',
      'deesserFrequencyHz',
      'reverbMix',
      'reverbSeed',
      'limiterCeilingDb',
      'limiterReleaseMs',
    ] as const;
    for (const field of expectedFields) {
      expect(cfg).toHaveProperty(field);
      expect(Number.isFinite(cfg[field])).toBe(true);
    }
    // ISP true-peak limiter fields are now part of the POD surface.
    expect(cfg).toHaveProperty('limiterEnableIspLimiter');
    expect(cfg).toHaveProperty('limiterIspCeilingDbtp');
    expect(Number.isFinite(cfg.limiterIspCeilingDbtp)).toBe(true);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = makeSine(0.5, 440);
    const out = phaseVocoder(x, SR, { speedRatio: 2.0 });
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('spectralEdit with no ops is an identity transform', () => {
    const x = makeSine(0.5, 440);
    const out = spectralEdit(x, SR, { ops: [] });
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
    const skip = 2048;
    let sig = 0;
    let noise = 0;
    for (let i = skip; i < x.length - skip; i++) {
      sig += x[i] * x[i];
      const d = x[i] - out[i];
      noise += d * d;
    }
    expect(10 * Math.log10(sig / noise)).toBeGreaterThan(20);
  });

  it('spectralEdit attenuates a frequency band', () => {
    const n = Math.floor(SR * 0.5);
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      x[i] =
        0.4 * Math.sin((2 * Math.PI * 1000 * i) / SR) +
        0.4 * Math.sin((2 * Math.PI * 5000 * i) / SR);
    }
    const out = spectralEdit(x, SR, {
      ops: [
        { startSample: 0, endSample: n, lowHz: 4000, highHz: 6000, gainDb: -24, mode: 'attenuate' },
      ],
    });
    expect(out.length).toBe(n);
    // 5 kHz tone drops substantially; 1 kHz tone is preserved.
    expect(goertzelPower(out, 5000)).toBeLessThan(goertzelPower(x, 5000) * 0.1);
    expect(goertzelPower(out, 1000)).toBeGreaterThan(goertzelPower(x, 1000) * 0.5);
  });

  it('spectralEdit omitted endSample spans the whole signal', () => {
    // An omitted endSample defaults to the signal length (matching Node), so a
    // mute region with only startSample muted the full band -- previously the
    // WASM path left end_sample at 0 and silently did nothing.
    const n = Math.floor(SR * 0.5);
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      x[i] =
        0.4 * Math.sin((2 * Math.PI * 1000 * i) / SR) +
        0.4 * Math.sin((2 * Math.PI * 5000 * i) / SR);
    }
    const out = spectralEdit(x, SR, {
      ops: [{ startSample: 0, lowHz: 4000, highHz: 6000, mode: 'mute' }],
    });
    expect(out.length).toBe(n);
    expect(goertzelPower(out, 5000)).toBeLessThan(goertzelPower(x, 5000) * 0.1);
  });

  it.each([
    ['nFft', 'nFft must be a power of two in [4, 262144]'],
    ['hopLength', 'hopLength must be an integer in [1, 2147483647]'],
    ['healRadiusFrames', 'healRadiusFrames must be an integer in [1, 2147483647]'],
  ] as const)('spectralEdit refuses %s 0 instead of reading it as the default', (field, text) => {
    const x = makeSine(0.1, 440);
    expect(() => spectralEdit(x, SR, { ops: [], [field]: 0 })).toThrow(
      new RangeError(`spectralEdit: ${text}`),
    );
  });

  it('spectralEdit refuses a high edge that is not a positive frequency instead of reading it as Nyquist', () => {
    const x = makeSine(0.1, 440);
    for (const highHz of [0, -5, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(
        () => spectralEdit(x, SR, { ops: [{ mode: 'mute' }, { highHz, mode: 'mute' }] }),
        `highHz ${highHz}`,
      ).toThrow(
        new RangeError(
          'spectralEdit: ops[1].highHz must be finite and greater than 0; omit it for Nyquist',
        ),
      );
    }
    const kept = spectralEdit(x, SR, { ops: [{ highHz: 1e6, mode: 'gain', gainDb: 0 }] });
    expect(kept.length).toBe(x.length);
  });

  it('spectralEdit rejects an out-of-range sampleRate', () => {
    const x = makeSine(0.1, 440);
    expect(() => spectralEdit(x, 10, { ops: [] })).toThrow();
    expect(() => spectralEdit(x, 1_000_000, { ops: [] })).toThrow();
  });

  it('hpssWithResidual splits into three signals', () => {
    const r = hpssWithResidual(makeSine(1, 440), SR);
    expect(r.harmonic.length).toBe(r.percussive.length);
    expect(r.percussive.length).toBe(r.residual.length);
    expect(r.harmonic.length).toBeGreaterThan(0);
    expect(allFinite(r.harmonic)).toBe(true);
  });

  it('lufsMultichannel measures dual-mono loudness', () => {
    const x = makeSine(1, 440);
    const r = lufsMultichannel({ channels: [x, x], sampleRate: SR });
    expect(Number.isFinite(r.integratedLufs)).toBe(true);
    expect(r.integratedLufs).toBeLessThan(0);
  });

  it('lufsSeriesMultichannel sums channel energies rather than averaging loudness', () => {
    // BS.1770-4 sums the K-weighted per-channel block energies, so two
    // uncorrelated channels at one level read 10*log10(2) above their
    // per-channel mean. A facade that measured each channel and averaged the
    // results in dB would put this difference at exactly 0.
    const left = noise(4, 12345);
    const right = noise(4, 987654321);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [left, right],
      sampleRate: SR,
    });
    const monoLeft = momentaryLufs(left, SR);
    const monoRight = momentaryLufs(right, SR);

    expect(momentary.length).toBe(monoLeft.length);
    expect(momentary.length).toBe(monoRight.length);
    expect(allFinite(momentary)).toBe(true);
    for (let i = 0; i < momentary.length; i++) {
      expect(momentary[i] - 0.5 * (monoLeft[i] + monoRight[i])).toBeCloseTo(3.0103, 1);
    }
    expect(shortTerm.length).toBeGreaterThan(0);
    expect(allFinite(shortTerm)).toBe(true);
  });

  it('lufsSeriesMultichannel matches the mono meters for one channel', () => {
    const x = noise(4, 24680);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [x],
      sampleRate: SR,
    });
    expect(Array.from(momentary)).toEqual(Array.from(momentaryLufs(x, SR)));
    expect(shortTerm.length).toBeGreaterThan(0);
  });

  it('lufsSeriesMultichannel returns an empty series below a window', () => {
    // 2 s clears the 400 ms momentary window and not the 3 s short-term one.
    const x = noise(2, 13579);
    const { momentaryLufs: momentary, shortTermLufs: shortTerm } = lufsSeriesMultichannel({
      channels: [x, x],
      sampleRate: SR,
    });
    expect(momentary.length).toBeGreaterThan(0);
    expect(shortTerm.length).toBe(0);
  });

  it('lufsSeriesMultichannel rejects an empty plane list', () => {
    expect(() => lufsSeriesMultichannel({ channels: [], sampleRate: SR })).toThrow(RangeError);
  });

  it('ebur128LoudnessRangeLu is finite and non-negative', () => {
    const lra = ebur128LoudnessRangeLu(makeSine(1, 440), SR);
    expect(Number.isFinite(lra)).toBe(true);
    expect(lra).toBeGreaterThanOrEqual(0);
  });

  it('pitchYin returns librosa-style estimates for unvoiced frames', () => {
    const silence = new Float32Array(SR); // fully unvoiced
    const nanRes = pitchYin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
    });
    expect(nanRes.f0.every((v) => Number.isFinite(v))).toBe(true);
    expect(nanRes.voicedFlag.every((v) => !v)).toBe(true);
  });

  it('pitchYin refuses fillNa, which only pYIN reads', () => {
    const silence = new Float32Array(SR);
    expect(() => pitchYin({ samples: silence, sampleRate: SR, fillNa: true } as never)).toThrow(
      /fillNa/,
    );
    expect(() => pitchYin(silence, SR, { fillNa: false } as never)).toThrow(/fillNa/);
  });

  it('pitchPyin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR);
    const nanRes = pitchPyin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
      fillNa: false,
    });
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

    const filled = pitchPyin(silence, SR, {
      frameLength: 2048,
      hopLength: 512,
      f0MinHz: 65,
      f0MaxHz: 2093,
      threshold: 0.3,
      fillNa: true,
    });
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });
});
