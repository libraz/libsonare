import { beforeAll, describe, expect, it } from 'vitest';
import {
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

const enumValue = (path: string, name: string): number => {
  const value = getSonareModule().masteringEnumValue('', path, name);
  if (value === null) {
    throw new Error(`no enum ${name} at ${path}`);
  }
  return value;
};

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

/** The flat map the native layer reads for `typed`, enum names already resolved. */
const flat = (): Record<string, unknown> => ({
  [`${MB}.enabled`]: true,
  [`${MB}.crossover.cutoffsHz.0`]: 200,
  [`${MB}.crossover.cutoffsHz.1`]: 1000,
  [`${MB}.crossover.cutoffsHz.2`]: 4000,
  [`${MB}.crossover.slope`]: enumValue(`${MB}.crossover.slope`, 'lr8'),
  [`${MB}.bands.0.thresholdDb`]: -24,
  [`${MB}.bands.0.ratio`]: 3,
  [`${MB}.bands.1.thresholdDb`]: -20,
  [`${MB}.bands.1.ratio`]: 2.5,
  [`${MB}.bands.1.detector`]: enumValue(`${MB}.bands.1.detector`, 'logRms'),
  [`${MB}.bands.2.thresholdDb`]: -18,
  [`${MB}.bands.2.ratio`]: 2,
  [`${MB}.bands.3.thresholdDb`]: -16,
  [`${MB}.bands.3.ratio`]: 4,
  [`${MB}.bands.3.makeupGainDb`]: 1,
});

describe('typed multiband crossover + bands', () => {
  it('masterAudio mono: nested equals the native flat keys bit-for-bit', () => {
    const a = masterAudio({ samples: left, sampleRate, overrides: typed });
    const b = getSonareModule().masteringChain(left, sampleRate, { __flatParams: flat() });
    expect(a.stages).toContain(MB);
    expect(Array.from(a.samples)).toEqual(Array.from(b.samples));
  });

  it('masterAudioStereo: nested equals the native flat keys bit-for-bit', () => {
    const a = masterAudioStereo({ left, right, sampleRate, overrides: typed });
    const b = getSonareModule().masteringChainStereo(left, right, sampleRate, {
      __flatParams: flat(),
    });
    expect(Array.from(a.left)).toEqual(Array.from(b.left));
    expect(Array.from(a.right)).toEqual(Array.from(b.right));
  });

  it('masterAudio overrides differ from the preset alone', () => {
    const a = masterAudio({ samples: left, sampleRate, preset: 'pop', overrides: typed });
    const base = masterAudio({ samples: left, sampleRate, preset: 'pop' });
    expect(Array.from(a.samples)).not.toEqual(Array.from(base.samples));
  });

  it('refuses the dotted spelling on every entry point', () => {
    const dotted = { [`${MB}.enabled`]: true } as unknown as MasteringChainConfig;
    expect(() => masterAudio({ samples: left, sampleRate, overrides: dotted })).toThrow(TypeError);
    expect(() => new StreamingMasteringChain({ sampleRate, numChannels: 1, ...dotted })).toThrow(
      TypeError,
    );
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

  it('takes enum tokens on nested paths, including a band detector, and refuses numbers', () => {
    const numeric = {
      dynamics: {
        multibandComp: {
          crossover: { cutoffsHz: [200, 1000, 4000], slope: 3 },
          bands: [{ detector: 1 }],
        },
      },
    } as unknown as MasteringChainConfig;
    expect(() => masterAudio({ samples: left, sampleRate, overrides: numeric })).toThrow(TypeError);
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
    ['lowCutoffHz', 200, 'cutoff0Hz'],
    ['highCutoffHz', 4000, 'cutoff1Hz'],
    ['lowThresholdDb', -20, 'bands.0.thresholdDb'],
    ['midRatio', 3, 'bands.1.ratio'],
    ['highReleaseMs', 80, 'bands.2.releaseMs'],
  ] as const)('refuses the retired %s shorthand and names %s', (key, value, current) => {
    const overrides = { dynamics: { multibandComp: { [key]: value } } } as never;
    expect(() => masterAudio({ samples: left, sampleRate, overrides })).toThrow(current);
  });

  it('refuses an empty array by path on every entry point', () => {
    const empty: MasteringChainConfig = {
      dynamics: { multibandComp: { crossover: { cutoffsHz: [] } } },
    };
    const message = `Mastering override '${MB}.crossover.cutoffsHz' is an empty list, which has no flat spelling.`;
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
    ).toThrow(`'${MB}.bands' is an empty list`);
  });
});
