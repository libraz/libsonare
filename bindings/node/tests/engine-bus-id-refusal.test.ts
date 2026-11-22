/**
 * An output bus id names its own path in every refusal, and an explicit `0` says what omitting it
 * does. The three entry points taking one share the text.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../dist/index.js';

type Entry = [string, string, (engine: RealtimeEngine, busId: number) => void];

const ENTRIES: Entry[] = [
  [
    'setTrackLanes',
    'lanes[0].outputBusId',
    (e, v) => e.setTrackLanes([{ trackId: 1, outputBusId: v }]),
  ],
  [
    'setTrackBuses',
    'buses[0].outputBusId',
    (e, v) => e.setTrackBuses([{ busId: 1, outputBusId: v }]),
  ],
  ['setTrackOutputBus', 'busId', (e, v) => e.setTrackOutputBus(1, v)],
];

function refusal(run: (engine: RealtimeEngine) => void): unknown {
  const engine = new RealtimeEngine({ sampleRate: 48000 });
  try {
    engine.setTrackBuses([{ busId: 2 }]);
    engine.setTrackLanes([{ trackId: 1 }]);
    run(engine);
    return undefined;
  } catch (error) {
    return error;
  } finally {
    engine.destroy();
  }
}

describe('RealtimeEngine output bus id refusals', () => {
  it.each(ENTRIES)('%s refuses an id outside the uint32 range by its path', (name, path, call) => {
    for (const value of [Number.NaN, Number.POSITIVE_INFINITY, 1.5, -1, 2 ** 32]) {
      const error = refusal((engine) => call(engine, value));
      expect(error, `${name} ${value}`).toBeInstanceOf(RangeError);
      expect((error as Error).message).toBe(
        `RealtimeEngine.${name}: ${path} must be an integer in [1, 4294967295]`,
      );
    }
  });

  it.each(ENTRIES)(
    '%s refuses an explicit 0 by saying what omitting it does',
    (name, path, call) => {
      const error = refusal((engine) => call(engine, 0));
      expect(error).toBeInstanceOf(RangeError);
      expect((error as Error).message).toBe(
        `RealtimeEngine.${name}: ${path} must not be 0; omit it to route to the master mix`,
      );
    },
  );
});
