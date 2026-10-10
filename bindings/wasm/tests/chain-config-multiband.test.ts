import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  type MasteringChainConfig,
  masterAudio,
  masteringChain,
  masteringChainStereo,
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

const flat = (): MasteringChainConfig =>
  ({
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
  }) as unknown as MasteringChainConfig;

describe('typed multiband crossover + bands', () => {
  it('masteringChain mono: nested equals flat keys bit-for-bit', () => {
    const a = masteringChain({ samples: left, sampleRate, config: typed });
    const b = masteringChain({ samples: left, sampleRate, config: flat() });
    expect(a.stages).toContain(MB);
    expect(Array.from(a.samples)).toEqual(Array.from(b.samples));
  });

  it('masteringChainStereo: nested equals flat keys bit-for-bit', () => {
    const a = masteringChainStereo({ left, right, sampleRate, config: typed });
    const b = masteringChainStereo({ left, right, sampleRate, config: flat() });
    expect(Array.from(a.left)).toEqual(Array.from(b.left));
    expect(Array.from(a.right)).toEqual(Array.from(b.right));
  });

  it('masterAudio overrides: nested equals flat keys, and differs from the preset', () => {
    const a = masterAudio({ samples: left, sampleRate, preset: 'pop', overrides: typed });
    const b = masterAudio({ samples: left, sampleRate, preset: 'pop', overrides: flat() });
    const base = masterAudio({ samples: left, sampleRate, preset: 'pop' });
    expect(Array.from(a.samples)).toEqual(Array.from(b.samples));
    expect(Array.from(a.samples)).not.toEqual(Array.from(base.samples));
  });

  it('StreamingMasteringChain: nested equals flat keys bit-for-bit', () => {
    const run = (config: MasteringChainConfig): Float32Array => {
      const chain = new StreamingMasteringChain(config);
      try {
        chain.prepare(sampleRate, 512, 1);
        const out: number[] = [];
        for (let at = 0; at < left.length; at += 512) {
          out.push(...chain.processMono(left.subarray(at, Math.min(at + 512, left.length))));
        }
        return Float32Array.from(out);
      } finally {
        chain.delete();
      }
    };
    expect(Array.from(run(typed))).toEqual(Array.from(run(flat())));
  });

  it('resolves enum names on nested paths, including a band detector', () => {
    const named = masteringChain({ samples: left, sampleRate, config: typed });
    const numeric = masteringChain({
      samples: left,
      sampleRate,
      config: {
        dynamics: {
          multibandComp: {
            ...typed.dynamics?.multibandComp,
            crossover: {
              cutoffsHz: [200, 1000, 4000],
              slope: enumValue(`${MB}.crossover.slope`, 'lr8'),
            },
            bands: typed.dynamics?.multibandComp?.bands?.map((band) =>
              band.detector === undefined
                ? band
                : { ...band, detector: enumValue(`${MB}.bands.1.detector`, 'logRms') },
            ),
          },
        },
      },
    });
    expect(Array.from(named.samples)).toEqual(Array.from(numeric.samples));
  });

  it('a typed crossover alone enables the stage like a lone shorthand cutoff', () => {
    const alone = masteringChain({
      samples: left,
      sampleRate,
      config: { dynamics: { multibandComp: { crossover: { cutoffsHz: [200, 2000, 8000] } } } },
    });
    const explicit = masteringChain({
      samples: left,
      sampleRate,
      config: {
        dynamics: { multibandComp: { enabled: true, crossover: { cutoffsHz: [200, 2000, 8000] } } },
      },
    });
    const shorthand = masteringChain({
      samples: left,
      sampleRate,
      config: { dynamics: { multibandComp: { lowCutoffHz: 200 } } },
    });
    expect(alone.stages).toContain(MB);
    expect(shorthand.stages).toContain(MB);
    expect(Array.from(alone.samples)).toEqual(Array.from(explicit.samples));
  });

  it('refuses an empty array by path on every entry point', () => {
    const empty: MasteringChainConfig = {
      dynamics: { multibandComp: { crossover: { cutoffsHz: [] } } },
    };
    const message = `Mastering override '${MB}.crossover.cutoffsHz' is an empty list, which has no flat spelling.`;
    expect(() => masteringChain({ samples: left, sampleRate, config: empty })).toThrow(message);
    expect(() => masterAudio({ samples: left, sampleRate, overrides: empty })).toThrow(message);
    expect(() => new StreamingMasteringChain(empty)).toThrow(message);
    expect(() =>
      masteringChain({
        samples: left,
        sampleRate,
        config: { dynamics: { multibandComp: { bands: [] } } },
      }),
    ).toThrow(`'${MB}.bands' is an empty list`);
  });
});
