import { beforeAll, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

beforeAll(async () => {
  await init();
});

it('lane pre-fader sends retain their level when the strip fader is lowered', () => {
  const renderBusPeak = (sendTiming: 'preFader' | 'postFader', faderDb: number): number => {
    const engine = new RealtimeEngine(48000, 256);
    try {
      engine.setTrackBuses([{ busId: 1, gainDb: 0 }]);
      engine.setTrackLanes([{ trackId: 10, sends: [{ busId: 1, sendTiming }] }]);
      engine.setTrackStripJson(
        10,
        JSON.stringify({
          version: 1,
          strips: [{ id: 'track-10', faderDb, panLaw: 3 }],
          buses: [],
          connections: [],
        }),
      );
      engine.setClips([
        {
          id: 1,
          trackId: 10,
          channels: [new Float32Array(256 * 24).fill(0.5)],
          startPpq: 0,
          lengthSamples: 256 * 24,
        },
      ]);
      engine.play();
      let peak = -120;
      for (let block = 0; block < 20; ++block) {
        engine.process([new Float32Array(256)]);
        for (const record of engine.drainMeterTelemetry()) {
          if (record.targetId === 33) {
            peak = record.peakDbL;
          }
        }
      }
      return peak;
    } finally {
      engine.destroy();
    }
  };
  const reference = renderBusPeak('preFader', 0);
  const pre = renderBusPeak('preFader', -60);
  const postReference = renderBusPeak('postFader', 0);
  const post = renderBusPeak('postFader', -60);
  expect(reference).toBeGreaterThan(-12);
  expect(pre).toBeCloseTo(reference, 3);
  // Compare each tap to its own unity-fader reference: the post-fader tap
  // also includes pan/channel conversion gain that the pre-fader tap omits.
  expect(postReference).toBeGreaterThan(-12);
  expect(postReference - post).toBeCloseTo(60, 2);
});
