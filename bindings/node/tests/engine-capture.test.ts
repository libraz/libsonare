import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

describe('RealtimeEngine native binding', () => {
  it('reads captured audio out of the capture buffer', () => {
    const engine = new RealtimeEngine(48000, 128);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);
    engine.armCapture();
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

  it('allocates capture storage from the canonical channel/count form', () => {
    const engine = new RealtimeEngine(48000, 128);
    engine.setCaptureBuffer(2, 128);
    engine.armCapture();
    engine.play();

    engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
    const captured = engine.capturedAudio();
    expect(captured).toHaveLength(2);
    expect(captured[0].length).toBe(128);
    expect(captured[1].length).toBe(128);
    expect(captured[0][0]).toBeCloseTo(0.25, 4);
    expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    expect(() => engine.setCaptureBuffer(0, 128)).toThrow(RangeError);
    expect(() => engine.setCaptureBuffer(2, 0)).toThrow(RangeError);
    engine.destroy();
  });

  it('sizes the canonical capture form inside the addon rather than in JS', () => {
    // The documented addon-owned form must allocate the capture planes exactly
    // once. Building them in JS and handing them over instead costs a second
    // full-size set that is copied in and dropped, so a 30-minute stereo capture
    // peaks at twice the buffer it asked for.
    const channels = 2;
    const capacityFrames = 4_000_000;
    const jsBytesIfAllocated = channels * capacityFrames * Float32Array.BYTES_PER_ELEMENT;

    const engine = new RealtimeEngine(48000, 128);
    try {
      // Node accounts JS-owned ArrayBuffers separately from native allocations,
      // so a plane built on this side lands in the delta and a plane the addon
      // sized does not. The threshold is a quarter of the full set: the two
      // outcomes are 32 MB apart, not marginally different.
      const before = process.memoryUsage().arrayBuffers;
      engine.setCaptureBuffer(channels, capacityFrames);
      const after = process.memoryUsage().arrayBuffers;
      expect(after - before).toBeLessThan(jsBytesIfAllocated / 4);

      // ...and what it sized is what capture writes into, so the assertion above
      // is not passing because the call did nothing.
      engine.armCapture();
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

  it('validates the canonical capture extent in the addon, not only in the facade', () => {
    const engine = new RealtimeEngine(48000, 128);
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
      engine.armCapture();
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

  it('keeps capture storage valid when the source ArrayBuffers are transferred', () => {
    const engine = new RealtimeEngine(48000, 128);
    const captureLeft = new Float32Array(128);
    const captureRight = new Float32Array(128);
    engine.setCaptureBuffer([captureLeft, captureRight]);

    structuredClone(captureLeft, { transfer: [captureLeft.buffer] });
    structuredClone(captureRight, { transfer: [captureRight.buffer] });
    expect(captureLeft.byteLength).toBe(0);
    expect(captureRight.byteLength).toBe(0);

    engine.armCapture();
    engine.play();
    engine.process([new Float32Array(128).fill(0.25), new Float32Array(128).fill(-0.25)]);
    const captured = engine.capturedAudio();
    expect(captured[0][0]).toBeCloseTo(0.25, 4);
    expect(captured[1][0]).toBeCloseTo(-0.25, 4);
    engine.destroy();
  });
});
