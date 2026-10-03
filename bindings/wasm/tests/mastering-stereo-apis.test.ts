import { beforeAll, describe, expect, it } from 'vitest';
import {
  init,
  masteringAbMatchLoudnessStereo,
  masteringPairProcessStereo,
  StreamingMasteringChain,
} from '../dist/index.js';
import type { StereoPairProcessor } from '../src/index.js';

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
    const chain = new StreamingMasteringChain({ 'eq.tilt.enabled': true });
    chain.prepare(SAMPLE_RATE, 128, 1);
    chain.delete();
    expect(() => chain.setParameter('eq.tilt.tiltDb', 3)).toThrow(/deleted object/);
  });

  it('applies one crossfade to both channels and supports request and positional forms', () => {
    const sourceLeft = tone(128, 0.2);
    const sourceRight = tone(128, -0.35, 0.7);
    const referenceLeft = tone(192, 0.8);
    const referenceRight = tone(192, 0.45, -0.4);
    const request = masteringPairProcessStereo({
      processorName: 'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      sampleRate: SAMPLE_RATE,
      params: { mix: 0.25 },
    });
    const positional = masteringPairProcessStereo(
      'match.abCrossfade',
      sourceLeft,
      sourceRight,
      referenceLeft,
      referenceRight,
      SAMPLE_RATE,
      { mix: 0.25 },
    );
    expect(request).toEqual(positional);
    expect(request.left).toHaveLength(sourceLeft.length);
    expect(request.right).toHaveLength(sourceRight.length);
    expect(request.left[32]).toBeCloseTo(0.75 * sourceLeft[32] + 0.25 * referenceLeft[32], 5);
    expect(request.right[32]).toBeCloseTo(0.75 * sourceRight[32] + 0.25 * referenceRight[32], 5);
  });

  it('resolves the default sample rate identically in request and positional forms', () => {
    const source = tone(128, 0.2);
    const reference = tone(128, 0.4);
    const request = masteringPairProcessStereo({
      processorName: 'match.abCrossfade',
      sourceLeft: source,
      sourceRight: source,
      referenceLeft: reference,
      referenceRight: reference,
    });
    const positional = masteringPairProcessStereo(
      'match.abCrossfade',
      source,
      source,
      reference,
      reference,
    );
    expect(request).toEqual(positional);
    expect(request.sampleRate).toBe(22050);
  });

  it('raises the same errors from request and positional forms', () => {
    const source = tone(32, 0.2);
    const reference = tone(32, 0.4);
    const short = tone(31, 0.2);
    const cases: Array<[StereoPairProcessor, Float32Array, Float32Array, RegExp]> = [
      ['match.abCrossfade', source, short, /Source left and right channel lengths must match/],
      ['match.abCrossfade', short, source, /Source left and right channel lengths must match/],
      ['match.abSwitch' as StereoPairProcessor, source, source, /abSwitch/],
    ];
    for (const [processorName, sourceLeft, sourceRight, message] of cases) {
      expect(() =>
        masteringPairProcessStereo({
          processorName,
          sourceLeft,
          sourceRight,
          referenceLeft: reference,
          referenceRight: reference,
          sampleRate: SAMPLE_RATE,
        }),
      ).toThrow(message);
      expect(() =>
        masteringPairProcessStereo(
          processorName,
          sourceLeft,
          sourceRight,
          reference,
          reference,
          SAMPLE_RATE,
        ),
      ).toThrow(message);
    }
    expect(() =>
      masteringPairProcessStereo({
        processorName: 'match.abCrossfade',
        sourceLeft: source,
        sourceRight: source,
        referenceLeft: reference,
        referenceRight: short,
        sampleRate: SAMPLE_RATE,
      }),
    ).toThrow(/Reference left and right channel lengths must match/);
    expect(() =>
      masteringPairProcessStereo(
        'match.abCrossfade',
        source,
        source,
        reference,
        short,
        SAMPLE_RATE,
      ),
    ).toThrow(/Reference left and right channel lengths must match/);
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
      'dynamics.compressor.enabled': true,
      'dynamics.compressor.thresholdDb': -24,
      'dynamics.compressor.ratio': 3,
    };
    const edited = new StreamingMasteringChain(config);
    const control = new StreamingMasteringChain(config);
    try {
      edited.prepare(SAMPLE_RATE, 128, 1);
      control.prepare(SAMPLE_RATE, 128, 1);
      const first = tone(128, 0.7);
      expect(edited.processMono(first)).toEqual(control.processMono(first));
      edited.setParameter('dynamics.compressor.thresholdDb', -40);
      const next = tone(128, 0.7, 0.1);
      expect(edited.processMono(next)).not.toEqual(control.processMono(next));
    } finally {
      edited.delete();
      control.delete();
    }
  });

  it('exposes the streaming chain realtime-safe setter', () => {
    const chain = new StreamingMasteringChain({
      'dynamics.compressor.enabled': true,
      'dynamics.compressor.thresholdDb': -24,
    });
    try {
      chain.prepare(SAMPLE_RATE, 128, 1);
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
      chain.delete();
    }
  });
});
