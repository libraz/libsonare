/**
 * An offline run's channel count is refused against the channels the engine was prepared with.
 *
 * primeOfflineParameters, renderOffline, bounceOffline and freezeOffline read the count after the
 * prepared-state check and refuse it outside [1, prepared channels] before any layout, block-size
 * or size check; Node and WASM carry this same table and the same texts as the C ABI.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

type Run = (engine: RealtimeEngine) => unknown;

interface Case {
  /** Channels the engine is prepared with. */
  prepared: number;
  label: string;
  run: Run;
  /** The refusal text after the method prefix; omitted for a call that must succeed. */
  refusal?: string;
}

const planes = (count: number): Float32Array[] =>
  Array.from({ length: count }, () => new Float32Array(16));
const prime =
  (numChannels: number, blockSize?: number): Run =>
  (engine) =>
    engine.primeOfflineParameters(numChannels, blockSize);
const render =
  (count: number, blockSize?: number): Run =>
  (engine) =>
    engine.renderOffline({ channels: planes(count), blockSize });
const bounce =
  (options: Record<string, unknown>): Run =>
  (engine) =>
    engine.bounceOffline({ totalFrames: 16, numChannels: 2, ...options });
const freeze =
  (options: Record<string, unknown>): Run =>
  (engine) =>
    engine.freezeOffline({ totalFrames: 16, numChannels: 2, ...options });

const range = (hi: number) => `numChannels must be an integer in [1, ${hi}]`;
const layout = 'numChannels must be one of 1, 2, 6, 8';
const block = 'blockSize must be an integer in [1, 128]';

const CASES: Case[] = [
  ...[0, 65, 1000].flatMap((n): Case[] => [
    { prepared: 64, label: `prime ${n}`, run: prime(n), refusal: range(64) },
    { prepared: 64, label: `bounce ${n}`, run: bounce({ numChannels: n }), refusal: range(64) },
    { prepared: 64, label: `freeze ${n}`, run: freeze({ numChannels: n }), refusal: range(64) },
  ]),
  { prepared: 64, label: 'render 65', run: render(65), refusal: range(64) },
  { prepared: 64, label: 'render 0', run: render(0), refusal: 'channels must not be empty' },
  ...[0, 3].flatMap((n): Case[] => [
    { prepared: 2, label: `prime ${n}`, run: prime(n), refusal: range(2) },
    { prepared: 2, label: `bounce ${n}`, run: bounce({ numChannels: n }), refusal: range(2) },
    { prepared: 2, label: `freeze ${n}`, run: freeze({ numChannels: n }), refusal: range(2) },
  ]),
  { prepared: 2, label: 'render 3', run: render(3), refusal: range(2) },
  { prepared: 2, label: 'prime 2', run: prime(2) },
  { prepared: 2, label: 'render 2', run: render(2) },
  { prepared: 8, label: 'bounce 3', run: bounce({ numChannels: 3 }), refusal: layout },
  {
    prepared: 64,
    label: 'bounce 65 is a range, not a layout',
    run: bounce({ numChannels: 65 }),
    refusal: range(64),
  },
  { prepared: 8, label: 'freeze 3', run: freeze({ numChannels: 3 }) },
  { prepared: 8, label: 'prime 3', run: prime(3) },
  { prepared: 64, label: 'prime block', run: prime(2, 1_000_000), refusal: block },
  { prepared: 64, label: 'render block', run: render(2, 1_000_000), refusal: block },
  { prepared: 64, label: 'bounce block', run: bounce({ blockSize: 1_000_000 }), refusal: block },
  { prepared: 64, label: 'freeze block', run: freeze({ blockSize: 1_000_000 }), refusal: block },
  {
    prepared: 64,
    label: 'prime channels before block',
    run: prime(65, 1_000_000),
    refusal: range(64),
  },
  {
    prepared: 64,
    label: 'render channels before block',
    run: render(65, 1_000_000),
    refusal: range(64),
  },
  {
    prepared: 64,
    label: 'bounce frames',
    run: bounce({ totalFrames: 0 }),
    refusal: 'totalFrames must be an integer of at least 1',
  },
  {
    prepared: 64,
    label: 'bounce size',
    run: bounce({ totalFrames: 1e12 }),
    refusal:
      'totalFrames, numChannels, sourceSampleRate and targetSampleRate give a result larger than the supported size',
  },
  {
    prepared: 64,
    label: 'bounce source rate',
    run: bounce({ sourceSampleRate: 44100 }),
    refusal: 'sourceSampleRate must equal the prepared sample rate 48000',
  },
  {
    prepared: 64,
    label: 'freeze frames',
    run: freeze({ totalFrames: 0 }),
    refusal: 'totalFrames must be an integer of at least 1',
  },
  {
    prepared: 64,
    label: 'freeze size',
    run: freeze({ totalFrames: 4e9, numChannels: 64 }),
    refusal: 'totalFrames times numChannels exceeds the supported size',
  },
  {
    prepared: 64,
    label: 'freeze startPpq',
    run: freeze({ startPpq: -1 }),
    refusal: 'startPpq must be finite and at least 0',
  },
  {
    prepared: 64,
    label: 'freeze startPpq NaN',
    run: freeze({ startPpq: Number.NaN }),
    refusal: 'startPpq must be finite',
  },
  {
    prepared: 64,
    label: 'freeze gain',
    run: freeze({ gain: -1 }),
    refusal: 'gain must be finite and at least 0',
  },
  {
    prepared: 64,
    label: 'freeze channels before startPpq',
    run: freeze({ numChannels: 0, startPpq: -1 }),
    refusal: range(64),
  },
];

const METHOD: Record<string, string> = {
  prime: 'primeOfflineParameters',
  render: 'renderOffline',
  bounce: 'bounceOffline',
  freeze: 'freezeOffline',
};

function makeEngine(maxChannels: number): RealtimeEngine {
  return new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128, maxChannels });
}

function checkCase(c: Case): void {
  const engine = makeEngine(c.prepared);
  try {
    if (c.refusal === undefined) {
      expect(() => c.run(engine)).not.toThrow();
    } else {
      const method = METHOD[c.label.split(' ')[0]];
      let caught: unknown;
      try {
        c.run(engine);
      } catch (error) {
        caught = error;
      }
      expect(caught).toBeInstanceOf(RangeError);
      expect((caught as Error).message).toBe(`RealtimeEngine.${method}: ${c.refusal}`);
    }
  } finally {
    engine.destroy();
  }
}

describe('offline channel-count refusals (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  for (const c of CASES) {
    it(`${c.prepared}-channel engine: ${c.label}`, () => checkCase(c));
  }

  it('refuses a freeze past the public timeline, as setClips does', () => {
    checkCase({
      prepared: 2,
      label: 'freeze startPpq past the timeline',
      run: freeze({ startPpq: 1e300 }),
      refusal: 'startPpq must be finite and in [0, 1e12]',
    });
  });

  it('refuses each zero-copy plane count by name', () => {
    const engine = makeEngine(2);
    try {
      for (const method of ['prepareChannels', 'prepareMonitorChannels'] as const) {
        expect(() => engine[method](0, 128)).toThrow(
          new RangeError(`RealtimeEngine.${method}: numChannels must be an integer in [1, 64]`),
        );
        expect(() => engine[method](65, 128)).toThrow(
          new RangeError(`RealtimeEngine.${method}: numChannels must be an integer in [1, 64]`),
        );
        expect(() => engine[method](2, 0)).toThrow(
          new RangeError(
            `RealtimeEngine.${method}: maxFrames must be an integer in [1, 2147483647]`,
          ),
        );
      }
    } finally {
      engine.destroy();
    }
  });
});
