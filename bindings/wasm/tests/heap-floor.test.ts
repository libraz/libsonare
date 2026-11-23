import { describe, expect, it } from 'vitest';
import createModule from '../dist/sonare.js';
import { binsPerHop, MEASURED_FLOORS } from '../src/internal/memory_budget';

const SR = 48000;

function signal(seconds: number): Float32Array {
  const samples = new Float32Array(Math.round(SR * seconds));
  let state = 0x12345678;
  for (let i = 0; i < samples.length; i++) {
    state ^= state << 13;
    state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5;
    state >>>= 0;
    samples[i] = 0.5 * Math.exp(-((i % 24000) / 12000) * 3) * (state / 4294967296 - 0.5);
  }
  return samples;
}

type Native = Awaited<ReturnType<typeof createModule>>;

// The break a call moves on a module of its own, per input byte: a high-water no earlier
// call's freed memory can absorb.
async function heldPerInputByte(samples: Float32Array, call: (m: Native) => unknown) {
  const module = await createModule();
  const before = module.sonareHeapHeadroom(0);
  call(module);
  return (before - module.sonareHeapHeadroom(0)) / (4 * samples.length);
}

// A floor stated above what the call holds would refuse calls that fit, so each floor is
// held below a fresh measurement; a lowered peak turns its case red until it is re-measured.
describe('each measured heap floor stays below what the call holds', () => {
  const samples = signal(3);

  it('timeStretch', async () => {
    for (const [speedRatio, nFft, hop] of [
      [8, 2048, 1024],
      [0.5, 2048, 512],
      [0.8, 2048, 256],
    ]) {
      const held = await heldPerInputByte(samples, (m) =>
        m.timeStretch(samples, SR, speedRatio, nFft, hop),
      );
      expect(held).toBeGreaterThanOrEqual(
        MEASURED_FLOORS.timeStretch(binsPerHop(nFft, hop), 1 / speedRatio),
      );
    }
  });

  it('pitchShift', async () => {
    for (const [semitones, nFft, hop] of [
      [-12, 2048, 1024],
      [12, 2048, 512],
    ]) {
      const held = await heldPerInputByte(samples, (m) =>
        m.pitchShift(samples, SR, semitones, nFft, hop),
      );
      expect(held).toBeGreaterThanOrEqual(
        MEASURED_FLOORS.pitchShift(binsPerHop(nFft, hop), 2 ** (semitones / 12)),
      );
    }
  });

  it('hpss', async () => {
    for (const [withResidual, nFft, hop] of [
      [false, 2048, 512],
      [true, 2048, 1024],
    ] as const) {
      const held = await heldPerInputByte(samples, (m) =>
        m.hpss(samples, SR, 31, 31, nFft, hop, false, withResidual),
      );
      expect(held).toBeGreaterThanOrEqual(
        MEASURED_FLOORS.hpss(binsPerHop(nFft, hop), withResidual),
      );
    }
  });

  it('decomposeStems', async () => {
    for (const [nComponents, nFft, hop] of [
      [1, 2048, 1024],
      [4, 2048, 1024],
    ]) {
      const held = await heldPerInputByte(samples, (m) =>
        m.decomposeStems(samples, SR, { nComponents, nFft, hopLength: hop }),
      );
      expect(held).toBeGreaterThanOrEqual(
        MEASURED_FLOORS.decomposeStems(binsPerHop(nFft, hop), nComponents),
      );
    }
  });
});
