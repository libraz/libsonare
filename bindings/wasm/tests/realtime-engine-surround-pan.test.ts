/**
 * Live surround placement of a track lane on a 5.1 destination: the output
 * planes follow `setTrackStripSurroundPan`, glide rather than click, and bad
 * input is refused like the neighbouring strip pan setters.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { ErrorCode, init, isSonareError, RealtimeEngine } from '../dist/index.js';

const SR = 48000;
const BLOCK = 256;
const PLANES = 6;
const TRACK = 10;
const SOURCE_AMPLITUDE = 0.5;
// Canonical 5.1 plane order.
const [PLANE_L, _PLANE_R, PLANE_C, PLANE_LFE, PLANE_LS, PLANE_RS] = [0, 1, 2, 3, 4, 5];

function makeEngine(): RealtimeEngine {
  const engine = new RealtimeEngine(SR, BLOCK);
  const frames = BLOCK * 64;
  engine.setClips([
    {
      id: 1,
      trackId: TRACK,
      channels: [new Float32Array(frames).fill(SOURCE_AMPLITUDE)],
      startPpq: 0,
      lengthSamples: frames,
    },
  ]);
  engine.setTrackLanes([TRACK]);
  engine.setTrackStripJson(
    TRACK,
    '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
  );
  return engine;
}

function renderBlock(engine: RealtimeEngine): Float32Array[] {
  return engine.process(Array.from({ length: PLANES }, () => new Float32Array(BLOCK)));
}

function energy(plane: Float32Array): number {
  let sum = 0;
  for (const value of plane) {
    sum += value * value;
  }
  return sum;
}

function framePower(planes: Float32Array[], frame: number): number {
  let sum = 0;
  for (const plane of planes) {
    sum += plane[frame] * plane[frame];
  }
  return sum;
}

describe('RealtimeEngine.setTrackStripSurroundPan (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('places the lane in Ls, then glides it to Rs with constant-power bounds', () => {
    const engine = makeEngine();
    try {
      engine.setTrackStripSurroundPan(TRACK, { azimuth: -110 });
      engine.play();
      let out = renderBlock(engine);
      for (let i = 0; i < 3; i += 1) {
        out = renderBlock(engine);
      }
      const ls = energy(out[PLANE_LS]);
      expect(ls).toBeGreaterThan(1);
      expect(energy(out[PLANE_RS])).toBeLessThan(ls * 1e-6);
      expect(energy(out[PLANE_C])).toBeLessThan(ls * 1e-6);
      expect(energy(out[PLANE_LFE])).toBeLessThan(ls * 1e-6);
      const steadyPower = framePower(out, BLOCK - 1);

      // Move live. The placement glides with a ~5 ms time constant, so the
      // move spans a few blocks; check every frame's total power.
      engine.setTrackStripSurroundPan(TRACK, { azimuth: 110 });
      let minRatio = Number.POSITIVE_INFINITY;
      let maxRatio = 0;
      let crossfadeFrames = 0;
      for (let block = 0; block < 16; block += 1) {
        out = renderBlock(engine);
        for (let frame = 0; frame < BLOCK; frame += 1) {
          const ratio = framePower(out, frame) / steadyPower;
          minRatio = Math.min(minRatio, ratio);
          maxRatio = Math.max(maxRatio, ratio);
          if (
            Math.abs(out[PLANE_LS][frame]) > SOURCE_AMPLITUDE * 0.5 &&
            Math.abs(out[PLANE_RS][frame]) > SOURCE_AMPLITUDE * 0.5
          ) {
            crossfadeFrames += 1;
          }
        }
      }
      // Non-vacuity: the window contains frames where both planes carry signal.
      expect(crossfadeFrames).toBeGreaterThan(0);
      // The gain vector is renormalized every sample, so the total power stays
      // at the steady value through the move (float32 accumulation scale).
      expect(minRatio).toBeGreaterThanOrEqual(1 - 1e-5);
      expect(maxRatio).toBeLessThanOrEqual(1 + 1e-5);

      const rs = energy(out[PLANE_RS]);
      expect(rs).toBeGreaterThan(1);
      expect(energy(out[PLANE_LS])).toBeLessThan(rs * 1e-6);
      expect(energy(out[PLANE_L])).toBeLessThan(rs * 1e-6);
      // The exponential glide leaves a sub-percent tail after 16 blocks.
      expect(Math.abs(rs - ls)).toBeLessThan(ls * 1e-4);
      expect(Math.abs(framePower(out, BLOCK - 1) - steadyPower)).toBeLessThan(steadyPower * 1e-4);
    } finally {
      engine.destroy();
    }
  });

  it('defaults omitted fields and refuses bad input by name', () => {
    const engine = makeEngine();
    try {
      expect(() => engine.setTrackStripSurroundPan(TRACK, {})).not.toThrow();
      expect(() => engine.setTrackStripSurroundPan(TRACK, { distance: 0 })).not.toThrow();

      const refused = (fn: () => void): unknown => {
        try {
          fn();
        } catch (error) {
          return error;
        }
        return undefined;
      };
      const nan = refused(() => engine.setTrackStripSurroundPan(TRACK, { azimuth: Number.NaN }));
      expect(isSonareError(nan)).toBe(true);
      expect((nan as { code: unknown }).code).toBe(ErrorCode.InvalidParameter);
      expect(String((nan as Error).message)).toMatch(/azimuth/);
      expect(() =>
        engine.setTrackStripSurroundPan(TRACK, { lfe: Number.POSITIVE_INFINITY }),
      ).toThrow();
      expect(() =>
        engine.setTrackStripSurroundPan(TRACK, { lfe: '1' as unknown as number }),
      ).toThrow(/lfe/);
      expect(() => engine.setTrackStripSurroundPan(TRACK, null as never)).toThrow();
      expect(() => engine.setTrackStripSurroundPan(999, { azimuth: 0 })).toThrow();
      expect(() => engine.setTrackStripSurroundPan(Number.NaN, { azimuth: 0 })).toThrow();
    } finally {
      engine.destroy();
    }
  });
});
