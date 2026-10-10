/**
 * The native RealtimeEngine embind object has no default for the trailing
 * render-frame argument on the transport/parameter/MIDI methods that take
 * one -- omission means immediate, and a negative frame is refused. These
 * tests exercise the raw `.native` object directly to guard the paths the
 * TS wrapper's own resolution never reaches.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

interface NativeTransport {
  play: (renderFrame?: number) => void;
  stop: (renderFrame?: number) => void;
  seekSample: (timelineSample: number, renderFrame?: number) => void;
  pushMidiPanic: (renderFrame?: number) => void;
  getTransportState: () => { playing: boolean; samplePosition: number };
}

function nativeOf(engine: RealtimeEngine): NativeTransport {
  return (engine as unknown as { native: NativeTransport }).native;
}

describe('RealtimeEngine native render-frame default', () => {
  beforeAll(async () => {
    await init();
  });

  it('play() with an omitted frame applies immediately', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(engine).play()).not.toThrow();
      engine.process([new Float32Array(256)]);
      expect(nativeOf(engine).getTransportState().playing).toBe(true);
    } finally {
      engine.destroy();
    }
  });

  it('stop() with an omitted frame applies immediately', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      nativeOf(engine).play();
      engine.process([new Float32Array(256)]);
      expect(() => nativeOf(engine).stop()).not.toThrow();
      engine.process([new Float32Array(256)]);
      expect(nativeOf(engine).getTransportState().playing).toBe(false);
    } finally {
      engine.destroy();
    }
  });

  it('seekSample() with an omitted frame applies immediately', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(engine).seekSample(100)).not.toThrow();
      engine.process([new Float32Array(256)]);
      expect(nativeOf(engine).getTransportState().samplePosition).toBe(100);
    } finally {
      engine.destroy();
    }
  });

  it('refuses a negative frame instead of reading it as immediate', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(engine).play(-1)).toThrow(RangeError);
      expect(() => nativeOf(engine).stop(-1)).toThrow(/renderFrame/);
      expect(() => nativeOf(engine).seekSample(0, -1)).toThrow(/renderFrame/);
      engine.process([new Float32Array(256)]);
      expect(nativeOf(engine).getTransportState().playing).toBe(false);
    } finally {
      engine.destroy();
    }
  });

  it('pushMidiPanic() with an omitted frame does not throw', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(engine).pushMidiPanic()).not.toThrow();
    } finally {
      engine.destroy();
    }
  });

  it('still refuses a non-integer or non-finite render frame', () => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(engine).play(1.5)).toThrow(/renderFrame/);
      expect(() => nativeOf(engine).stop(Number.NaN)).toThrow(/renderFrame/);
      expect(() => nativeOf(engine).seekSample(0, Number.POSITIVE_INFINITY)).toThrow(/renderFrame/);
    } finally {
      engine.destroy();
    }
  });
});
