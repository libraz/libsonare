import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, meteringStereoCorrelation, mixingScenePreset, mixStereo } from '../src/index';
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
import { unknownKeyText } from './_helpers';

describe('boundary regressions (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('rejects inverse matrix overflow shapes before copying or entering the core', () => {
    const module = getSonareModule();
    const tiny = new Float32Array(1);
    for (const [rows, frames] of INVERSE_OVERFLOW_SHAPES) {
      expect(() => module.melToStft(tiny, rows, frames, SR, 1024, 0, 0, false)).toThrow(
        /matrix shape exceeds WASM budget|nFrames must be an integer in \[1, 4096\]/,
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
    expect(valid.magnitude).toBeInstanceOf(Float32Array);
    expect(valid.nFrames).toBe(1);
  });

  it('rejects non-finite inverse matrices in the core, not in the embind wrapper', () => {
    // The embind wrappers call the C++ core directly, so the C-ABI translation
    // unit's validation never runs here. These assert the core precondition
    // itself, reached with the facade's JS pre-check bypassed.
    const module = getSonareModule();

    const mel = new Float32Array(8 * 4).fill(0.1);
    mel[3] = Number.NaN;
    expect(() => module.melToStft(mel, 8, 4, SR, 256, 0, 0, false)).toThrow(
      /^melToStft: M contains a non-finite sample at index 3$/,
    );
    expect(() => module.melToAudio(mel, 8, 4, SR, 256, 64, 0, 0, 2, false)).toThrow(
      /^melToAudio: M contains a non-finite sample at index 3$/,
    );

    const mfcc = new Float32Array(5 * 4).fill(0.1);
    mfcc[7] = Number.POSITIVE_INFINITY;
    expect(() => module.mfccToMel(mfcc, 5, 4, 8, 0)).toThrow(
      /^mfccToMel: mfcc contains a non-finite sample at index 7$/,
    );
    expect(() => module.mfccToAudio(mfcc, 5, 4, 8, SR, 256, 64, 0, 0, 2, false, 0)).toThrow(
      /^mfccToAudio: mfcc contains a non-finite sample at index 7$/,
    );

    const magnitude = new Float32Array(3 * 5).fill(1);
    magnitude[4] = Number.NaN;
    expect(() => module.griffinLim(magnitude, 3, 5, SR, 4, 1, 1, 0)).toThrow(
      /griffinLim: magnitude contains a non-finite value/,
    );

    const cq = new Float32Array(12 * 4).fill(0.5);
    cq[5] = Number.NaN;
    expect(() => module.cqtToAudio(cq, 12, 4, 8000, 128, 130.8128, 12, 1)).toThrow(
      /griffinlim_cqt: magnitude contains a non-finite value/,
    );
    expect(() => module.vqtToAudio(cq, 12, 4, 8000, 128, 130.8128, 12, 24, 1)).toThrow(
      /griffinlim_vqt: magnitude contains a non-finite value/,
    );
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
      withoutSidechain.processMono(sine(0.005));
      const dryOut = withoutSidechain.processMono(sine(0.005));
      // Controls update on a fixed 64-sample grid, so the sidechain acts from the second block.
      expect(rms(expectedOutputs[1])).toBeLessThan(rms(dryOut) * 0.8);

      actual.setSidechainStereo(key, key);

      expect(() => actual.setSidechainStereo(key, new Float32Array(BLOCK / 2))).toThrow(
        /left and right must have the same length/,
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

  it('scans both stereo meter channels and refuses a validate key', () => {
    const invalid = sine(0.5);
    invalid[17] = Number.NaN;
    expect(() =>
      meteringStereoCorrelation({ left: invalid, right: sine(0.5), sampleRate: SR }),
    ).toThrow();
    expect(() =>
      meteringStereoCorrelation({
        left: invalid,
        right: sine(0.5),
        sampleRate: SR,
      }),
    ).toThrow();
    const invalidRight = sine(0.5);
    invalidRight[23] = Number.POSITIVE_INFINITY;
    expect(() =>
      meteringStereoCorrelation({
        left: sine(0.5),
        right: invalidRight,
        sampleRate: SR,
      }),
    ).toThrow();
    expect(() =>
      meteringStereoCorrelation({
        left: sine(0.5),
        right: sine(0.5),
        sampleRate: 7999,
      }),
    ).toThrow();
    expect(() =>
      meteringStereoCorrelation({
        left: sine(0.5),
        right: sine(0.5),
        sampleRate: 384001,
      }),
    ).toThrow();
    expect(
      meteringStereoCorrelation({
        left: sine(0.5),
        right: sine(0.5),
        sampleRate: SR,
      }),
    ).toBeCloseTo(1, 5);
    expect(() =>
      meteringStereoCorrelation({
        left: sine(0.5),
        right: sine(0.5),
        sampleRate: SR,
        validate: false,
      } as never),
    ).toThrow(unknownKeyText('meteringStereoCorrelation: unknown key validate'));
  });

  it('validates drainTailStereo before allocation at both facade and raw boundaries', () => {
    const mixer = Mixer.fromJson(JSON.stringify(mixingScenePreset('vocalReverbSend')), {
      sampleRate: SR,
      maxBlockSize: BLOCK,
    });
    try {
      for (const invalid of INVALID_DRAIN_COUNTS) {
        expect(() => mixer.drainTailStereo(invalid)).toThrow();
      }
      expect(mixer.drainTailStereo(BLOCK - 1).left.length).toBe(BLOCK - 1);
      expect(mixer.drainTailStereo(BLOCK).left.length).toBe(BLOCK);
    } finally {
      mixer.destroy();
    }

    const raw = getSonareModule().createMixerFromSceneJson(
      getSonareModule().mixingScenePresetJson('vocalReverbSend'),
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
    const one = (strip: Record<string, unknown>) =>
      mixStereo({ strips: [{ left: channel, right: channel, ...strip }], sampleRate: SR });
    expect(() => one({ faderDb: Number.NaN })).toThrow();
    expect(() => one({ inputTrimDb: Number.POSITIVE_INFINITY })).toThrow();
    expect(() => one({ width: Number.NEGATIVE_INFINITY })).toThrow();
    expect(() => one({ pan: 0, panMode: 'unknown' })).toThrow(/panMode must be one of/);
    expect(() => one({ pan: 0, panMode: 99 })).toThrow();
    expect(one({}).left.length).toBe(BLOCK);
  });
});
