import { beforeAll, describe, expect, it } from 'vitest';
import {
  createVocalEditSession,
  init,
  masteringRepairTrimSilence,
  RealtimeEngine,
} from '../src/index';

beforeAll(async () => init());

const REFUSAL = Symbol.for('libsonare.refusal');

interface AttachedRefusal {
  subjects: string[];
  text: string;
}

function refusalFrom(run: () => unknown): { error: Error; refusal: AttachedRefusal | undefined } {
  try {
    run();
  } catch (error) {
    const refusal = (error as Record<symbol, unknown>)[REFUSAL] as AttachedRefusal | undefined;
    return { error: error as Error, refusal };
  }
  throw new Error('expected a refusal');
}

describe('a bound given in seconds is refused under the seconds name', () => {
  const tone = new Float32Array(48000).fill(0.1);

  it('names paddingSec and its range in seconds', () => {
    const { error, refusal } = refusalFrom(() =>
      masteringRepairTrimSilence({ samples: tone, sampleRate: 48000, paddingSec: 30 }),
    );
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(
      'masteringRepairTrimSilence: paddingSec must be finite and in [0, 20]',
    );
    expect(refusal?.subjects).toEqual(['paddingSec']);
  });

  it('keeps paddingSamples when the caller wrote samples', () => {
    const { error, refusal } = refusalFrom(() =>
      masteringRepairTrimSilence({ samples: tone, sampleRate: 48000, paddingSamples: 48000 * 30 }),
    );
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(
      'masteringRepairTrimSilence: paddingSamples must be an integer in [0, 960000]',
    );
    expect(refusal?.subjects).toEqual(['paddingSamples']);
  });

  it('names both punch bounds in seconds in a relation', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      const inverted = refusalFrom(() =>
        engine.configureCapture({ capacityFrames: 128, punch: { startSec: 0.5, endSec: 0.1 } }),
      );
      expect(inverted.error).toBeInstanceOf(RangeError);
      expect(inverted.error.message).toBe(
        'RealtimeEngine.configureCapture: punch.endSec must not be before punch.startSec',
      );
      expect(inverted.refusal?.subjects).toEqual(['punch.endSec', 'punch.startSec']);
      const mixed = refusalFrom(() =>
        engine.configureCapture({
          capacityFrames: 128,
          punch: { startSec: 0.5, endSample: 4800 },
        }),
      );
      expect(mixed.error.message).toBe(
        'RealtimeEngine.configureCapture: punch.endSample must not be before punch.startSec',
      );
    } finally {
      engine.destroy();
    }
  });

  describe('a vocal render range given in seconds', () => {
    let snapshot: ReturnType<ReturnType<typeof createVocalEditSession>['captureRenderSnapshot']>;
    beforeAll(() => {
      const sampleRate = 16000;
      const length = 4096;
      const frames = Math.ceil(length / 128) + 1;
      snapshot = createVocalEditSession({
        samples: Float32Array.from(
          { length },
          (_, i) => 0.25 * Math.sin((2 * Math.PI * 440 * i) / sampleRate),
        ),
        sampleRate,
        frameLengthSamples: 256,
        hopLengthSamples: 128,
        analysis: {
          frameOriginSample: 0,
          samplesPerFrame: 128,
          frameLengthSamples: 256,
          f0Hz: new Float32Array(frames).fill(440),
          voiced: new Uint8Array(frames).fill(1),
          algorithmId: 'libsonare.pyin',
          algorithmVersion: 1,
        },
      }).captureRenderSnapshot();
    });

    it('names an out-of-bounds end in seconds, synchronously and asynchronously', async () => {
      const range = { startSec: 0, endSec: 10 };
      const text = 'range.endSec must be finite and in [0, 0.256]';
      const sync = refusalFrom(() => snapshot.render({ range }));
      expect(sync.error).toBeInstanceOf(RangeError);
      expect(sync.error.message).toBe(`VocalRenderSnapshot.render: ${text}`);
      expect(sync.refusal?.subjects).toEqual(['range.endSec']);
      const rejected = await snapshot.renderAsync({ range }).then(
        () => undefined,
        (error: unknown) => error as Error,
      );
      expect(rejected).toBeInstanceOf(RangeError);
      expect(rejected?.message).toBe(`VocalRenderSnapshot.renderAsync: ${text}`);
      expect((rejected as unknown as Record<symbol, AttachedRefusal>)[REFUSAL].subjects).toEqual([
        'range.endSec',
      ]);
    });

    it('names an inverted range in seconds, asynchronously too', async () => {
      const range = { startSec: 0.2, endSec: 0.1 };
      const text = 'range.endSec must be greater than range.startSec';
      expect(() => snapshot.render({ range })).toThrow(`VocalRenderSnapshot.render: ${text}`);
      await expect(snapshot.renderAsync({ range })).rejects.toThrow(
        `VocalRenderSnapshot.renderAsync: ${text}`,
      );
    });
  });
});
