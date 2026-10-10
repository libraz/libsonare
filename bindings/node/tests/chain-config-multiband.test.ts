import { describe, expect, it } from 'vitest';
import { flattenChainConfig } from '../src/_chain_config.js';
import {
  type MasteringChainConfig,
  masterAudio,
  masteringChain,
  masteringChainStereo,
  StreamingMasteringChain,
} from '../src/index.js';
import { addon } from '../src/native.js';

type StreamingConfig = ConstructorParameters<typeof StreamingMasteringChain>[0];

/* biome-ignore lint/suspicious/noExplicitAny: the addon is untyped here on purpose. */
const native = addon as any;

const SAMPLE_RATE = 44100;
const FRAMES = 11025;

function tone(seed: number): Float32Array {
  const out = new Float32Array(FRAMES);
  for (let i = 0; i < FRAMES; i++) {
    const t = i / SAMPLE_RATE;
    out[i] =
      0.3 * Math.sin(2 * Math.PI * 100 * seed * t) +
      0.2 * Math.sin(2 * Math.PI * 1000 * seed * t) +
      0.1 * Math.sin(2 * Math.PI * 5000 * t);
  }
  return out;
}

const TYPED: MasteringChainConfig = {
  dynamics: {
    multibandComp: {
      enabled: true,
      crossover: { cutoffsHz: [200, 2000, 8000], slope: 'lr8' },
      bands: [
        { thresholdDb: -30, ratio: 4 },
        { thresholdDb: -28, ratio: 3, attackMs: 5 },
        { thresholdDb: -26, ratio: 2 },
        { thresholdDb: -20, ratio: 6, detector: 'logRms' },
      ],
    },
  },
};

const P = 'dynamics.multibandComp';
const FLAT: Record<string, number | boolean | string> = {
  [`${P}.enabled`]: true,
  [`${P}.crossover.cutoffsHz.0`]: 200,
  [`${P}.crossover.cutoffsHz.1`]: 2000,
  [`${P}.crossover.cutoffsHz.2`]: 8000,
  [`${P}.crossover.slope`]: 'lr8',
  [`${P}.bands.0.thresholdDb`]: -30,
  [`${P}.bands.0.ratio`]: 4,
  [`${P}.bands.1.thresholdDb`]: -28,
  [`${P}.bands.1.ratio`]: 3,
  [`${P}.bands.1.attackMs`]: 5,
  [`${P}.bands.2.thresholdDb`]: -26,
  [`${P}.bands.2.ratio`]: 2,
  [`${P}.bands.3.thresholdDb`]: -20,
  [`${P}.bands.3.ratio`]: 6,
  [`${P}.bands.3.detector`]: 'logRms',
};

function same(a: Float32Array, b: Float32Array): boolean {
  return (
    Buffer.compare(
      Buffer.from(a.buffer, a.byteOffset, a.byteLength),
      Buffer.from(b.buffer, b.byteOffset, b.byteLength),
    ) === 0
  );
}

describe('typed multiband crossover + bands', () => {
  it('flattens arrays as <path>.<index> and names enums on nested paths', () => {
    const flat = flattenChainConfig(TYPED);
    expect(flat[`${P}.crossover.cutoffsHz.2`]).toBe(8000);
    expect(flat[`${P}.bands.3.ratio`]).toBe(6);
    expect(flat).toEqual(flattenChainConfig(FLAT));
    // Enum names resolved to numbers, on the crossover and on a band detector.
    expect(typeof flat[`${P}.crossover.slope`]).toBe('number');
    expect(typeof flat[`${P}.bands.3.detector`]).toBe('number');
  });

  it('renders typed and flat spellings bit-identically (offline mono)', () => {
    const x = tone(1);
    const typed = masteringChain(x, SAMPLE_RATE, TYPED);
    const flat = masteringChain(x, SAMPLE_RATE, FLAT as unknown as MasteringChainConfig);
    expect(same(typed.samples, flat.samples)).toBe(true);
    // Control: the stage is consumed, so dropping the bands changes the output.
    const bare = masteringChain(x, SAMPLE_RATE, {
      dynamics: { multibandComp: { enabled: true, crossover: { cutoffsHz: [200, 2000, 8000] } } },
    });
    expect(same(typed.samples, bare.samples)).toBe(false);
  });

  it('renders typed and flat spellings bit-identically (offline stereo)', () => {
    const l = tone(1);
    const r = tone(1.5);
    const typed = masteringChainStereo(l, r, SAMPLE_RATE, TYPED);
    const flat = masteringChainStereo(l, r, SAMPLE_RATE, FLAT as unknown as MasteringChainConfig);
    expect(same(typed.left, flat.left)).toBe(true);
    expect(same(typed.right, flat.right)).toBe(true);
  });

  it('applies the typed form as masterAudio overrides on a 3-band preset base', () => {
    const x = tone(1);
    const typed = masterAudio(x, SAMPLE_RATE, 'pop', TYPED);
    const flat = masterAudio(x, SAMPLE_RATE, 'pop', FLAT as unknown as MasteringChainConfig);
    expect(same(typed.samples, flat.samples)).toBe(true);
  });

  it('runs identically through StreamingMasteringChain', () => {
    const x = tone(1);
    const run = (config: MasteringChainConfig): Float32Array => {
      const chain = new StreamingMasteringChain(config as StreamingConfig);
      chain.prepare(SAMPLE_RATE, 512, 1);
      const out = new Float32Array(FRAMES);
      for (let at = 0; at + 512 <= FRAMES; at += 512) {
        out.set(chain.processMono(x.slice(at, at + 512)), at);
      }
      return out;
    };
    const typed = run(TYPED);
    expect(same(typed, run(FLAT as unknown as MasteringChainConfig))).toBe(true);
    expect(same(typed, run({}))).toBe(false);
  });

  it('enables the stage from a typed crossover alone', () => {
    const x = tone(1);
    const typed = masteringChain(x, SAMPLE_RATE, {
      dynamics: { multibandComp: { crossover: { cutoffsHz: [200, 2000, 8000] } } },
    });
    const baseline = masteringChain(x, SAMPLE_RATE, {});
    expect(typed.stages.length).toBeGreaterThan(baseline.stages.length);
  });

  it('refuses an empty array by path on the facade flattener and the addon', () => {
    const empty = { dynamics: { multibandComp: { crossover: { cutoffsHz: [] } } } };
    const message = `Mastering override '${P}.crossover.cutoffsHz' is an empty list, which has no flat spelling`;
    expect(() => flattenChainConfig(empty)).toThrow(message);
    expect(() => masteringChain(tone(1), SAMPLE_RATE, empty as MasteringChainConfig)).toThrow(
      message,
    );
    expect(() => new StreamingMasteringChain(empty as StreamingConfig)).toThrow(message);
    expect(() => new native.StreamingMasteringChain(empty)).toThrow(message);
  });
});

describe('addon streaming flattener recurses arrays', () => {
  it('takes a nested array config as the same chain as its flat keys', () => {
    const x = tone(1);
    const run = (config: unknown): Float32Array => {
      const chain = new native.StreamingMasteringChain(config);
      chain.prepare(SAMPLE_RATE, 512, 1);
      const out = new Float32Array(FRAMES);
      for (let at = 0; at + 512 <= FRAMES; at += 512) {
        out.set(chain.processMono(x.slice(at, at + 512)), at);
      }
      return out;
    };
    const numericFlat = flattenChainConfig(TYPED);
    const nested = {
      dynamics: {
        multibandComp: {
          enabled: true,
          crossover: {
            cutoffsHz: [200, 2000, 8000],
            slope: numericFlat[`${P}.crossover.slope`],
          },
          bands: [
            { thresholdDb: -30, ratio: 4 },
            { thresholdDb: -28, ratio: 3, attackMs: 5 },
            { thresholdDb: -26, ratio: 2 },
            { thresholdDb: -20, ratio: 6, detector: numericFlat[`${P}.bands.3.detector`] },
          ],
        },
      },
    };
    expect(same(run(nested), run(numericFlat))).toBe(true);
  });
});
