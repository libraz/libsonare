import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

// The live input is routed into a track lane by one setter family, and the dry input monitor is
// engine state with one owner: neither moves when capture is configured.
const SR = 48000;
const BLOCK = 128;

beforeAll(async () => {
  await init();
});

function withEngine(fn: (engine: RealtimeEngine) => void): void {
  const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
  try {
    fn(engine);
  } finally {
    engine.destroy();
  }
}

function refusal(call: () => unknown): Error {
  try {
    call();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected a refusal');
}

describe('input track', () => {
  it('sets, moves, clears and reads the lane the live input feeds', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 1 }, { trackId: 2 }]);
      expect(engine.inputTrack).toBeUndefined();
      engine.setInputTrack(1);
      expect(engine.inputTrack).toBe(1);
      engine.setInputTrack(2);
      expect(engine.inputTrack).toBe(2);
      engine.clearInputTrack();
      expect(engine.inputTrack).toBeUndefined();
    });
  });

  it('refuses an id no lane carries and leaves the routing as it was', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 1 }]);
      engine.setInputTrack(1);
      const error = refusal(() => engine.setInputTrack(7));
      expect(error).toBeInstanceOf(RangeError);
      expect(error.message).toContain('trackId must be the id of a track lane in this engine');
      expect(engine.inputTrack).toBe(1);
      expect(refusal(() => engine.setInputTrack(-1))).toBeInstanceOf(RangeError);
    });
  });

  it('clears the input track when the lane list drops its lane', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 1 }, { trackId: 2 }]);
      engine.setInputTrack(2);
      engine.setTrackLanes([{ trackId: 1 }]);
      expect(engine.inputTrack).toBeUndefined();
    });
  });
});

describe('input monitor', () => {
  it('reads back what the setter wrote, defaulting the gain to 1', () => {
    withEngine((engine) => {
      expect(engine.inputMonitor()).toEqual({ enabled: true, gain: 1 });
      engine.setInputMonitor({ enabled: false });
      expect(engine.inputMonitor()).toEqual({ enabled: false, gain: 1 });
      engine.setInputMonitor({ enabled: true, gain: 0.5 });
      expect(engine.inputMonitor()).toEqual({ enabled: true, gain: 0.5 });
      // A negative linear gain inverts the polarity; it is accepted, not refused.
      engine.setInputMonitor({ enabled: true, gain: -0.5 });
      expect(engine.inputMonitor()).toEqual({ enabled: true, gain: -0.5 });
    });
  });

  it('round-trips the reader through the setter', () => {
    withEngine((engine) => {
      engine.setInputMonitor({ enabled: false, gain: 0.25 });
      const read = engine.inputMonitor();
      engine.setInputMonitor(read);
      expect(engine.inputMonitor()).toEqual(read);
    });
  });

  it('is left alone by configureCapture, which refuses the inputMonitor key', () => {
    withEngine((engine) => {
      engine.setInputMonitor({ enabled: false });
      engine.configureCapture({ capacityFrames: 256, source: 'input' });
      expect(engine.inputMonitor()).toEqual({ enabled: false, gain: 1 });
      engine.configureCapture({ capacityFrames: 256 });
      expect(engine.inputMonitor()).toEqual({ enabled: false, gain: 1 });
      const error = refusal(() =>
        engine.configureCapture({ capacityFrames: 256, inputMonitor: { enabled: true } } as never),
      );
      expect(error).toBeInstanceOf(TypeError);
      expect(error.message).toContain('unknown key inputMonitor');
      expect(engine.inputMonitor()).toEqual({ enabled: false, gain: 1 });
    });
  });

  it('refuses a non-finite gain by name and keeps the monitor', () => {
    withEngine((engine) => {
      engine.setInputMonitor({ enabled: false, gain: 0.5 });
      for (const gain of [Number.NaN, Number.POSITIVE_INFINITY]) {
        const error = refusal(() => engine.setInputMonitor({ enabled: true, gain }));
        expect(error).toBeInstanceOf(RangeError);
        expect(error.message).toMatch(/gain/);
      }
      expect(refusal(() => engine.setInputMonitor({ enabled: 1 as never }))).toBeInstanceOf(
        TypeError,
      );
      expect(engine.inputMonitor()).toEqual({ enabled: false, gain: 0.5 });
    });
  });
});

describe('the live input through a lane', () => {
  it('reaches the output through the input track with the dry monitor off', () => {
    withEngine((engine) => {
      engine.setTrackLanes([{ trackId: 1 }]);
      engine.setInputMonitor({ enabled: false });
      const input = () => [new Float32Array(BLOCK).fill(0.25), new Float32Array(BLOCK).fill(0.25)];
      const peak = (planes: Float32Array[]) =>
        Math.max(...planes.map((plane) => Math.max(...plane.map(Math.abs))));
      expect(peak(engine.process(input()))).toBe(0);
      engine.setInputTrack(1);
      // The 5 ms ramp spans two blocks at 48 kHz; the third is past it.
      engine.process(input());
      engine.process(input());
      expect(peak(engine.process(input()))).toBeGreaterThan(0.1);
    });
  });
});
