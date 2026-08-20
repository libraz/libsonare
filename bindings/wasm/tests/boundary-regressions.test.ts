import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  Mixer,
  meteringStereoCorrelation,
  mixingScenePresetJson,
  mixStereo,
} from '../src/index';
import { getSonareModule } from '../src/module_state';
import {
  BLOCK,
  configureDynamicEq,
  INVALID_DRAIN_COUNTS,
  INVERSE_OVERFLOW_SHAPES,
  rms,
  SR,
  sine,
} from './_boundary_fixtures.mjs';

describe('boundary regressions (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('rejects inverse matrix overflow shapes before copying or entering the core', () => {
    const module = getSonareModule();
    const tiny = new Float32Array(1);
    for (const [rows, frames] of INVERSE_OVERFLOW_SHAPES) {
      expect(() => module.melToStft(tiny, rows, frames, SR, 1024, 0, 0, false)).toThrow(
        /matrix shape exceeds WASM budget/,
      );
    }

    const budget = 64 * 1024 * 1024;
    expect(() => module.melToStft(tiny, budget - 1, 1, SR, 2, 0, 0, false)).toThrow(
      /length must equal/,
    );
    expect(() => module.melToStft(tiny, budget, 1, SR, 2, 0, 0, false)).toThrow(
      /length must equal/,
    );
    expect(() => module.melToStft(tiny, budget + 1, 1, SR, 2, 0, 0, false)).toThrow(
      /matrix shape exceeds WASM budget/,
    );

    const valid = module.melToStft(new Float32Array([1]), 1, 1, SR, 2, 0, 0, false);
    expect(valid.power).toBeInstanceOf(Float32Array);
    expect(valid.nFrames).toBe(1);
  });

  it('rejects an oversized array-like length before allocation and keeps the module usable', () => {
    const module = getSonareModule();
    const oversized = { length: 64 * 1024 * 1024 + 1 } as unknown as Float32Array;
    expect(() => module.meteringPeakDb(oversized, SR)).toThrow(/WASM Float32 input budget/);

    // Each plane is individually below the limit. The pair exceeds it by one;
    // fake array-like inputs ensure a vector copy would be observable as a JS
    // TypedArray.set failure (and a large allocation) if preflight ran late.
    const first = { length: 32 * 1024 * 1024 } as unknown as Float32Array;
    const second = { length: 32 * 1024 * 1024 + 1 } as unknown as Float32Array;
    expect(() => module.masterAudioStereo('pop', first, second, SR, null)).toThrow(
      /WASM Float32 input budget/,
    );
    expect(module.meteringPeakDb(new Float32Array([0.5]), SR)).toBeCloseTo(-6.0206, 3);
  });

  it('keeps the active sidechain pointer array alive and preserves state after a failed update', () => {
    const module = getSonareModule();
    const actual = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    const expected = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    const withoutSidechain = module.createEqualizer({ sampleRate: SR, maxBlockSize: BLOCK });
    try {
      configureDynamicEq(actual);
      configureDynamicEq(expected);
      configureDynamicEq(withoutSidechain);
      const key = sine(0.8);
      expected.setSidechainStereo(key, key);
      const expectedOutputs = [
        expected.processMono(sine(0.005)),
        expected.processMono(sine(0.005)),
      ];
      const dryOut = withoutSidechain.processMono(sine(0.005));
      expect(rms(expectedOutputs[0])).toBeLessThan(rms(dryOut) * 0.8);

      actual.setSidechainStereo(key, key);

      expect(() => actual.setSidechainStereo(key, new Float32Array(BLOCK / 2))).toThrow(
        /lengths must match/,
      );
      // Exercise other raw native frames before processing; the borrowed outer
      // pointer array must remain valid regardless of stack reuse.
      for (let i = 0; i < 64; i++) {
        module.mfccToMel(new Float32Array([1]), 1, 1, 1, 0);
      }
      const actualOutputs = [actual.processMono(sine(0.005)), actual.processMono(sine(0.005))];
      expect(actualOutputs.map((output) => Array.from(output))).toEqual(
        expectedOutputs.map((output) => Array.from(output)),
      );
    } finally {
      actual.delete();
      expected.delete();
      withoutSidechain.delete();
    }
  });

  it('revalidates stereo meters natively when validate is false', () => {
    const invalid = sine(0.5);
    invalid[17] = Number.NaN;
    expect(() => meteringStereoCorrelation(invalid, sine(0.5), SR)).toThrow();
    expect(() => meteringStereoCorrelation(invalid, sine(0.5), SR, { validate: false })).toThrow();
    const invalidRight = sine(0.5);
    invalidRight[23] = Number.POSITIVE_INFINITY;
    expect(() =>
      meteringStereoCorrelation(sine(0.5), invalidRight, SR, { validate: false }),
    ).toThrow();
    expect(() =>
      meteringStereoCorrelation(sine(0.5), sine(0.5), 7999, { validate: false }),
    ).toThrow();
    expect(() =>
      meteringStereoCorrelation(sine(0.5), sine(0.5), 384001, { validate: false }),
    ).toThrow();
    expect(meteringStereoCorrelation(sine(0.5), sine(0.5), SR, { validate: false })).toBeCloseTo(
      1,
      5,
    );
  });

  it('validates drainTailStereo before allocation at both facade and raw boundaries', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('vocalReverbSend'), SR, BLOCK);
    try {
      for (const invalid of INVALID_DRAIN_COUNTS) {
        expect(() => mixer.drainTailStereo(invalid)).toThrow();
      }
      expect(mixer.drainTailStereo(BLOCK - 1).left.length).toBe(BLOCK - 1);
      expect(mixer.drainTailStereo(BLOCK).left.length).toBe(BLOCK);
    } finally {
      mixer.delete();
    }

    const raw = getSonareModule().createMixerFromSceneJson(
      mixingScenePresetJson('vocalReverbSend'),
      SR,
      BLOCK,
    );
    try {
      for (const invalid of INVALID_DRAIN_COUNTS) {
        expect(() => raw.drainTailStereo(invalid)).toThrow();
      }
      expect(raw.drainTailStereo(BLOCK - 1).left.length).toBe(BLOCK - 1);
      expect(raw.drainTailStereo(BLOCK).left.length).toBe(BLOCK);
    } finally {
      raw.delete();
    }
  });

  it('propagates invalid one-shot strip settings and recovers on the next call', () => {
    const channel = sine(0.25);
    expect(() => mixStereo([channel], [channel], SR, { faderDb: Number.NaN })).toThrow();
    expect(() =>
      mixStereo([channel], [channel], SR, { inputTrimDb: Number.POSITIVE_INFINITY }),
    ).toThrow();
    expect(() =>
      mixStereo([channel], [channel], SR, { width: Number.NEGATIVE_INFINITY }),
    ).toThrow();
    expect(() =>
      mixStereo([channel], [channel], SR, { pan: 0, panMode: 'unknown' as never }),
    ).toThrow(/unknown mixing pan mode/);
    expect(() => mixStereo([channel], [channel], SR, { pan: 0, panMode: 99 })).toThrow();
    expect(mixStereo([channel], [channel], SR).left.length).toBe(BLOCK);
  });
});
