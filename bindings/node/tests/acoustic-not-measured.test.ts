/**
 * An acoustic value the analyzer did not measure is an absent key, never NaN; a band array of a
 * measurement that was taken keeps its full length with NaN per band not fitted, even every band.
 */

import { describe, expect, it } from 'vitest';
import {
  analyzeImpulseResponse,
  detectAcoustic,
  estimateRoom,
  masteringRepairDereverbConfigForRoom,
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
    expect(result.rt60Bands).toHaveLength(6);
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

  it('omits the dimensions and the DRR of a silent room estimate and keeps both band arrays as NaN', () => {
    const estimate = estimateRoom(new Float32Array(48000), 48000);
    expect(estimate.confidence).toBe(0);
    for (const key of ['volume', 'lengthM', 'widthM', 'heightM', 'drrDb'] as const) {
      expect(key in estimate, key).toBe(false);
    }
    for (const key of ['bandAbsorption', 'rt60Bands'] as const) {
      expect(estimate[key], key).toHaveLength(6);
      expect(Array.from(estimate[key] ?? []).every(Number.isNaN), key).toBe(true);
    }
    expect(() => roomGeometryFromEstimate({ estimate })).toThrow(/no measurable dimensions/);
  });

  it('reads a lone impulse, whose direct sound leaves nothing outside its window, as an infinite DRR', () => {
    const samples = new Float32Array(48000);
    samples[100] = 1;
    const estimate = estimateRoom(samples, 48000);
    expect('drrDb' in estimate).toBe(true);
    expect(estimate.drrDb).toBe(Number.POSITIVE_INFINITY);
  });
});

describe('a room and reverberation time that were not measured', () => {
  it('omits the reverberation time of a silent recording and keeps its bands as NaN', () => {
    const result = detectAcoustic(new Float32Array(48000), 48000);
    expect('rt60' in result).toBe(false);
    expect(result.rt60Bands).toHaveLength(6);
    expect(Array.from(result.rt60Bands ?? []).every(Number.isNaN)).toBe(true);
    expect('edtBands' in result).toBe(false);
  });

  it('keeps the volume and dimensions of a measured estimate as numbers', () => {
    const rir = synthesizeRir({
      sampleRate: 48000,
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      absorption: 0.15,
    });
    const estimate = estimateRoom(rir.rir, 48000);
    for (const key of ['volume', 'lengthM', 'widthM', 'heightM'] as const) {
      if (key in estimate) {
        expect(Number.isFinite(estimate[key]), key).toBe(true);
      }
    }
  });

  it('accepts an estimate with an absent volume and leaves the dereverb delay as given', () => {
    const estimate = estimateRoom(new Float32Array(48000), 48000);
    expect('volume' in estimate).toBe(false);
    const config = masteringRepairDereverbConfigForRoom({ estimate, lateDelayMs: 33 });
    expect(config.lateDelayMs).toBeCloseTo(33, 5);
  });
});
