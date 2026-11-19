import { describe, expect, it } from 'vitest';
import {
  ErrorCode,
  type MasteringChainConfig,
  masterAudio,
  masterAudioStereo,
  StreamingMasteringChain,
} from '../src/index.js';
import { addon } from '../src/native.js';

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
// The same chain spelled as dotted paths, one per leaf.
const DOTTED: Record<string, number | boolean | string> = {
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
const DOTTED_JSON = JSON.stringify({ version: 2, params: DOTTED });

const invalidFormat = (message: string) =>
  expect.objectContaining({
    code: ErrorCode.InvalidFormat,
    message: expect.stringContaining(message),
  });

function same(a: Float32Array, b: Float32Array): boolean {
  return (
    Buffer.compare(
      Buffer.from(a.buffer, a.byteOffset, a.byteLength),
      Buffer.from(b.buffer, b.byteOffset, b.byteLength),
    ) === 0
  );
}

describe('typed multiband crossover + bands', () => {
  it('takes the dotted spelling as the same chain as the nested one', () => {
    const x = tone(1);
    const typed = masterAudio({ samples: x, sampleRate: SAMPLE_RATE, overrides: TYPED });
    const dotted = masterAudio({
      samples: x,
      sampleRate: SAMPLE_RATE,
      overrides: DOTTED as MasteringChainConfig,
    });
    expect(same(typed.samples, dotted.samples)).toBe(true);
  });

  it('renders the typed form and the native dotted document bit-identically (offline mono)', () => {
    const x = tone(1);
    const typed = masterAudio({ samples: x, sampleRate: SAMPLE_RATE, overrides: TYPED });
    const flat = native.masteringChain(x, SAMPLE_RATE, DOTTED_JSON);
    expect(same(typed.samples, flat.samples)).toBe(true);
    // Control: the stage is consumed, so dropping the bands changes the output.
    const bare = masterAudio({
      samples: x,
      sampleRate: SAMPLE_RATE,
      overrides: {
        dynamics: { multibandComp: { enabled: true, crossover: { cutoffsHz: [200, 2000, 8000] } } },
      },
    });
    expect(same(typed.samples, bare.samples)).toBe(false);
  });

  it('renders the typed form and the native dotted document bit-identically (offline stereo)', () => {
    const l = tone(1);
    const r = tone(1.5);
    const typed = masterAudioStereo({
      left: l,
      right: r,
      sampleRate: SAMPLE_RATE,
      overrides: TYPED,
    });
    const flat = native.masteringChainStereo(l, r, SAMPLE_RATE, DOTTED_JSON);
    expect(same(typed.left, flat.left)).toBe(true);
    expect(same(typed.right, flat.right)).toBe(true);
  });

  it('applies the typed form as masterAudio overrides on a 3-band preset base', () => {
    const x = tone(1);
    const typed = masterAudio(x, SAMPLE_RATE, { preset: 'pop', overrides: TYPED });
    const flat = native.masterAudio('pop', x, SAMPLE_RATE, DOTTED_JSON);
    expect(same(typed.samples, flat.samples)).toBe(true);
  });

  it('runs identically through StreamingMasteringChain', () => {
    const x = tone(1);
    const run = (config: MasteringChainConfig): Float32Array => {
      const chain = new StreamingMasteringChain({
        sampleRate: SAMPLE_RATE,
        numChannels: 1,
        maxBlockSize: 512,
        ...config,
      });
      const out = new Float32Array(FRAMES);
      for (let at = 0; at + 512 <= FRAMES; at += 512) {
        out.set(chain.processMono(x.slice(at, at + 512)), at);
      }
      return out;
    };
    const typed = run(TYPED);
    const nativeChain = new native.StreamingMasteringChain(DOTTED_JSON);
    nativeChain.prepare(SAMPLE_RATE, 512, 1);
    const viaNative = new Float32Array(FRAMES);
    for (let at = 0; at + 512 <= FRAMES; at += 512) {
      viaNative.set(nativeChain.processMono(x.slice(at, at + 512)), at);
    }
    expect(same(typed, viaNative)).toBe(true);
    expect(same(typed, run({}))).toBe(false);
  });

  it('enables the stage from a typed crossover alone', () => {
    const x = tone(1);
    const typed = masterAudio({
      samples: x,
      sampleRate: SAMPLE_RATE,
      overrides: {
        dynamics: { multibandComp: { crossover: { cutoffsHz: [200, 2000, 8000] } } },
      },
    });
    const baseline = masterAudio({ samples: x, sampleRate: SAMPLE_RATE, overrides: {} });
    expect(typed.stages.length).toBeGreaterThan(baseline.stages.length);
  });

  it.each([
    ['lowCutoffHz', 200, 'crossover.cutoffsHz.0'],
    ['highCutoffHz', 4000, 'crossover.cutoffsHz.1'],
    ['lowThresholdDb', -20, 'bands.0.thresholdDb'],
    ['midRatio', 3, 'bands.1.ratio'],
    ['highReleaseMs', 80, 'bands.2.releaseMs'],
  ] as const)('refuses the retired %s shorthand and names %s', (key, value, current) => {
    const overrides = { dynamics: { multibandComp: { [key]: value } } } as never;
    expect(() => masterAudio({ samples: tone(1), sampleRate: SAMPLE_RATE, overrides })).toThrow(
      invalidFormat(
        `masterAudio: unknown key params.${P}.${key} (renamed to params.${P}.${current})`,
      ),
    );
  });

  it('refuses a number on a token path, naming the path', () => {
    const overrides = {
      dynamics: { multibandComp: { crossover: { cutoffsHz: [200, 2000], slope: 2 } } },
    } as never;
    expect(() => masterAudio({ samples: tone(1), sampleRate: SAMPLE_RATE, overrides })).toThrow(
      invalidFormat(`${P}.crossover.slope takes a token, not a number`),
    );
  });

  it('refuses an empty array by path on every entry point', () => {
    const empty = { dynamics: { multibandComp: { crossover: { cutoffsHz: [] } } } };
    const message = invalidFormat(`params.${P}.crossover.cutoffsHz must not be an empty list`);
    expect(() =>
      masterAudio({
        samples: tone(1),
        sampleRate: SAMPLE_RATE,
        overrides: empty as MasteringChainConfig,
      }),
    ).toThrow(message);
    expect(
      () =>
        new StreamingMasteringChain({
          sampleRate: SAMPLE_RATE,
          numChannels: 1,
          ...(empty as MasteringChainConfig),
        }),
    ).toThrow(message);
    expect(() => new native.StreamingMasteringChain(JSON.stringify({ params: empty }))).toThrow(
      message,
    );
  });
});

describe('the addon reads nested and dotted documents as one chain', () => {
  it('takes a nested array document as the same chain as its dotted paths', () => {
    const x = tone(1);
    const run = (configJson: string): Float32Array => {
      const chain = new native.StreamingMasteringChain(configJson);
      chain.prepare(SAMPLE_RATE, 512, 1);
      const out = new Float32Array(FRAMES);
      for (let at = 0; at + 512 <= FRAMES; at += 512) {
        out.set(chain.processMono(x.slice(at, at + 512)), at);
      }
      return out;
    };
    expect(same(run(JSON.stringify({ params: TYPED })), run(DOTTED_JSON))).toBe(true);
  });
});
