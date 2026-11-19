import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  type MasteringChainConfig,
  masterAudio,
  masterAudioStereo,
  StreamingMasteringChain,
} from '../src/index';
import { getSonareModule } from '../src/module_state';

const sampleRate = 22_050;
const frames = sampleRate / 2;
const left = Float32Array.from(
  { length: frames },
  (_, i) =>
    0.3 * Math.sin((2 * Math.PI * 110 * i) / sampleRate) +
    0.2 * Math.sin((2 * Math.PI * 1500 * i) / sampleRate) +
    0.1 * Math.sin((2 * Math.PI * 6000 * i) / sampleRate),
);
const right = Float32Array.from(left, (value, i) => 0.8 * value + 0.05 * Math.sin(i * 0.01));

const MB = 'dynamics.multibandComp';

beforeAll(async () => init());

const typed: MasteringChainConfig = {
  dynamics: {
    multibandComp: {
      enabled: true,
      crossover: { cutoffsHz: [200, 1000, 4000], slope: 'lr8' },
      bands: [
        { thresholdDb: -24, ratio: 3 },
        { thresholdDb: -20, ratio: 2.5, detector: 'logRms' },
        { thresholdDb: -18, ratio: 2 },
        { thresholdDb: -16, ratio: 4, makeupGainDb: 1 },
      ],
    },
  },
};

/** The same chain spelled as dotted paths, one per leaf. */
const dotted = (): Record<string, unknown> => ({
  [`${MB}.enabled`]: true,
  [`${MB}.crossover.cutoffsHz.0`]: 200,
  [`${MB}.crossover.cutoffsHz.1`]: 1000,
  [`${MB}.crossover.cutoffsHz.2`]: 4000,
  [`${MB}.crossover.slope`]: 'lr8',
  [`${MB}.bands.0.thresholdDb`]: -24,
  [`${MB}.bands.0.ratio`]: 3,
  [`${MB}.bands.1.thresholdDb`]: -20,
  [`${MB}.bands.1.ratio`]: 2.5,
  [`${MB}.bands.1.detector`]: 'logRms',
  [`${MB}.bands.2.thresholdDb`]: -18,
  [`${MB}.bands.2.ratio`]: 2,
  [`${MB}.bands.3.thresholdDb`]: -16,
  [`${MB}.bands.3.ratio`]: 4,
  [`${MB}.bands.3.makeupGainDb`]: 1,
});
const dottedJson = (): string => JSON.stringify({ version: 2, params: dotted() });

const invalidFormat = (message: string) =>
  expect.objectContaining({
    code: ErrorCode.InvalidFormat,
    message: expect.stringContaining(message),
  });

describe('typed multiband crossover + bands', () => {
  it('masterAudio mono: nested equals the native dotted document bit-for-bit', () => {
    const a = masterAudio({ samples: left, sampleRate, overrides: typed });
    const b = getSonareModule().masteringChain(left, sampleRate, dottedJson());
    expect(a.stages).toContain(MB);
    expect(Array.from(a.samples)).toEqual(Array.from(b.samples));
  });

  it('masterAudioStereo: nested equals the native dotted document bit-for-bit', () => {
    const a = masterAudioStereo({ left, right, sampleRate, overrides: typed });
    const b = getSonareModule().masteringChainStereo(left, right, sampleRate, dottedJson());
    expect(Array.from(a.left)).toEqual(Array.from(b.left));
    expect(Array.from(a.right)).toEqual(Array.from(b.right));
  });

  it('masterAudio overrides differ from the preset alone', () => {
    const a = masterAudio({ samples: left, sampleRate, preset: 'pop', overrides: typed });
    const base = masterAudio({ samples: left, sampleRate, preset: 'pop' });
    expect(Array.from(a.samples)).not.toEqual(Array.from(base.samples));
  });

  it('takes the dotted spelling as the same chain as the nested one', () => {
    const overrides = dotted() as MasteringChainConfig;
    const a = masterAudio({ samples: left, sampleRate, overrides: typed });
    const b = masterAudio({ samples: left, sampleRate, overrides });
    expect(Array.from(a.samples)).toEqual(Array.from(b.samples));
  });

  it('StreamingMasteringChain: runs the nested config like the offline chain stage list', () => {
    const chain = new StreamingMasteringChain({
      sampleRate,
      numChannels: 1,
      maxBlockSize: 512,
      ...typed,
    });
    try {
      expect(chain.stageNames()).toContain(MB);
      const out: number[] = [];
      for (let at = 0; at < left.length; at += 512) {
        out.push(...chain.processMono(left.subarray(at, Math.min(at + 512, left.length))));
      }
      expect(out.length).toBe(left.length);
    } finally {
      chain.destroy();
    }
  });

  it('refuses a number on a token path, naming the path', () => {
    const numeric = {
      dynamics: {
        multibandComp: {
          crossover: { cutoffsHz: [200, 1000, 4000], slope: 3 },
        },
      },
    } as unknown as MasteringChainConfig;
    expect(() => masterAudio({ samples: left, sampleRate, overrides: numeric })).toThrow(
      invalidFormat(`${MB}.crossover.slope takes a token, not a number`),
    );
  });

  it('a typed crossover alone enables the stage', () => {
    const alone = masterAudio({
      samples: left,
      sampleRate,
      overrides: { dynamics: { multibandComp: { crossover: { cutoffsHz: [200, 2000, 8000] } } } },
    });
    const explicit = masterAudio({
      samples: left,
      sampleRate,
      overrides: {
        dynamics: { multibandComp: { enabled: true, crossover: { cutoffsHz: [200, 2000, 8000] } } },
      },
    });
    expect(alone.stages).toContain(MB);
    expect(Array.from(alone.samples)).toEqual(Array.from(explicit.samples));
  });

  it.each([
    ['lowCutoffHz', 200, 'crossover.cutoffsHz.0'],
    ['highCutoffHz', 4000, 'crossover.cutoffsHz.1'],
    ['lowThresholdDb', -20, 'bands.0.thresholdDb'],
    ['midRatio', 3, 'bands.1.ratio'],
    ['highReleaseMs', 80, 'bands.2.releaseMs'],
  ] as const)('refuses the retired %s shorthand and names %s', (key, value, current) => {
    const overrides = { dynamics: { multibandComp: { [key]: value } } } as never;
    expect(() => masterAudio({ samples: left, sampleRate, overrides })).toThrow(
      invalidFormat(
        `masterAudio: unknown key params.${MB}.${key} (renamed to params.${MB}.${current})`,
      ),
    );
  });

  it('refuses an empty array by path on every entry point', () => {
    const empty: MasteringChainConfig = {
      dynamics: { multibandComp: { crossover: { cutoffsHz: [] } } },
    };
    const message = invalidFormat(`params.${MB}.crossover.cutoffsHz must not be an empty list`);
    expect(() => masterAudio({ samples: left, sampleRate, overrides: empty })).toThrow(message);
    expect(() => new StreamingMasteringChain({ sampleRate, numChannels: 1, ...empty })).toThrow(
      message,
    );
    expect(() =>
      masterAudio({
        samples: left,
        sampleRate,
        overrides: { dynamics: { multibandComp: { bands: [] } } },
      }),
    ).toThrow(invalidFormat(`masterAudio: params.${MB}.bands must not be an empty list`));
  });
});
