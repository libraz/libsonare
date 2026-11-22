/**
 * configureCapture sets the record offset and the punch window in one engine call, so a looped
 * punch setup is checked against the state it produces rather than refused halfway.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../src/index';

beforeAll(async () => init());

/** An engine looping over one bar with a capture buffer armed, and the loop's length in samples. */
function armedLoop() {
  const engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
  engine.configureCapture({ capacityFrames: 128 });
  engine.setLoop({ startPpq: 0, endPpq: 4 });
  engine.armCapture({ armed: true });
  return { engine, loop: engine.sampleAtPpq(4) - engine.sampleAtPpq(0) };
}

const PUNCH = { startSample: 0, endSample: 1000 };

describe('configureCapture applies the record offset and the punch window together', () => {
  it('accepts an offset of exactly one loop pass and refuses one sample more, leaving the engine as it was', () => {
    const { engine, loop } = armedLoop();
    try {
      engine.configureCapture({ capacityFrames: 128, recordOffsetSamples: loop, punch: PUNCH });
      expect(engine.captureStatus()).toMatchObject({
        recordOffsetSamples: loop,
        punchEnabled: true,
      });
      expect(() =>
        engine.configureCapture({
          capacityFrames: 128,
          recordOffsetSamples: loop + 1,
          punch: PUNCH,
        }),
      ).toThrow(RangeError);
      expect(() =>
        engine.configureCapture({
          capacityFrames: 128,
          recordOffsetSamples: loop + 1,
          punch: PUNCH,
        }),
      ).toThrow(
        `RealtimeEngine.configureCapture: recordOffsetSamples must be at most the loop length, ${loop} samples, while capture is armed with punch on`,
      );
      expect(engine.captureStatus()).toMatchObject({
        recordOffsetSamples: loop,
        punchEnabled: true,
      });
    } finally {
      engine.destroy();
    }
  });

  it('moves between setups whichever half a step-by-step setup would have set first', () => {
    const { engine, loop } = armedLoop();
    try {
      // Punch off: the offset has no effect, so ten passes are accepted.
      engine.configureCapture({ capacityFrames: 128, recordOffsetSamples: 10 * loop });
      // Punch on with a short offset: setting the window first would meet the long offset.
      engine.configureCapture({ capacityFrames: 128, recordOffsetSamples: 64, punch: PUNCH });
      expect(engine.captureStatus()).toMatchObject({ recordOffsetSamples: 64, punchEnabled: true });
      // Punch off with a long offset: setting the offset first would meet the window still on.
      engine.configureCapture({ capacityFrames: 128, recordOffsetSamples: 10 * loop });
      expect(engine.captureStatus()).toMatchObject({
        recordOffsetSamples: 10 * loop,
        punchEnabled: false,
      });
    } finally {
      engine.destroy();
    }
  });

  it('leaves the offset untouched when the punch window is refused', () => {
    const { engine } = armedLoop();
    try {
      engine.configureCapture({ capacityFrames: 128, recordOffsetSamples: 64, punch: PUNCH });
      expect(() =>
        engine.configureCapture({
          capacityFrames: 128,
          recordOffsetSamples: 128,
          punch: { startSample: 100, endSample: 50 },
        }),
      ).toThrow(
        'RealtimeEngine.configureCapture: punch.endSample must not be before punch.startSample',
      );
      expect(engine.captureStatus()).toMatchObject({ recordOffsetSamples: 64, punchEnabled: true });
    } finally {
      engine.destroy();
    }
  });
});
