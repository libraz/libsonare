import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  it('reads captured audio out of the capture buffer', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.configureCapture({ capacityFrames: 128 });
    engine.armCapture({ armed: true });
    engine.play();

    engine.process([new Float32Array(128).fill(0.5), new Float32Array(128).fill(-0.5)]);
    const status = engine.captureStatus();
    expect(status.capturedFrames).toBe(128);

    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.5, 4);
    expect(captured[1][0]).toBeCloseTo(-0.5, 4);
    engine.destroy();
  });

  it('reports accumulated dropped capture frames and resets them', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 16 });
    try {
      engine.configureCapture({ capacityFrames: 4, numChannels: 1, source: 'input' });
      engine.armCapture({ armed: true });
      engine.play();

      const block = [new Float32Array(8).fill(0.25)];
      engine.process(block);
      const first = engine.captureStatus().droppedFrames;
      expect(typeof first).toBe('number');
      expect(Number.isSafeInteger(first)).toBe(true);
      expect(first).toBe(4);

      engine.process(block);
      expect(engine.captureStatus().droppedFrames).toBe(12);

      engine.armCapture({ armed: false });
      engine.resetCapture();
      expect(engine.captureStatus().droppedFrames).toBe(0);
    } finally {
      engine.destroy();
    }
  });

  it('allocates capture storage from the channel count and capacity', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    engine.configureCapture({ capacityFrames: 128, numChannels: 2 });
    engine.armCapture({ armed: true });
    engine.play();

    engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.25, 4);
    expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    engine.armCapture({ armed: false });
    expect(() => engine.configureCapture({ capacityFrames: 128, numChannels: 0 })).toThrow(
      RangeError,
    );
    expect(() => engine.configureCapture({ capacityFrames: 0 })).toThrow(RangeError);
    expect(() => engine.configureCapture({ capacityFrames: 128, numChannel: 2 } as never)).toThrow(
      /unknown key numChannel \(expected /,
    );
    engine.destroy();
  });

  it('sizes the capture storage inside the addon rather than in JS', () => {
    // The documented addon-owned form must allocate the capture planes exactly
    // once. Building them in JS and handing them over instead costs a second
    // full-size set that is copied in and dropped, so a 30-minute stereo capture
    // peaks at twice the buffer it asked for.
    const channels = 2;
    const capacityFrames = 4_000_000;
    const jsBytesIfAllocated = channels * capacityFrames * Float32Array.BYTES_PER_ELEMENT;

    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      // Node accounts JS-owned ArrayBuffers separately from native allocations,
      // so a plane built on this side lands in the delta and a plane the addon
      // sized does not. The threshold is a quarter of the full set: the two
      // outcomes are 32 MB apart, not marginally different.
      const before = process.memoryUsage().arrayBuffers;
      engine.configureCapture({ capacityFrames, numChannels: channels });
      const after = process.memoryUsage().arrayBuffers;
      expect(after - before).toBeLessThan(jsBytesIfAllocated / 4);

      // ...and what it sized is what capture writes into, so the assertion above
      // is not passing because the call did nothing.
      engine.armCapture({ armed: true });
      engine.play();
      engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
      expect(engine.captureStatus().capturedFrames).toBe(128);
      const captured = engine.capturedAudio();
      expect(captured).toHaveLength(channels);
      expect(captured[0][0]).toBeCloseTo(0.25, 4);
      expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    } finally {
      engine.destroy();
    }
  });

  it('validates the capture extent in the addon, not only in the facade', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    const native = (
      engine as unknown as { native: { setCaptureBuffer: (...args: unknown[]) => void } }
    ).native;
    try {
      // Reached past the facade's own RangeError guard: the addon arity has to
      // stand on its own, or that guard is the only thing between a caller and a
      // zero-sized capture buffer.
      expect(() => native.setCaptureBuffer(2, 0)).toThrow(RangeError);
      expect(() => native.setCaptureBuffer(0, 128)).toThrow(RangeError);
      expect(() => native.setCaptureBuffer(2)).toThrow(TypeError);

      // The same arity, used directly, captures exactly as the array form does.
      native.setCaptureBuffer(2, 128);
      engine.armCapture({ armed: true });
      engine.play();
      engine.process([new Float32Array(128).fill(0.5), new Float32Array(128).fill(-0.5)]);
      const captured = engine.capturedAudio();
      expect(captured).toHaveLength(2);
      expect(captured[0].length).toBe(128);
      expect(captured[0][0]).toBeCloseTo(0.5, 4);
      expect(captured[1][0]).toBeCloseTo(-0.5, 4);
    } finally {
      engine.destroy();
    }
  });

  it('resizes the buffer only when its shape changes and refuses a resize while armed', () => {
    const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
    try {
      engine.configureCapture({ capacityFrames: 128 });
      engine.armCapture({ armed: true });
      engine.play();
      engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
      // Same shape: the captured frames survive a re-configuration.
      engine.configureCapture({ capacityFrames: 128, source: 'output' });
      expect(engine.captureStatus().capturedFrames).toBe(128);
      // A different shape would reallocate under a running capture.
      let refused: unknown;
      try {
        engine.configureCapture({ capacityFrames: 256 });
      } catch (error) {
        refused = error;
      }
      expect(isSonareError(refused)).toBe(true);
      expect((refused as { code: number }).code).toBe(ErrorCode.InvalidState);
    } finally {
      engine.destroy();
    }
  });
});
