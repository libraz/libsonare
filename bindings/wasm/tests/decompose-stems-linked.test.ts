/**
 * `decomposeStemsLinked`: the multi-channel form of `decomposeStems` that
 * shares one NMF model and one soft mask across every channel.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { decomposeStems, decomposeStemsLinked, init } from '../dist/index.js';
import { expectRefusalOf } from './_helpers';

const SAMPLE_RATE = 22050;
const FRAMES = 8192;

function tone(freqHz: number, frames = FRAMES, sampleRate = SAMPLE_RATE): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = 0.5 * Math.sin((2 * Math.PI * freqHz * i) / sampleRate);
  }
  return out;
}

describe('decomposeStemsLinked', () => {
  beforeAll(async () => {
    await init();
  });

  it('carries the same sample rate through mono and linked stems', () => {
    const samples = tone(440);
    const options = {
      sampleRate: SAMPLE_RATE,
      nComponents: 2,
      nIter: 8,
      nFft: 512,
      hopLength: 128,
    };
    const mono = decomposeStems({ samples, ...options });
    const linked = decomposeStemsLinked({ channels: [samples], ...options });

    expect(mono.sampleRate).toBe(SAMPLE_RATE);
    expect(linked.sampleRate).toBe(SAMPLE_RATE);
    expect(linked.components.map((component) => component[0])).toEqual(mono.components);
  });

  it('reproduces decomposeStems bit for bit for a single channel', () => {
    const samples = tone(440);
    const mono = decomposeStems({ samples, sampleRate: SAMPLE_RATE, nComponents: 3 });
    const linked = decomposeStemsLinked({
      channels: [samples],
      sampleRate: SAMPLE_RATE,
      nComponents: 3,
    });

    expect(linked.components).toHaveLength(mono.components.length);
    for (let k = 0; k < mono.components.length; k++) {
      expect(linked.components[k]).toHaveLength(1);
      expect(linked.components[k][0]).toEqual(mono.components[k]);
    }
    expect(linked.w).toEqual(mono.w);
    expect(linked.h).toEqual(mono.h);
    expect(linked.sampleRate).toBe(mono.sampleRate);
  });

  it('shapes a two-channel result as components[component][channel] and reconstructs each channel', () => {
    const left = tone(440);
    const right = tone(660);
    const result = decomposeStemsLinked({
      channels: [left, right],
      sampleRate: SAMPLE_RATE,
      nComponents: 4,
    });

    const nComponents = result.components.length;
    expect(nComponents).toBe(4);
    for (const component of result.components) {
      expect(component).toHaveLength(2);
      expect(component[0]).toHaveLength(left.length);
      expect(component[1]).toHaveLength(right.length);
    }

    for (let channel = 0; channel < 2; channel++) {
      const input = channel === 0 ? left : right;
      const reconstructed = new Float32Array(input.length);
      for (const component of result.components) {
        const signal = component[channel];
        for (let i = 0; i < signal.length; i++) {
          reconstructed[i] += signal[i];
        }
      }
      let signalEnergy = 0;
      let errorEnergy = 0;
      for (let i = 0; i < input.length; i++) {
        signalEnergy += input[i] * input[i];
        const diff = input[i] - reconstructed[i];
        errorEnergy += diff * diff;
      }
      const snrDb = 10 * Math.log10(signalEnergy / Math.max(errorEnergy, 1e-20));
      expect(snrDb).toBeGreaterThan(40);
    }
  });

  it('rejects a null/empty/undersized channel set and mismatched channel lengths', () => {
    // biome-ignore lint/suspicious/noExplicitAny: deliberately invalid input
    const missingChannels = undefined as any;
    expect(() =>
      decomposeStemsLinked({ channels: missingChannels, sampleRate: SAMPLE_RATE }),
    ).toThrow();
    expect(() => decomposeStemsLinked({ channels: [], sampleRate: SAMPLE_RATE })).toThrow();
    expect(() =>
      decomposeStemsLinked({
        channels: [new Float32Array(FRAMES), new Float32Array(FRAMES / 2)],
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrow();
  });

  it('refuses a missing channel set by type and an empty one by domain', () => {
    expect(
      expectRefusalOf(
        () =>
          decomposeStemsLinked({
            channels: undefined as unknown as Float32Array[],
            sampleRate: SAMPLE_RATE,
          }),
        TypeError,
      ).message,
    ).toBe('decomposeStemsLinked: channels must be an array of Float32Array');
    expect(
      expectRefusalOf(() => decomposeStemsLinked({ channels: [], sampleRate: SAMPLE_RATE }))
        .message,
    ).toBe('decomposeStemsLinked: channels must not be empty');
  });

  it('refuses an nFft no COLA geometry has, on both entry points', () => {
    const samples = tone(440);
    for (const nFft of [2047, 2, 1 << 20]) {
      expect(
        expectRefusalOf(() => decomposeStems({ samples, sampleRate: SAMPLE_RATE, nFft })).message,
      ).toBe('decomposeStems: nFft must be an even integer in [4, 524288]');
      expect(
        expectRefusalOf(() =>
          decomposeStemsLinked({ channels: [samples], sampleRate: SAMPLE_RATE, nFft }),
        ).message,
      ).toBe('decomposeStemsLinked: nFft must be an even integer in [4, 524288]');
    }
  });

  it('refuses an iteration count below 1, on both entry points', () => {
    const samples = tone(440);
    for (const nIter of [0, -1]) {
      expect(
        expectRefusalOf(() => decomposeStems({ samples, sampleRate: SAMPLE_RATE, nIter })).message,
      ).toBe('decomposeStems: nIter must be an integer in [1, 2147483647]');
      expect(
        expectRefusalOf(() =>
          decomposeStemsLinked({ channels: [samples], sampleRate: SAMPLE_RATE, nIter }),
        ).message,
      ).toBe('decomposeStemsLinked: nIter must be an integer in [1, 2147483647]');
    }
  });

  it('rejects an invalid option the same way decomposeStems does', () => {
    const channels = [tone(440)];
    expect(() =>
      decomposeStemsLinked({ channels, sampleRate: SAMPLE_RATE, nComponents: -1 }),
    ).toThrow();
    expect(() =>
      decomposeStemsLinked({ channels, sampleRate: SAMPLE_RATE, maskPower: 0.5 }),
    ).toThrow();
  });
});
