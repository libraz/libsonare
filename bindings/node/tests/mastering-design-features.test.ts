import { describe, expect, it } from 'vitest';
import {
  masteringAbMatchLoudnessStereo,
  masteringApplyPairProcessor,
  masteringApplyPairProcessorStereo,
  StreamingMasteringChain,
} from '../src/index.js';
import type { StereoPairProcessor } from '../src/types.js';

const SR = 44100;

function channel(length: number, gain: number, phase = 0): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = gain * Math.sin((2 * Math.PI * 440 * i) / SR + phase);
  }
  return out;
}

describe('stereo mastering design APIs', () => {
  it('keeps stereo pair crossfade channels aligned at mix endpoints', () => {
    const sourceLeft = channel(256, 0.2);
    const sourceRight = channel(256, -0.35, 0.7);
    const referenceLeft = channel(384, 0.8);
    const referenceRight = channel(384, 0.45, -0.4);

    const source = masteringApplyPairProcessorStereo({
      processor: 'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SR,
      params: { mix: 0 },
    });
    const reference = masteringApplyPairProcessorStereo({
      processor: 'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SR,
      params: { mix: 1 },
    });

    expect(source.left).toEqual(sourceLeft);
    expect(source.right).toEqual(sourceRight);
    expect(reference.left).toEqual(referenceLeft.slice(0, sourceLeft.length));
    expect(reference.right).toEqual(referenceRight.slice(0, sourceRight.length));
  });

  it('applies one stereo crossfade amount per channel', () => {
    const sourceLeft = channel(128, 0.2);
    const sourceRight = channel(128, -0.35, 0.7);
    const referenceLeft = channel(192, 0.8);
    const referenceRight = channel(192, 0.45, -0.4);
    const request = masteringApplyPairProcessorStereo({
      processor: 'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SR,
      params: { mix: 0.25 },
    });
    expect(request.left[32]).toBeCloseTo(0.75 * sourceLeft[32] + 0.25 * referenceLeft[32], 5);
    expect(request.right[32]).toBeCloseTo(0.75 * sourceRight[32] + 0.25 * referenceRight[32], 5);
    expect(request.left.length).toBe(sourceLeft.length);
  });

  it('requires a sample rate and has no positional form', () => {
    const sourceLeft = channel(128, 0.2);
    const sourceRight = channel(128, -0.3);
    const referenceLeft = channel(160, 0.5);
    const referenceRight = channel(160, -0.6);
    expect(() =>
      (masteringApplyPairProcessorStereo as (request: unknown) => unknown)({
        processor: 'match.abCrossfade',
        sourceLeft,
        sourceRight,
        referenceLeft,
        referenceRight,
        params: { mix: 0.25 },
      }),
    ).toThrow(TypeError);
    expect(() =>
      (masteringApplyPairProcessorStereo as (...args: unknown[]) => unknown)(
        'match.abCrossfade',
        sourceLeft,
        sourceRight,
        referenceLeft,
        referenceRight,
        SR,
        { mix: 0.25 },
      ),
    ).toThrow(TypeError);
  });

  it('raises the same error class for the same defect on every pair input', () => {
    const source = channel(32, 0.2);
    const reference = channel(32, 0.4);
    const short = channel(31, 0.2);
    const cases: Array<[StereoPairProcessor, Float32Array, Float32Array, Float32Array, RegExp]> = [
      ['match.abCrossfade', short, reference, reference, /Source left and right/],
      ['match.abCrossfade', source, reference, short, /Reference left and right/],
      ['match.abSwitch' as StereoPairProcessor, source, reference, reference, /abSwitch/],
    ];
    for (const [processor, sourceRight, referenceLeft, referenceRight, message] of cases) {
      const request = () =>
        masteringApplyPairProcessorStereo({
          processor,
          sourceLeft: source,
          sourceRight,
          referenceLeft,
          referenceRight,
          sampleRate: SR,
        });
      expect(request).toThrow(message);
    }
  });

  it('rejects unsupported stereo pair processors and channel shape errors', () => {
    const source = channel(32, 0.2);
    const reference = channel(32, 0.4);
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abSwitch' as StereoPairProcessor,
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: reference,
        sampleRate: SR,
      }),
    ).toThrow();
    const sourceMismatch = () =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: channel(31, 0.2),
        referenceLeft: reference,
        referenceRight: reference,
        sampleRate: SR,
      });
    expect(sourceMismatch).toThrow(RangeError);
    expect(sourceMismatch).toThrow(
      /^masteringApplyPairProcessorStereo: Source left and right channel lengths must match\.$/,
    );
    const referenceMismatch = () =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: channel(31, 0.4),
        sampleRate: SR,
      });
    expect(referenceMismatch).toThrow(RangeError);
    expect(referenceMismatch).toThrow(
      /^masteringApplyPairProcessorStereo: Reference left and right channel lengths must match\.$/,
    );
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: null as never,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: reference,
        sampleRate: SR,
      }),
    ).toThrow(TypeError);
  });

  it('rejects embedded NULs before the C string boundary', () => {
    const source = channel(32, 0.2);
    const reference = channel(32, 0.4);
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade\0junk' as never,
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: reference,
        sampleRate: SR,
      }),
    ).toThrow(/NUL/);
    expect(() =>
      masteringApplyPairProcessorStereo({
        processor: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: reference,
        sampleRate: SR,
        params: { 'mix\0junk': 0 },
      }),
    ).toThrow(/NUL/);
  });

  it('shares one gain across stereo AB loudness matching', () => {
    const sourceLeft = channel(512, 0.1);
    const sourceRight = channel(512, -0.4);
    const referenceLeft = channel(768, 0.5);
    const referenceRight = channel(768, -0.6);
    const matched = masteringAbMatchLoudnessStereo({
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SR,
    });
    expect(matched.left).toHaveLength(sourceLeft.length);
    expect(matched.right).toHaveLength(sourceRight.length);
    const gain = 10 ** (matched.appliedGainDb / 20);
    expect(matched.left[200]).toBeCloseTo(sourceLeft[200] * gain, 5);
    expect(matched.right[200]).toBeCloseTo(sourceRight[200] * gain, 5);
    expect(matched.matchedTruePeakDbtp).toBeDefined();
    expect(matched.sampleRate).toBe(SR);
  });

  it('requires a sample rate for stereo loudness matching', () => {
    const source = channel(512, 0.1);
    expect(() =>
      (masteringAbMatchLoudnessStereo as (request: unknown) => unknown)({
        sourceLeft: source,
        sourceRight: channel(512, -0.1),
        referenceLeft: channel(768, 0.4),
        referenceRight: channel(768, -0.4),
      }),
    ).toThrow(TypeError);
  });
});

describe('StreamingMasteringChain.setParameter', () => {
  it('rejects updates after destroy', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 128,
      eq: { tilt: { enabled: true } },
    });
    chain.destroy();
    expect(() => chain.setParameter('eq.tilt.tiltDb', 3)).toThrow(/destroyed/);
  });

  it('requires an integer sample rate at construction and has no prepare()', () => {
    expect(() => new StreamingMasteringChain({ sampleRate: SR + 0.5, numChannels: 1 })).toThrow(
      RangeError,
    );
    expect(() => new StreamingMasteringChain({} as never)).toThrow(TypeError);
    const chain = new StreamingMasteringChain({ sampleRate: SR, numChannels: 1 });
    expect((chain as unknown as Record<string, unknown>).prepare).toBeUndefined();
    chain.destroy();
  });

  it('retains state when setting the current value at a block boundary', () => {
    const config = {
      sampleRate: SR,
      numChannels: 1 as const,
      maxBlockSize: 128,
      dynamics: { compressor: { enabled: true, thresholdDb: -24, ratio: 3 } },
    };
    const edited = new StreamingMasteringChain(config);
    const control = new StreamingMasteringChain(config);
    const first = channel(128, 0.7);
    edited.processMono(first);
    control.processMono(first);
    edited.setParameter('dynamics.compressor.thresholdDb', -24);
    const next = channel(128, 0.7, 0.1);
    expect(edited.processMono(next)).toEqual(control.processMono(next));
  });

  it('applies a changed value to the running stream', () => {
    const config = {
      sampleRate: SR,
      numChannels: 1 as const,
      maxBlockSize: 128,
      dynamics: { compressor: { enabled: true, thresholdDb: -24, ratio: 3 } },
    };
    const edited = new StreamingMasteringChain(config);
    const control = new StreamingMasteringChain(config);
    const first = channel(128, 0.7);
    expect(edited.processMono(first)).toEqual(control.processMono(first));
    edited.setParameter('dynamics.compressor.thresholdDb', -40);
    const next = channel(128, 0.7, 0.1);
    expect(edited.processMono(next)).not.toEqual(control.processMono(next));
  });

  it('rejects unknown and non-realtime parameters', () => {
    const chain = new StreamingMasteringChain({
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 128,
      dynamics: { compressor: { enabled: true } },
    });
    const disabled = new StreamingMasteringChain({
      sampleRate: SR,
      numChannels: 1,
      maxBlockSize: 128,
    });
    expect(() => chain.setParameter('does.not.exist', 1)).toThrow();
    expect(() => chain.setParameter('dynamics.compressor.enabled', 0)).toThrow();
    expect(() => disabled.setParameter('dynamics.compressor.thresholdDb', -24)).toThrow();
    expect(() => chain.setParameter('dynamics.compressor.thresholdDb', Number.NaN)).toThrow();
    expect(() => chain.setParameter('dynamics.compressor.thresholdDb\0junk', -24)).toThrow(/NUL/);
  });
});

describe('existing mono pair controls', () => {
  it('keeps the mono pair API available', () => {
    const source = channel(64, 0.2);
    const reference = channel(64, 0.4);
    expect(
      masteringApplyPairProcessor({
        processor: 'match.abCrossfade',
        source,
        reference,
        sampleRate: SR,
        params: { mix: 0 },
      }),
    ).toEqual(expect.objectContaining({ samples: source }));
  });
});
