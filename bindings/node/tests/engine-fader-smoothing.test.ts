/**
 * A step on a reserved target settles on the target's fixed 5 ms guard; `smoothed: true` ramps over
 * the engine smoothing time, and the guard under it adds a small lag.
 */

import { describe, expect, it } from 'vitest';
import { RealtimeEngine } from '../src/index.js';

const SR = 48000;
const BLOCK = 240;
const SMOOTHING_MS = 200;
const GUARD_MS = 5;
const TO_DB = -20;

/** One lane of full-scale DC whose fader a step has set to 0 dB, so a ramp has a base to leave. */
function faderRig(): { level: (ms: number) => number; set: (smoothed: boolean) => void } {
  const engine = new RealtimeEngine({ sampleRate: SR, maxBlockSize: BLOCK });
  const frames = SR * 2;
  engine.setClips([
    {
      id: 1,
      trackId: 10,
      channels: [new Float32Array(frames).fill(1)],
      startPpq: 0,
      lengthSamples: frames,
    },
  ]);
  engine.setTrackLanes([{ trackId: 10 }]);
  engine.setParamSmoothingMs(SMOOTHING_MS);
  const fader = engine.resolveStripAutomationId(10, 'faderDb');
  engine.play();
  engine.setParameter(fader, 0);
  const trace: number[] = [];
  const run = (blocks: number) => {
    for (let i = 0; i < blocks; i += 1) {
      const [left] = engine.process([new Float32Array(BLOCK), new Float32Array(BLOCK)]);
      trace.push(...left);
    }
  };
  run(20);
  const base = trace[trace.length - 1];
  trace.length = 0;
  return {
    set: (smoothed) => {
      engine.setParameter(fader, TO_DB, undefined, smoothed ? { smoothed: true } : undefined);
      run(Math.ceil((SMOOTHING_MS * 2 * SR) / 1000 / BLOCK));
      engine.destroy();
    },
    level: (ms) => trace[Math.round((ms * SR) / 1000) - 1] / base,
  };
}

const toDb = (gain: number) => 20 * Math.log10(gain);

describe('a reserved fader target under setParameter', () => {
  it('ramps a smoothed set over the smoothing time, cascaded with the 5 ms guard', () => {
    const rig = faderRig();
    rig.set(true);
    // One time constant covers 1 - 1/e of the move in dB. The guard lags the ramp by about its
    // own time times the ramp's slope, so the band is three times that lag.
    const expected = TO_DB * (1 - Math.exp(-1));
    const guardLag = (-TO_DB / SMOOTHING_MS) * Math.exp(-1) * GUARD_MS;
    expect(Math.abs(toDb(rig.level(SMOOTHING_MS)) - expected)).toBeLessThan(3 * guardLag);
    // Ten guard time constants in, the ramp has covered under a quarter of the move.
    expect(toDb(rig.level(10 * GUARD_MS)) / TO_DB).toBeLessThan(0.25);
  });

  it('de-zippers a step over the fixed 5 ms guard, whatever the smoothing time', () => {
    const rig = faderRig();
    rig.set(false);
    // The guard glides the linear gain: 1 - 1/e of the way at one guard time constant.
    const target = 10 ** (TO_DB / 20);
    const covered = (1 - rig.level(GUARD_MS)) / (1 - target);
    expect(Math.abs(covered - (1 - Math.exp(-1)))).toBeLessThan(0.02);
    expect(rig.level(10 * GUARD_MS)).toBeCloseTo(target, 4);
  });
});
