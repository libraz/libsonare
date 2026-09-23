/**
 * The native RealtimeEngine embind object has no default for the trailing
 * render-frame argument on the transport/parameter/MIDI methods that take
 * one -- the "-1 = now" default lives only in the TS wrapper. These tests
 * exercise the raw `.native` object directly (bypassing the wrapper's own
 * `renderFrame = -1` default) to guard the omitted-argument path the
 * wrapper never reaches.
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

  it('play() with an omitted frame does not throw and matches passing -1', () => {
    const withDefault = new RealtimeEngine(48000, 256);
    const withExplicit = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(withDefault).play()).not.toThrow();
      expect(() => nativeOf(withExplicit).play(-1)).not.toThrow();
      withDefault.process([new Float32Array(256)]);
      withExplicit.process([new Float32Array(256)]);
      expect(nativeOf(withDefault).getTransportState().playing).toBe(true);
      expect(nativeOf(withDefault).getTransportState().playing).toBe(
        nativeOf(withExplicit).getTransportState().playing,
      );
    } finally {
      withDefault.destroy();
      withExplicit.destroy();
    }
  });

  it('stop() with an omitted frame does not throw and matches passing -1', () => {
    const withDefault = new RealtimeEngine(48000, 256);
    const withExplicit = new RealtimeEngine(48000, 256);
    try {
      nativeOf(withDefault).play();
      nativeOf(withExplicit).play(-1);
      withDefault.process([new Float32Array(256)]);
      withExplicit.process([new Float32Array(256)]);

      expect(() => nativeOf(withDefault).stop()).not.toThrow();
      expect(() => nativeOf(withExplicit).stop(-1)).not.toThrow();
      withDefault.process([new Float32Array(256)]);
      withExplicit.process([new Float32Array(256)]);
      expect(nativeOf(withDefault).getTransportState().playing).toBe(false);
      expect(nativeOf(withDefault).getTransportState().playing).toBe(
        nativeOf(withExplicit).getTransportState().playing,
      );
    } finally {
      withDefault.destroy();
      withExplicit.destroy();
    }
  });

  it('seekSample() with an omitted frame does not throw and matches passing -1', () => {
    const withDefault = new RealtimeEngine(48000, 256);
    const withExplicit = new RealtimeEngine(48000, 256);
    try {
      expect(() => nativeOf(withDefault).seekSample(100)).not.toThrow();
      expect(() => nativeOf(withExplicit).seekSample(100, -1)).not.toThrow();
      withDefault.process([new Float32Array(256)]);
      withExplicit.process([new Float32Array(256)]);
      expect(nativeOf(withDefault).getTransportState().samplePosition).toBe(100);
      expect(nativeOf(withDefault).getTransportState().samplePosition).toBe(
        nativeOf(withExplicit).getTransportState().samplePosition,
      );
    } finally {
      withDefault.destroy();
      withExplicit.destroy();
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
