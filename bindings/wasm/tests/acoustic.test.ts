/**
 * Geometric room-acoustics tests for the WASM binding. Mirrors the Node /
 * Python synthesizeRir / estimateRoom / roomMorph surfaces.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { estimateRoom, init, roomMorph, synthesizeRir } from '../src/index';

beforeAll(async () => {
  await init();
});

describe('geometric room acoustics', () => {
  it('synthesizes a decaying RIR from geometry', () => {
    const result = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    expect(result.hasError).toBe(false);
    expect(result.sampleRate).toBe(48000);
    expect(result.rir.length).toBeGreaterThan(0);
    expect(Array.from(result.rir).some((s) => Math.abs(s) > 0)).toBe(true);
  });

  it('flags invalid geometry with an empty RIR', () => {
    const result = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, sourceX: 99 });
    expect(result.hasError).toBe(true);
    expect(result.rir.length).toBe(0);
  });

  it('round-trips a known shoebox within tolerance', () => {
    const rir = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const est = estimateRoom(rir.rir, 48000, {
      aspectHintLw: 7 / 5,
      aspectHintLh: 7 / 3,
      referenceAbsorption: 0.15,
    });
    expect(est.volume).toBeGreaterThan(105 * 0.8);
    expect(est.volume).toBeLessThan(105 * 1.2);
    expect(est.confidence).toBeGreaterThan(0);
    expect(est.rt60Bands.length).toBeGreaterThanOrEqual(4);
    expect(Number.isFinite(est.drrDb)).toBe(true);
  });

  it('morphs toward a target room and is deterministic', () => {
    const samples = new Float32Array(4000);
    samples[0] = 1.0;
    const opts = { lengthM: 12, widthM: 9, heightM: 5, absorption: 0.08, wet: 0.7 };
    const a = roomMorph(samples, 48000, opts);
    const b = roomMorph(samples, 48000, opts);
    expect(a.length).toBeGreaterThan(samples.length);
    expect(Array.from(a)).toEqual(Array.from(b));
  });

  it('rejects out-of-range sample rates', () => {
    expect(() => synthesizeRir({ sampleRate: 0 })).toThrow();
    expect(() => synthesizeRir({ sampleRate: 500000 })).toThrow();
    const samples = new Float32Array(1000);
    samples[0] = 1.0;
    expect(() => estimateRoom(samples, 0)).toThrow();
    expect(() => roomMorph(samples, 500000, { lengthM: 5, widthM: 4, heightM: 3 })).toThrow();
  });

  it('rejects empty and non-finite input buffers', () => {
    expect(() => estimateRoom(new Float32Array(0), 48000)).toThrow();
    const bad = new Float32Array(1000);
    bad[10] = Number.NaN;
    expect(() => estimateRoom(bad, 48000)).toThrow();
    expect(() => roomMorph(bad, 48000, { lengthM: 5, widthM: 4, heightM: 3 })).toThrow();
  });

  it('honors the late-tail model selector', () => {
    const base = { lengthM: 7, widthM: 5, heightM: 3, absorption: 0.4, maxSeconds: 0.3 };
    const sabine = synthesizeRir({ ...base, preferEyring: false });
    const eyring = synthesizeRir({ ...base, preferEyring: true });
    expect(Array.from(sabine.rir)).not.toEqual(Array.from(eyring.rir));
  });

  it('honors per-band wall scattering', () => {
    const base = {
      lengthM: 7,
      widthM: 5,
      heightM: 3,
      bandAbsorption: new Float32Array([0.2, 0.22, 0.24, 0.26]),
      maxSeconds: 0.3,
      seed: 123,
    };
    const mirror = synthesizeRir({ ...base, bandScattering: new Float32Array([0, 0, 0, 0]) });
    const diffuse = synthesizeRir({
      ...base,
      bandScattering: new Float32Array([0.8, 0.8, 0.8, 0.8]),
    });
    expect(Array.from(diffuse.rir)).not.toEqual(Array.from(mirror.rir));
  });

  it('emits absorption and rt60 bands at the same length', () => {
    const rir = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.15 });
    const est = estimateRoom(rir.rir, 48000);
    expect(est.absorptionBands.length).toBe(est.rt60Bands.length);
  });
});
