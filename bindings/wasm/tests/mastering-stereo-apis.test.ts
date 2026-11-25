import { beforeAll, describe, expect, it } from 'vitest';
import {
  ErrorCode,
  init,
  isSonareError,
  masteringAbMatchLoudnessStereo,
  masteringApplyPairProcessorStereo,
  StreamingMasteringChain,
} from '../dist/index.js';
import type { StereoPairProcessor } from '../src/index.js';
import { expectRefusalOf } from './_helpers.js';

const SAMPLE_RATE = 44_100;

function tone(length: number, gain: number, phase = 0): Float32Array {
  return Float32Array.from(
    { length },
    (_, index) => gain * Math.sin((2 * Math.PI * 440 * index) / SAMPLE_RATE + phase),
  );
}

describe('WASM stereo mastering design APIs', () => {
  beforeAll(async () => init());

  it('rejects streaming updates after delete', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: SAMPLE_RATE,
      numChannels: 1,
      eq: { tilt: { enabled: true } },
    });
    chain.destroy();
    expect(() => chain.setParameter('eq.tilt.tiltDb', 3)).toThrow(
      /^StreamingMasteringChain\.setParameter: StreamingMasteringChain has been destroyed$/,
    );
  });

  it('applies one crossfade to both channels', () => {
    const sourceLeft = tone(128, 0.2);
    const sourceRight = tone(128, -0.35, 0.7);
    const referenceLeft = tone(192, 0.8);
    const referenceRight = tone(192, 0.45, -0.4);
    const request = masteringApplyPairProcessorStereo({
      processor: 'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SAMPLE_RATE,
      params: { mix: 0.25 },
    });
    expect(request.left).toHaveLength(sourceLeft.length);
    expect(request.right).toHaveLength(sourceRight.length);
    expect(request.left[32]).toBeCloseTo(0.75 * sourceLeft[32] + 0.25 * referenceLeft[32], 5);
    expect(request.right[32]).toBeCloseTo(0.75 * sourceRight[32] + 0.25 * referenceRight[32], 5);
  });

  it('requires sampleRate: it has no default', () => {
    const source = tone(128, 0.2);
    const reference = tone(128, 0.4);
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: reference,
      } as never),
    ).toThrow(TypeError);
  });

  it('control: refuses unequal channel lengths and unknown processors', () => {
    const source = tone(32, 0.2);
    const reference = tone(32, 0.4);
    const short = tone(31, 0.2);
    const cases: Array<[StereoPairProcessor, Float32Array, Float32Array, RegExp]> = [
      [
        'match.abCrossfade',
        source,
        short,
        /^masteringApplyPairProcessorStereo: sourceLeft and sourceRight must have the same length$/,
      ],
      [
        'match.abCrossfade',
        short,
        source,
        /^masteringApplyPairProcessorStereo: sourceLeft and sourceRight must have the same length$/,
      ],
      [
        'match.abSwitch' as StereoPairProcessor,
        source,
        source,
        /processor does not support planar stereo pairs/,
      ],
    ];
    for (const [processor, sourceLeft, sourceRight, message] of cases) {
      expect(() =>
        masteringApplyPairProcessorStereo({
          processor,
          sourceLeft,
          sourceRight,
          referenceLeft: reference,
          referenceRight: reference,
          sampleRate: SAMPLE_RATE,
        }),
      ).toThrow(message);
    }
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: short,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrow(
      /^masteringApplyPairProcessorStereo: referenceLeft and referenceRight must have the same length$/,
    );
  });

  it('control: checks source audio before narrowing the reference sample rate', () => {
    const source = tone(32, 0.2);
    source[3] = Number.NaN;
    const request = {
      processor: 'match.abCrossfade' as const,
      sourceLeft: source,
      sourceRight: tone(32, 0.2),
      referenceLeft: tone(32, 0.2),
      referenceRight: tone(32, 0.2),
      sampleRate: SAMPLE_RATE,
      referenceSampleRate: 0.5,
    };
    expect(() => masteringApplyPairProcessorStereo(request)).toThrow(
      new RangeError(
        'masteringApplyPairProcessorStereo: sourceLeft contains a non-finite sample at index 3',
      ),
    );

    const invalidReferenceRate = { ...request, sourceLeft: tone(32, 0.2), referenceSampleRate: 0 };
    expect(() => masteringApplyPairProcessorStereo(invalidReferenceRate)).toThrow(
      new RangeError(
        'masteringApplyPairProcessorStereo: referenceSampleRate must be an integer in [8000, 384000]',
      ),
    );
  });

  it('checks an empty source before narrowing the primary sample rate', () => {
    const empty = new Float32Array(0);
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade' as const,
        sourceLeft: empty,
        sourceRight: empty,
        referenceLeft: tone(32, 0.2),
        referenceRight: tone(32, 0.2),
        sampleRate: 0.5,
      }),
    ).toThrow(
      new RangeError('masteringApplyPairProcessorStereo: sourceLeft must be a non-empty buffer'),
    );
  });

  it('checks stereo processor support before channel length relation', () => {
    const one = tone(32, 0.2);
    const two = tone(31, 0.2);
    const unknown = (() => {
      try {
        masteringApplyPairProcessorStereo({
          processor: 'match.notRegistered' as never,
          sourceLeft: one,
          sourceRight: two,
          referenceLeft: one,
          referenceRight: one,
          sampleRate: SAMPLE_RATE,
        });
      } catch (error) {
        return error;
      }
      throw new Error('expected the unknown processor to be refused');
    })();
    expect(unknown).toBeInstanceOf(RangeError);
    expect((unknown as Error).message).toMatch(
      /^masteringApplyPairProcessorStereo: processor must be one of /,
    );

    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.notRegistered' as never,
        sourceLeft: {} as never,
        sourceRight: one,
        referenceLeft: one,
        referenceRight: one,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrow(
      new TypeError('masteringApplyPairProcessorStereo: sourceLeft must be a Float32Array'),
    );

    const unsupported = (() => {
      try {
        masteringApplyPairProcessorStereo({
          processor: 'match.abSwitch' as never,
          sourceLeft: one,
          sourceRight: two,
          referenceLeft: one,
          referenceRight: one,
          sampleRate: SAMPLE_RATE,
        });
      } catch (error) {
        return error;
      }
      throw new Error('expected the unsupported processor to be refused');
    })();
    expect(isSonareError(unsupported)).toBe(true);
    expect((unsupported as { code: number }).code).toBe(ErrorCode.NotSupported);
    expect((unsupported as Error).message).toMatch(
      /^masteringApplyPairProcessorStereo: processor does not support planar stereo pairs$/,
    );
  });

  it.each([
    {
      name: 'processor membership precedes unequal plane lengths',
      request: {
        processor: 'match.notRegistered',
        sourceLeft: tone(32, 0.2),
        sourceRight: tone(31, 0.2),
        referenceLeft: tone(32, 0.4),
        referenceRight: tone(32, 0.4),
        sampleRate: 0,
      },
      kind: RangeError,
      messagePrefix:
        'masteringApplyPairProcessorStereo: processor must be one of match.abCrossfade, match.abSwitch, match.alignReferenceToSource, match.applyMatchEq',
    },
    {
      name: 'params bag shape precedes processor membership and domains',
      request: {
        processor: 'match.notRegistered',
        sourceLeft: tone(32, 0.2),
        sourceRight: tone(32, 0.2),
        referenceLeft: tone(32, 0.4),
        referenceRight: tone(32, 0.4),
        sampleRate: 0,
        params: 1,
      },
      kind: TypeError,
      messagePrefix: 'masteringApplyPairProcessorStereo: params must be an object',
    },
    {
      name: 'parameter value shape precedes processor membership and domains',
      request: {
        processor: 'match.notRegistered',
        sourceLeft: tone(32, 0.2),
        sourceRight: tone(32, 0.2),
        referenceLeft: tone(32, 0.4),
        referenceRight: tone(32, 0.4),
        sampleRate: 0,
        params: { mix: 'bad' },
      },
      kind: TypeError,
      messagePrefix:
        "masteringApplyPairProcessorStereo: mastering override 'mix' " +
        'must be a number or boolean',
    },
    {
      name: 'control: sampleRate domain precedes source samples',
      request: (() => {
        const sourceLeft = tone(32, 0.2);
        sourceLeft[0] = Number.NaN;
        return {
          processor: 'match.abCrossfade',
          sourceLeft,
          sourceRight: tone(32, 0.2),
          referenceLeft: tone(32, 0.4),
          referenceRight: tone(32, 0.4),
          sampleRate: 48_000.5,
          referenceSampleRate: 0,
        };
      })(),
      kind: RangeError,
      messagePrefix:
        'masteringApplyPairProcessorStereo: sampleRate must be an integer in ' + '[8000, 384000]',
    },
    {
      name: 'control: referenceSampleRate domain precedes parameter domain',
      request: {
        processor: 'match.abCrossfade',
        sourceLeft: tone(32, 0.2),
        sourceRight: tone(32, 0.2),
        referenceLeft: tone(32, 0.4),
        referenceRight: tone(32, 0.4),
        sampleRate: SAMPLE_RATE,
        referenceSampleRate: 0,
        params: { mix: 2 },
      },
      kind: RangeError,
      messagePrefix:
        'masteringApplyPairProcessorStereo: referenceSampleRate must be an integer in ' +
        '[8000, 384000]',
    },
    {
      name: 'control: source samples precede referenceSampleRate domain',
      request: (() => {
        const sourceLeft = tone(32, 0.2);
        sourceLeft[0] = Number.NaN;
        return {
          processor: 'match.abCrossfade',
          sourceLeft,
          sourceRight: tone(32, 0.2),
          referenceLeft: tone(32, 0.4),
          referenceRight: tone(32, 0.4),
          sampleRate: SAMPLE_RATE,
          referenceSampleRate: 48_000.5,
        };
      })(),
      kind: RangeError,
      messagePrefix:
        'masteringApplyPairProcessorStereo: sourceLeft contains a non-finite sample at index 0',
    },
  ])('$name', ({ request, kind, messagePrefix }) => {
    const error = expectRefusalOf(() => masteringApplyPairProcessorStereo(request as never), kind);
    expect(error.message).toBe(messagePrefix);
  });

  it('shares one gain across stereo loudness matching', () => {
    const sourceLeft = tone(512, 0.1);
    const sourceRight = tone(512, 0.4, 0.3);
    const referenceLeft = tone(768, 0.5);
    const referenceRight = tone(768, 0.6, -0.2);
    const matched = masteringAbMatchLoudnessStereo({
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SAMPLE_RATE,
    });
    const gain = 10 ** (matched.appliedGainDb / 20);
    expect(matched.left).toHaveLength(sourceLeft.length);
    expect(matched.right).toHaveLength(sourceRight.length);
    expect(matched.left[200]).toBeCloseTo(sourceLeft[200] * gain, 5);
    expect(matched.right[200]).toBeCloseTo(sourceRight[200] * gain, 5);
    expect(matched.matchedTruePeakDbtp).toBeDefined();
  });

  it('applies a changed streaming parameter to the running stream', () => {
    const config = {
      sampleRate: SAMPLE_RATE,
      numChannels: 1 as const,
      dynamics: { compressor: { enabled: true, thresholdDb: -24, ratio: 3 } },
    };
    const edited = new StreamingMasteringChain(config);
    const control = new StreamingMasteringChain(config);
    try {
      const first = tone(128, 0.7);
      expect(edited.processMono(first)).toEqual(control.processMono(first));
      edited.setParameter('dynamics.compressor.thresholdDb', -40);
      const next = tone(128, 0.7, 0.1);
      expect(edited.processMono(next)).not.toEqual(control.processMono(next));
    } finally {
      edited.destroy();
      control.destroy();
    }
  });

  it('exposes the streaming chain realtime-safe setter', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: SAMPLE_RATE,
      numChannels: 1,
      dynamics: { compressor: { enabled: true, thresholdDb: -24 } },
    });
    try {
      expect(() => chain.setParameter('dynamics.compressor.thresholdDb', -24)).not.toThrow();
      expect(() => chain.setParameter('does.not.exist', 1)).toThrow();
      expect(() => chain.setParameter('dynamics.compressor.enabled', 0)).toThrow();
      expect(() => chain.setParameter('dynamics.compressor.thresholdDb', Number.NaN)).toThrow();
      expect(() =>
        chain.setParameter('dynamics.compressor.thresholdDb', Number.POSITIVE_INFINITY),
      ).toThrow();
      expect(() =>
        chain.setParameter('dynamics.compressor.thresholdDb', Number.MAX_VALUE),
      ).toThrow();
      expect(() =>
        chain.setParameter('dynamics.compressor.thresholdDb', true as unknown as number),
      ).toThrow();
      expect(() =>
        chain.setParameter('dynamics.compressor.thresholdDb', 'bad' as unknown as number),
      ).toThrow();
    } finally {
      chain.destroy();
    }
  });
});
