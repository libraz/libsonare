/**
 * Smoke tests for the 12 functions newly exposed to the WASM binding
 * (spectralContrast / polyFeatures / zeroCrossings / pitchTuning /
 * estimateTuning / decompose / nnFilter / remix / phaseVocoder /
 * hpssWithResidual / lufsInterleaved / ebur128LoudnessRange). These guard the
 * TS facade wiring; the DSP itself is validated at the C ABI / C++ layer.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import {
  decompose,
  ebur128LoudnessRange,
  estimateTuning,
  hpssWithResidual,
  init,
  lufsInterleaved,
  nnFilter,
  phaseVocoder,
  pitchPyin,
  pitchTuning,
  pitchYin,
  polyFeatures,
  realtimeVoiceChangerPresetConfig,
  remix,
  spectralContrast,
  spectralEdit,
  voiceCharacterPresetId,
  zeroCrossings,
} from '../src/index';

const SR = 22050;

function makeSine(durationSec: number, freqHz: number): Float32Array {
  const n = Math.floor(SR * durationSec);
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / SR);
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
    const r = spectralContrast(makeSine(1, 440), SR, 2048, 512, 6);
    expect(r.rows).toBe(7);
    expect(r.cols).toBeGreaterThan(0);
    expect(r.data.length).toBe(r.rows * r.cols);
    expect(allFinite(r.data)).toBe(true);
  });

  it('polyFeatures returns an (order+1) x nFrames matrix', () => {
    const r = polyFeatures(makeSine(1, 440), SR, 2048, 512, 1);
    expect(r.rows).toBe(2);
    expect(r.cols).toBeGreaterThan(0);
    expect(allFinite(r.data)).toBe(true);
  });

  it('zeroCrossings returns sorted in-range indices', () => {
    const x = makeSine(0.05, 440);
    const idx = zeroCrossings(x);
    expect(idx.length).toBeGreaterThan(0);
    for (let i = 1; i < idx.length; i++) {
      expect(idx[i]).toBeGreaterThan(idx[i - 1]);
      expect(idx[i]).toBeLessThan(x.length);
    }
  });

  it('pitchTuning returns a finite deviation', () => {
    const t = pitchTuning(new Float32Array([440, 880, 660]));
    expect(Number.isFinite(t)).toBe(true);
    expect(t).toBeGreaterThanOrEqual(-0.5);
    expect(t).toBeLessThan(0.5);
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
    const r = decompose(s, nFeatures, nFrames, nComponents, 20, 2.0);
    expect(r.w.length).toBe(nFeatures * nComponents);
    expect(r.h.length).toBe(nComponents * nFrames);
    expect(allFinite(r.w)).toBe(true);
    expect(allFinite(r.h)).toBe(true);
  });

  it('nnFilter preserves the spectrogram shape', () => {
    const nFeatures = 12;
    const nFrames = 20;
    const s = new Float32Array(nFeatures * nFrames);
    for (let i = 0; i < s.length; i++) {
      s[i] = Math.abs(Math.cos(i));
    }
    const r = nnFilter(s, nFeatures, nFrames);
    expect(r.rows).toBe(nFeatures);
    expect(r.cols).toBe(nFrames);
    expect(allFinite(r.data)).toBe(true);
  });

  it('decompose and nnFilter reject invalid dimensions', () => {
    const small = new Float32Array([1, 2, 3, 4]);
    expect(() => decompose(small, 2, 2, 0, 20, 2.0)).toThrow();
    expect(() => decompose(small, 2, 2, 2, 0, 2.0)).toThrow();
    expect(() => decompose(small, 1000, 1000, 2, 20, 2.0)).toThrow();
    expect(() => nnFilter(small, 1000, 1000)).toThrow();
  });

  it('remix concatenates interval slices', () => {
    const x = makeSine(0.5, 440);
    const half = Math.floor(x.length / 2);
    const out = remix(x, new Int32Array([half, x.length, 0, half]));
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
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
    const out = remix(x, new Int32Array([boundary, length]));
    expect(out.length).toBe(length - boundary);
    // Exact boundary => first output sample is x[boundary] (1), not x[boundary-1] (2).
    expect(out[0]).toBe(1);
  });

  it('voiceCharacterPresetId maps a known ordinal to its canonical id', () => {
    // Ordinal 1 == SONARE_VC_PRESET_BRIGHT_IDOL.
    expect(voiceCharacterPresetId(1)).toBe('bright-idol');
    expect(voiceCharacterPresetId(0)).toBe('neutral-monitor');
    // Out-of-range ordinal yields null.
    expect(voiceCharacterPresetId(999)).toBeNull();
  });

  it('realtimeVoiceChangerPresetConfig returns a POD object with expected fields', () => {
    const cfg = realtimeVoiceChangerPresetConfig('bright-idol');
    expect(cfg).not.toBeNull();
    if (cfg === null) {
      return;
    }
    const expectedFields = [
      'input_gain_db',
      'output_gain_db',
      'wet_mix',
      'retune_semitones',
      'retune_mix',
      'retune_grain_size',
      'formant_factor',
      'eq_highpass_hz',
      'gate_threshold_db',
      'compressor_threshold_db',
      'deesser_frequency_hz',
      'reverb_mix',
      'reverb_seed',
      'limiter_ceiling_db',
      'limiter_release_ms',
    ] as const;
    for (const field of expectedFields) {
      expect(cfg).toHaveProperty(field);
      expect(Number.isFinite(cfg[field])).toBe(true);
    }
    // ISP true-peak limiter fields are now part of the POD surface.
    expect(cfg).toHaveProperty('limiter_enable_isp_limiter');
    expect(cfg).toHaveProperty('limiter_isp_ceiling_dbtp');
    expect(Number.isFinite(cfg.limiter_isp_ceiling_dbtp)).toBe(true);
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = makeSine(0.5, 440);
    const out = phaseVocoder(x, 2.0);
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('spectralEdit with no ops is an identity transform', () => {
    const x = makeSine(0.5, 440);
    const out = spectralEdit(x, SR, []);
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
    const out = spectralEdit(x, SR, [
      { startSample: 0, endSample: n, lowHz: 4000, highHz: 6000, gainDb: -24, mode: 'attenuate' },
    ]);
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
    const out = spectralEdit(x, SR, [{ startSample: 0, lowHz: 4000, highHz: 6000, mode: 'mute' }]);
    expect(out.length).toBe(n);
    expect(goertzelPower(out, 5000)).toBeLessThan(goertzelPower(x, 5000) * 0.1);
  });

  it('spectralEdit healRadiusFrames:0 keeps the core default (matches C ABI)', () => {
    const x = makeSine(0.5, 440);
    // 0 means "keep the default" at the C-ABI oracle; WASM must not throw here.
    const out = spectralEdit(x, SR, [], { healRadiusFrames: 0 });
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('spectralEdit nFft:0 / hopLength:0 keep the core defaults', () => {
    const x = makeSine(0.5, 440);
    const out = spectralEdit(x, SR, [], { nFft: 0, hopLength: 0 });
    expect(out.length).toBe(x.length);
    expect(allFinite(out)).toBe(true);
  });

  it('spectralEdit rejects an out-of-range sampleRate', () => {
    const x = makeSine(0.1, 440);
    expect(() => spectralEdit(x, 10, [])).toThrow();
    expect(() => spectralEdit(x, 1_000_000, [])).toThrow();
  });

  it('hpssWithResidual splits into three signals', () => {
    const r = hpssWithResidual(makeSine(1, 440));
    expect(r.harmonic.length).toBe(r.percussive.length);
    expect(r.percussive.length).toBe(r.residual.length);
    expect(r.harmonic.length).toBeGreaterThan(0);
    expect(allFinite(r.harmonic)).toBe(true);
  });

  it('lufsInterleaved measures dual-mono loudness', () => {
    const x = makeSine(1, 440);
    const interleaved = new Float32Array(x.length * 2);
    for (let i = 0; i < x.length; i++) {
      interleaved[2 * i] = x[i];
      interleaved[2 * i + 1] = x[i];
    }
    const r = lufsInterleaved(interleaved, 2, SR);
    expect(Number.isFinite(r.integratedLufs)).toBe(true);
    expect(r.integratedLufs).toBeLessThan(0);
  });

  it('ebur128LoudnessRange is finite and non-negative', () => {
    const lra = ebur128LoudnessRange(makeSine(1, 440));
    expect(Number.isFinite(lra)).toBe(true);
    expect(lra).toBeGreaterThanOrEqual(0);
  });

  it('pitchYin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR); // fully unvoiced
    const nanRes = pitchYin(silence, SR, 2048, 512, 65, 2093, 0.3, false);
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

    const filled = pitchYin(silence, SR, 2048, 512, 65, 2093, 0.3, true);
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });

  it('pitchPyin fillNa controls the unvoiced value', () => {
    const silence = new Float32Array(SR);
    const nanRes = pitchPyin(silence, SR, 2048, 512, 65, 2093, 0.3, false);
    expect(nanRes.f0.some((v) => Number.isNaN(v))).toBe(true);

    const filled = pitchPyin(silence, SR, 2048, 512, 65, 2093, 0.3, true);
    expect(filled.f0.every((v) => Number.isFinite(v))).toBe(true);
  });
});
