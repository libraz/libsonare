/**
 * An acoustic value the analyzer did not measure is an absent key, never NaN, an empty array or an
 * all-NaN array; a band array that was measured keeps its full length with NaN per band not fitted.
 */

import { describe, expect, it } from 'vitest';
import {
  analyzeImpulseResponse,
  detectAcoustic,
  estimateRoom,
  roomGeometryFromEstimate,
  synthesizeRir,
} from '../src/index.js';

function noise(length: number, seed = 7): Float32Array {
  let state = seed;
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    state = (state * 1103515245 + 12345) & 0x7fffffff;
    out[i] = (state / 0x7fffffff) * 2 - 1;
  }
  return out;
}

const ACOUSTIC_SCALARS = ['rt60', 'edt', 'c50', 'c80', 'd50'] as const;
const ACOUSTIC_BANDS = ['rt60Bands', 'edtBands', 'c50Bands', 'c80Bands'] as const;

describe('acoustic values that were not measured', () => {
  it('omits the clarity family and its band arrays on the blind path', () => {
    const result = detectAcoustic(noise(48000 * 2), 48000);
    expect(result.isBlind).toBe(true);
    for (const key of ['edt', 'c50', 'c80', 'd50', 'edtBands', 'c50Bands', 'c80Bands'] as const) {
      expect(key in result, key).toBe(false);
    }
    for (const key of ACOUSTIC_SCALARS) {
      if (key in result) {
        expect(Number.isNaN(result[key]), key).toBe(false);
      }
    }
    if (result.rt60Bands !== undefined) {
      expect(result.rt60Bands).toHaveLength(6);
      expect(result.rt60Bands.some((value) => !Number.isNaN(value))).toBe(true);
    }
  });

  it('keeps every band array of a measured impulse response at full length', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    const result = analyzeImpulseResponse(rir.rir, 48000);
    expect(result.isBlind).toBe(false);
    for (const key of ACOUSTIC_SCALARS) {
      expect(Number.isFinite(result[key]), key).toBe(true);
    }
    for (const key of ACOUSTIC_BANDS) {
      expect(result[key], key).toHaveLength(6);
    }
  });

  it('omits the dimensions and both band arrays of a silent room estimate', () => {
    const estimate = estimateRoom(new Float32Array(48000), 48000);
    expect(estimate.confidence).toBe(0);
    for (const key of [
      'volume',
      'lengthM',
      'widthM',
      'heightM',
      'bandAbsorption',
      'rt60Bands',
    ] as const) {
      expect(key in estimate, key).toBe(false);
    }
    expect(() => roomGeometryFromEstimate({ estimate })).toThrow(/no measurable dimensions/);
  });
});
