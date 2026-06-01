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
    if (cfg === null) return;
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
  });

  it('phaseVocoder time-scales the signal', () => {
    const x = makeSine(0.5, 440);
    const out = phaseVocoder(x, 2.0);
    expect(out.length).toBeGreaterThan(0);
    expect(out.length).toBeLessThan(x.length);
    expect(allFinite(out)).toBe(true);
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
