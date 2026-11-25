import { beforeAll, describe, expect, expectTypeOf, it } from 'vitest';
import {
  type EqBandInput,
  type EqBandPhase,
  type EqBandType,
  type EqCoeffMode,
  type EqPhaseMode,
  type EqStereoPlacement,
  init,
  type StereoBlock,
  StreamingEqualizer,
  type StreamingEqualizerConfig,
  StreamingMasteringChain,
  type StreamingMasteringChainConfig,
} from '../../dist/index.js';
import { StreamingMasteringChain as WorkletChain } from '../../dist/worklet.js';
import { init as initSource } from '../../src/index';
import { getSonareModule } from '../../src/module_state';

const sampleRate = 48000;
const block = 256;

function adjacentDouble(value: number, upward: boolean): number {
  if (value === 0) {
    return upward ? Number.MIN_VALUE : -Number.MIN_VALUE;
  }
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, value, false);
  let bits = view.getBigUint64(0, false);
  bits += value > 0 === upward ? 1n : -1n;
  view.setBigUint64(0, bits, false);
  return view.getFloat64(0, false);
}

function tone(frames: number): Float32Array {
  const out = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    out[i] = 0.2 * Math.sin((2 * Math.PI * 440 * i) / sampleRate);
  }
  return out;
}

function errorOf(fn: () => unknown): unknown {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

const chainConfig = {
  sampleRate,
  numChannels: 1 as const,
  maxBlockSize: block,
  eq: { tilt: { tiltDb: 1 } },
};

beforeAll(async () => {
  await init();
});

describe('StreamingEqualizer.constructor', () => {
  it.each([
    ['no config', undefined, TypeError],
    ['null config', null, TypeError],
    ['no sampleRate', {}, TypeError],
    ['string sampleRate', { sampleRate: '48000' }, TypeError],
    ['fractional sampleRate', { sampleRate: 48000.5 }, RangeError],
    ['zero sampleRate', { sampleRate: 0 }, RangeError],
    ['string maxBlockSize', { sampleRate, maxBlockSize: '512' }, TypeError],
    ['zero maxBlockSize', { sampleRate, maxBlockSize: 0 }, RangeError],
    ['fractional maxBlockSize', { sampleRate, maxBlockSize: 512.5 }, RangeError],
  ] as const)('refuses %s', (_name, config, errorClass) => {
    expect(errorOf(() => new StreamingEqualizer(config as never))).toBeInstanceOf(errorClass);
  });

  it('builds with only a sampleRate and processes a block', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(eq.processMono(tone(block)).length).toBe(block);
    } finally {
      eq.destroy();
    }
  });

  it('takes a StreamingEqualizerConfig object', () => {
    expectTypeOf<ConstructorParameters<typeof StreamingEqualizer>>().toEqualTypeOf<
      [config: StreamingEqualizerConfig]
    >();
    expect(
      errorOf(
        () =>
          new (StreamingEqualizer as unknown as new (...args: unknown[]) => unknown)(sampleRate),
      ),
    ).toBeInstanceOf(TypeError);
  });
});

describe('StreamingEqualizerConfig.sampleRate', () => {
  it('is required in the type', () => {
    expectTypeOf<StreamingEqualizerConfig['sampleRate']>().toEqualTypeOf<number>();
    // @ts-expect-error sampleRate is required
    const config: StreamingEqualizerConfig = { maxBlockSize: 512 };
    expect(config).toBeDefined();
  });
});

describe('StreamingEqualizer.setAutoGain', () => {
  it('takes an options object with a boolean enabled', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      eq.setAutoGain({ enabled: true });
      expect(eq.processMono(tone(block)).length).toBe(block);
      eq.setAutoGain({ enabled: false });
      expect(eq.processMono(tone(block)).length).toBe(block);
    } finally {
      eq.destroy();
    }
  });

  it.each([
    ['a bare boolean', true],
    ['a non-boolean enabled', { enabled: 1 }],
    ['an omitted enabled', {}],
    ['null', null],
  ] as const)('refuses %s with a TypeError', (_name, value) => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(errorOf(() => eq.setAutoGain(value as never))).toBeInstanceOf(TypeError);
    } finally {
      eq.destroy();
    }
  });
});

describe('StreamingMasteringChain.constructor', () => {
  it.each([
    ['no config', undefined, TypeError, /config/],
    ['no sampleRate', { eq: { tilt: { tiltDb: 1 } }, numChannels: 1 }, TypeError, /sampleRate/],
    ['fractional sampleRate', { sampleRate: 48000.5, numChannels: 1 }, RangeError, /sampleRate/],
    [
      'zero maxBlockSize',
      { sampleRate, numChannels: 1, maxBlockSize: 0 },
      RangeError,
      /maxBlockSize|max_block_size/,
    ],
    [
      'string maxBlockSize',
      { sampleRate, numChannels: 1, maxBlockSize: '128' },
      TypeError,
      /maxBlockSize/,
    ],
    ['no numChannels', { sampleRate }, TypeError, /numChannels/],
    ['three channels', { sampleRate, numChannels: 3 }, RangeError, /numChannels|num_channels/],
    ['zero channels', { sampleRate, numChannels: 0 }, RangeError, /numChannels|num_channels/],
    ['string channels', { sampleRate, numChannels: '2' }, TypeError, /numChannels/],
  ] as const)('refuses %s', (_name, config, errorClass, field) => {
    const error = errorOf(() => new StreamingMasteringChain(config as never));
    expect(error).toBeInstanceOf(errorClass);
    expect((error as Error).message).toMatch(field);
  });

  it('prepares in the constructor for the requested channel count', () => {
    const mono = new StreamingMasteringChain(chainConfig);
    const stereo = new StreamingMasteringChain({
      ...chainConfig,
      numChannels: 2,
      maxBlockSize: block,
    });
    try {
      expect(mono.processMono(tone(block)).length).toBe(block);
      const out = stereo.processStereo(tone(block), tone(block));
      expect(out.left.length).toBe(block);
      expect(out.right.length).toBe(block);
    } finally {
      mono.destroy();
      stereo.destroy();
    }
  });

  it('takes the library default block size when maxBlockSize is omitted', () => {
    const { maxBlockSize: _omitted, ...withoutBlock } = chainConfig;
    const chain = new StreamingMasteringChain(withoutBlock);
    try {
      expect(chain.processMono(tone(64)).length).toBe(64);
    } finally {
      chain.destroy();
    }
  });

  it('has no prepare() on either entry point', () => {
    for (const ctor of [StreamingMasteringChain, WorkletChain]) {
      expect('prepare' in ctor.prototype).toBe(false);
    }
  });
});

describe('maxBlockSize', () => {
  it('reports the library default a chain accepts when maxBlockSize is omitted', () => {
    const { maxBlockSize: _omitted, ...withoutBlock } = chainConfig;
    const chain = new StreamingMasteringChain(withoutBlock);
    try {
      const max = chain.maxBlockSize;
      expect(max).toBeGreaterThan(0);
      expect(chain.processMono(tone(max)).length).toBe(max);
      expect(() => chain.processMono(tone(max + 1))).toThrow(RangeError);
    } finally {
      chain.destroy();
    }
  });

  it('reports the given block size on a chain', () => {
    const chain = new StreamingMasteringChain(chainConfig);
    try {
      expect(chain.maxBlockSize).toBe(block);
    } finally {
      chain.destroy();
    }
  });

  it('reports the library default an equalizer accepts when maxBlockSize is omitted', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      const max = eq.maxBlockSize;
      expect(max).toBeGreaterThan(0);
      expect(eq.processMono(tone(max)).length).toBe(max);
      expect(() => eq.processMono(tone(max + 1))).toThrow(RangeError);
    } finally {
      eq.destroy();
    }
  });

  it('reports the given block size on an equalizer', () => {
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: block });
    try {
      expect(eq.maxBlockSize).toBe(block);
    } finally {
      eq.destroy();
    }
  });
});

describe('StreamingMasteringChainConfig', () => {
  it('carries the construction fields beside the chain sections', () => {
    expectTypeOf<StreamingMasteringChainConfig['sampleRate']>().toEqualTypeOf<number>();
    expectTypeOf<StreamingMasteringChainConfig['numChannels']>().toEqualTypeOf<1 | 2>();
    expectTypeOf<StreamingMasteringChainConfig['maxBlockSize']>().toEqualTypeOf<
      number | undefined
    >();
  });
});

describe('streaming counters and latency are readonly getters', () => {
  const GETTERS = [
    ['StreamingMasteringChain.latencySamples', StreamingMasteringChain, 'latencySamples'],
    [
      'StreamingMasteringChain.nonFiniteSubstitutionCount',
      StreamingMasteringChain,
      'nonFiniteSubstitutionCount',
    ],
    [
      'StreamingMasteringChain.nonFiniteDiscardCount',
      StreamingMasteringChain,
      'nonFiniteDiscardCount',
    ],
    ['worklet StreamingMasteringChain.latencySamples', WorkletChain, 'latencySamples'],
    [
      'worklet StreamingMasteringChain.nonFiniteSubstitutionCount',
      WorkletChain,
      'nonFiniteSubstitutionCount',
    ],
    [
      'worklet StreamingMasteringChain.nonFiniteDiscardCount',
      WorkletChain,
      'nonFiniteDiscardCount',
    ],
    ['StreamingEqualizer.lastAutoGainDb', StreamingEqualizer, 'lastAutoGainDb'],
    ['StreamingEqualizer.latencySamples', StreamingEqualizer, 'latencySamples'],
    ['StreamingEqualizer.nonFiniteDiscardCount', StreamingEqualizer, 'nonFiniteDiscardCount'],
  ] as const;

  it.each(GETTERS)('%s', (_label, ctor, name) => {
    const descriptor = Object.getOwnPropertyDescriptor(ctor.prototype, name);
    expect(typeof descriptor?.get).toBe('function');
    expect(descriptor?.set).toBeUndefined();
  });

  it('reads through the instance', () => {
    const chain = new StreamingMasteringChain(chainConfig);
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(chain.stageNames()).toEqual(['eq.tilt']);
      expect(chain.latencySamples).toBeGreaterThanOrEqual(0);
      expect(chain.nonFiniteSubstitutionCount).toBe(0);
      expect(chain.nonFiniteDiscardCount).toBe(0);
      expect(eq.nonFiniteDiscardCount).toBe(0);
      expect(typeof eq.lastAutoGainDb).toBe('number');
    } finally {
      chain.destroy();
      eq.destroy();
    }
  });
});

describe('StreamingMasteringChain.stageNames', () => {
  it('is a method returning a fresh array per call', () => {
    const chain = new StreamingMasteringChain(chainConfig);
    try {
      const first = chain.stageNames();
      expect(first).toEqual(['eq.tilt']);
      expect(chain.stageNames()).not.toBe(first);
      expectTypeOf(chain.stageNames).toEqualTypeOf<() => string[]>();
    } finally {
      chain.destroy();
    }
  });
});

describe('EqStereoPlacement', () => {
  const freqs = new Float32Array([100, 1000, 10000]);
  const mid = { type: 'peak', frequencyHz: 1000, gainDb: 6, q: 1, enabled: true, placement: 'mid' };

  it('takes the five lowerCamel tokens, default stereo', () => {
    expectTypeOf<EqStereoPlacement>().toEqualTypeOf<'stereo' | 'left' | 'right' | 'mid' | 'side'>();
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      eq.setBand(0, mid as never);
      const stereo = eq.magnitudeResponse(freqs);
      expect(Array.from(eq.magnitudeResponse(freqs, 'stereo'))).toEqual(Array.from(stereo));
      expect(eq.magnitudeResponse(freqs, 'mid')[1]).toBeGreaterThan(5);
      expect(Math.abs(eq.magnitudeResponse(freqs, 'left')[1] as number)).toBeLessThan(0.5);
    } finally {
      eq.destroy();
    }
  });

  it('magnitudeResponse refuses invalid frequencies by index without clamping', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      for (const frequency of [
        Number.NaN,
        Number.POSITIVE_INFINITY,
        Number.NEGATIVE_INFINITY,
        -1,
        sampleRate / 2 + 1,
      ]) {
        const error = errorOf(() => eq.magnitudeResponse(new Float32Array([frequency])));
        expect(error).toBeInstanceOf(RangeError);
        expect((error as Error).message).toMatch(
          /^StreamingEqualizer\.magnitudeResponse: frequenciesHz\[0\] must be finite and in \[0, 24000\]$/,
        );
      }
      expect(eq.magnitudeResponse(new Float32Array([0, sampleRate / 2]))).toHaveLength(2);
    } finally {
      eq.destroy();
    }
  });

  it('StreamingEqualizer.magnitudeResponse takes the current placement token', () => {
    expectTypeOf<Parameters<StreamingEqualizer['magnitudeResponse']>[1]>().toEqualTypeOf<
      EqStereoPlacement | undefined
    >();
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(Array.from(eq.magnitudeResponse(freqs))).toEqual(
        Array.from(eq.magnitudeResponse(freqs, 'stereo')),
      );
    } finally {
      eq.destroy();
    }
  });

  it.each([
    ['Mid', RangeError],
    ['Stereo', RangeError],
    ['MID', RangeError],
    ['bogus', RangeError],
    [3, TypeError],
    [0, TypeError],
    [{}, TypeError],
  ] as const)('magnitudeResponse and setBand refuse %j', (value, errorClass) => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(errorOf(() => eq.magnitudeResponse(freqs, value as never))).toBeInstanceOf(errorClass);
      expect(errorOf(() => eq.setBand(0, { placement: value } as never))).toBeInstanceOf(
        errorClass,
      );
    } finally {
      eq.destroy();
    }
  });
});

describe('StereoBlock', () => {
  it('names the planar stereo result of the process and flush calls', () => {
    expectTypeOf<StereoBlock>().toEqualTypeOf<{ left: Float32Array; right: Float32Array }>();
    const chain = new StreamingMasteringChain({ ...chainConfig, numChannels: 2 });
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: block });
    try {
      expectTypeOf(chain.processStereo).returns.toEqualTypeOf<StereoBlock>();
      expectTypeOf(chain.flushStereo).returns.toEqualTypeOf<StereoBlock>();
      expectTypeOf(eq.processStereo).returns.toEqualTypeOf<StereoBlock>();
    } finally {
      chain.destroy();
      eq.destroy();
    }
  });
});

describe('StreamingEqualizer band tokens', () => {
  it('declares the band tokens in lowerCamel and no lookaheadMs', () => {
    expectTypeOf<EqBandInput['type']>().toEqualTypeOf<EqBandType | undefined>();
    expectTypeOf<'lowShelf'>().toExtend<EqBandType>();
    expectTypeOf<'vicanek'>().toExtend<EqCoeffMode>();
    expectTypeOf<'naturalPhase'>().toExtend<EqBandPhase>();
    // @ts-expect-error retired spelling of detectorDelayMs
    type NoLookahead = EqBandInput['lookaheadMs'];
    expectTypeOf<NoLookahead>().not.toBeNever();
  });

  it('takes every lowerCamel token', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      const types: EqBandType[] = [
        'peak',
        'lowShelf',
        'highShelf',
        'lowPass',
        'highPass',
        'bandPass',
        'notch',
        'tiltShelf',
        'flatTilt',
        'allPass',
      ];
      for (const type of types) {
        eq.setBand(0, { type, frequencyHz: 1000, enabled: true });
      }
      eq.setBand(0, { type: 'peak', coeffMode: 'vicanek', phase: 'naturalPhase' });
      eq.setBand(0, { type: 'peak', coeffMode: 'rbj', phase: 'zeroLatency' });
    } finally {
      eq.destroy();
    }
  });

  it.each([
    { type: 'Peak' },
    { type: 'LowShelf' },
    { type: 'Bell' },
    { type: 'peak', coeffMode: 'Rbj' },
    { type: 'peak', phase: 'NaturalPhase' },
  ])('refuses non-canonical enum tokens %j', (band) => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(errorOf(() => eq.setBand(0, band as never))).toBeInstanceOf(RangeError);
    } finally {
      eq.destroy();
    }
  });

  it.each(['lookaheadMs', 'bogus'])(
    'refuses the undeclared band key %s with the sorted accepted list',
    (key) => {
      const eq = new StreamingEqualizer({ sampleRate });
      try {
        const error = errorOf(() => eq.setBand(0, { type: 'peak', [key]: 5 } as never));
        expect(error).toBeInstanceOf(TypeError);
        expect((error as Error).message).toBe(
          `StreamingEqualizer.setBand: unknown key band.${key} (expected attackMs, autoThreshold, bypassed, coeffMode, detectorDelayMs, dynamic, enabled, externalSidechain, frequencyHz, gainDb, phase, placement, proportionalQ, proportionalQStrength, q, rangeDb, ratio, releaseMs, sidechainFreqHz, sidechainQ, slopeDbOct, soloed, thresholdDb, type)`,
        );
      } finally {
        eq.destroy();
      }
    },
  );
});

describe('StreamingEqualizer numeric domains', () => {
  it('refuses caller doubles outside EQ numeric domains before float narrowing', () => {
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: block });
    const floatMax = 3.4028234663852886e38;
    const check = (call: () => unknown, subject: string) => {
      const error = errorOf(call);
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toBe(subject);
    };
    try {
      const dbText = 'must be finite and in [-3.4028235e38, 770]';
      const outputGainText = 'must be finite and at most 770';
      check(
        () => eq.setGainScale(adjacentDouble(0, false)),
        'StreamingEqualizer.setGainScale: scale must be finite and in [0, 2]',
      );
      check(
        () => eq.setGainScale(adjacentDouble(2, true)),
        'StreamingEqualizer.setGainScale: scale must be finite and in [0, 2]',
      );
      check(
        () => eq.setOutputPan(adjacentDouble(-1, false)),
        'StreamingEqualizer.setOutputPan: pan must be finite and in [-1, 1]',
      );
      check(
        () => eq.setOutputPan(adjacentDouble(1, true)),
        'StreamingEqualizer.setOutputPan: pan must be finite and in [-1, 1]',
      );
      check(
        () => eq.setOutputGainDb(adjacentDouble(-floatMax, false)),
        `StreamingEqualizer.setOutputGainDb: gainDb ${outputGainText}`,
      );
      check(
        () => eq.setOutputGainDb(adjacentDouble(770, true)),
        `StreamingEqualizer.setOutputGainDb: gainDb ${outputGainText}`,
      );

      const band = { type: 'peak', frequencyHz: 1000, gainDb: 0, q: 1, enabled: true } as const;
      const bandCases: Array<[string, number, string]> = [
        ['gainDb', adjacentDouble(-floatMax, false), dbText],
        ['gainDb', adjacentDouble(770, true), dbText],
      ];
      for (const [key, value, predicate] of bandCases) {
        check(
          () => eq.setBand(0, { ...band, [key]: value } as never),
          `StreamingEqualizer.setBand: band.${key} ${predicate}`,
        );
      }

      const dynamicBand = { ...band, dynamic: true };
      const floatMaxText = '3.4028235e38';
      const dynamicCases: Array<[string, number, string]> = [
        ['thresholdDb', adjacentDouble(-floatMax, false), dbText],
        ['thresholdDb', adjacentDouble(770, true), dbText],
        ['rangeDb', adjacentDouble(-floatMax, false), dbText],
        ['rangeDb', adjacentDouble(770, true), dbText],
        ['ratio', adjacentDouble(1, false), 'must be finite and in [1, 3.4028235e38]'],
        ['ratio', adjacentDouble(floatMax, true), 'must be finite and in [1, 3.4028235e38]'],
        ['attackMs', adjacentDouble(0, false), `must be finite and in [0, ${floatMaxText}]`],
        ['attackMs', adjacentDouble(floatMax, true), `must be finite and in [0, ${floatMaxText}]`],
        ['releaseMs', adjacentDouble(0, false), `must be finite and in [0, ${floatMaxText}]`],
        ['releaseMs', adjacentDouble(floatMax, true), `must be finite and in [0, ${floatMaxText}]`],
        ['detectorDelayMs', adjacentDouble(0, false), `must be finite and in [0, ${floatMaxText}]`],
        [
          'detectorDelayMs',
          adjacentDouble(floatMax, true),
          `must be finite and in [0, ${floatMaxText}]`,
        ],
        [
          'sidechainQ',
          adjacentDouble(floatMax, true),
          `must be finite and in (0, ${floatMaxText}]`,
        ],
        [
          'sidechainFreqHz',
          adjacentDouble(-1, false),
          `must be finite and in (0, ${floatMaxText}]`,
        ],
        ['sidechainFreqHz', adjacentDouble(-1, true), `must be finite and in (0, ${floatMaxText}]`],
        [
          'sidechainFreqHz',
          adjacentDouble(floatMax, true),
          `must be finite and in (0, ${floatMaxText}]`,
        ],
      ];
      for (const [key, value, predicate] of dynamicCases) {
        check(
          () => eq.setBand(0, { ...dynamicBand, [key]: value } as never),
          `StreamingEqualizer.setBand: band.${key} ${predicate}`,
        );
      }
      // Control: exact -1 remains the documented follow-band-frequency value.
      expect(() => eq.setBand(0, { ...dynamicBand, sidechainFreqHz: -1 })).not.toThrow();
    } finally {
      eq.destroy();
    }
  });
});

describe('StreamingEqualizer strict numeric bounds', () => {
  it('names strict EQ edge refusals under the band argument', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    const floatMaxText = '3.4028235e38';
    try {
      const frequency = errorOf(() =>
        eq.setBand(0, {
          type: 'peak',
          frequencyHz: adjacentDouble(0, false),
          gainDb: 0,
          enabled: true,
        }),
      );
      expect(frequency).toBeInstanceOf(RangeError);
      expect((frequency as Error).message).toBe(
        'StreamingEqualizer.setBand: band.frequencyHz must be finite and in (0, 24000)',
      );
      const frequencyUpper = errorOf(() =>
        eq.setBand(0, {
          type: 'peak',
          frequencyHz: adjacentDouble(24000, true),
          gainDb: 0,
          enabled: true,
        }),
      );
      expect(frequencyUpper).toBeInstanceOf(RangeError);
      expect((frequencyUpper as Error).message).toBe(
        'StreamingEqualizer.setBand: band.frequencyHz must be finite and in (0, 24000)',
      );
      const q = errorOf(() =>
        eq.setBand(0, {
          type: 'peak',
          frequencyHz: 1000,
          gainDb: 0,
          enabled: true,
          dynamic: true,
          sidechainQ: adjacentDouble(0, false),
        }),
      );
      expect(q).toBeInstanceOf(RangeError);
      expect((q as Error).message).toBe(
        `StreamingEqualizer.setBand: band.sidechainQ must be finite and in (0, ${floatMaxText}]`,
      );
      const sidechainFrequency = errorOf(() =>
        eq.setBand(0, {
          type: 'peak',
          frequencyHz: 1000,
          gainDb: 0,
          enabled: true,
          dynamic: true,
          sidechainFreqHz: adjacentDouble(0, false),
        }),
      );
      expect(sidechainFrequency).toBeInstanceOf(RangeError);
      expect((sidechainFrequency as Error).message).toBe(
        `StreamingEqualizer.setBand: band.sidechainFreqHz must be finite and in (0, ${floatMaxText}]`,
      );
    } finally {
      eq.destroy();
    }
  });
});

describe('StreamingEqualizer refusal subjects', () => {
  it('uses the public field paths for band and output-domain refusals', () => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      const frequency = errorOf(() =>
        eq.setBand(0, { type: 'peak', frequencyHz: 0, gainDb: 3, q: 1, enabled: true }),
      );
      expect(frequency).toBeInstanceOf(RangeError);
      expect((frequency as Error).message).toContain('band.frequencyHz');

      const scale = errorOf(() => eq.setGainScale(-0.1));
      expect(scale).toBeInstanceOf(RangeError);
      expect((scale as Error).message).toMatch(/(?:^|: )scale must /);

      const gain = errorOf(() => eq.setOutputGainDb(770.25));
      expect(gain).toBeInstanceOf(RangeError);
      expect((gain as Error).message).toMatch(/(?:^|: )gainDb must /);

      const pan = errorOf(() => eq.setOutputPan(2));
      expect(pan).toBeInstanceOf(RangeError);
      expect((pan as Error).message).toMatch(/(?:^|: )pan must /);
    } finally {
      eq.destroy();
    }
  });

  it('names invalid ordinals at the raw EQ embind entry', async () => {
    await initSource();
    const raw = getSonareModule().createEqualizer({ sampleRate });
    try {
      for (const value of [99, 1.5, 1e100]) {
        const phase = errorOf(() => raw.setPhaseMode(value));
        expect(phase).toBeInstanceOf(RangeError);
        expect((phase as Error).message).toBe(
          'StreamingEqualizer.setPhaseMode: mode must be an integer in [1, 3]',
        );

        const placement = errorOf(() => raw.magnitudeResponse(value, new Float32Array([1000])));
        expect(placement).toBeInstanceOf(RangeError);
        expect((placement as Error).message).toBe(
          'StreamingEqualizer.magnitudeResponse: placement must be an integer in [0, 4]',
        );
      }
    } finally {
      raw.delete();
    }
  });
});

describe('StreamingEqualizer.setPhaseMode', () => {
  const peak = { type: 'peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true } as const;

  it('takes the three tokens and linear phase adds latency', () => {
    const eq = new StreamingEqualizer({ sampleRate, maxBlockSize: 512 });
    try {
      const mode: EqPhaseMode = 'linear';
      eq.setPhaseMode(mode);
      eq.setBand(0, peak);
      expect(eq.latencySamples).toBeGreaterThan(0);
      eq.setPhaseMode('natural');
      eq.setPhaseMode('zero');
    } finally {
      eq.destroy();
    }
  });

  it.each([
    ['zero-latency', RangeError],
    ['zero_latency', RangeError],
    ['natural-phase', RangeError],
    ['linear_phase', RangeError],
    ['Linear', RangeError],
    ['', RangeError],
    [1, TypeError],
    [3, TypeError],
    [undefined, TypeError],
    [null, TypeError],
  ] as const)('refuses %j', (value, errorClass) => {
    const eq = new StreamingEqualizer({ sampleRate });
    try {
      expect(errorOf(() => eq.setPhaseMode(value as never))).toBeInstanceOf(errorClass);
    } finally {
      eq.destroy();
    }
  });
});

describe('native still uses the old spelling', () => {
  beforeAll(async () => {
    await initSource();
  });

  it('phase mode is an ordinal (3 = linear)', () => {
    const raw = getSonareModule().createEqualizer({ sampleRate, maxBlockSize: 512 });
    try {
      raw.setPhaseMode(3);
      raw.setBand(0, { type: 'peak', frequencyHz: 1000, gainDb: 3, q: 1, enabled: true });
      expect(raw.latencySamples()).toBeGreaterThan(0);
    } finally {
      raw.delete();
    }
  });

  it('chain construction and prepare are two native steps', () => {
    const raw = getSonareModule().createStreamingMasteringChain(
      JSON.stringify({ params: { 'eq.tilt.tiltDb': 1 } }),
    );
    try {
      expect(typeof raw.prepare).toBe('function');
      raw.prepare(sampleRate, 128, 1);
      expect(raw.stageNames()).toContain('eq.tilt');
    } finally {
      raw.delete();
    }
  });
});
