/**
 * A chunked offline bounce has to concatenate to the same audio one continuous
 * render produces.
 *
 * `renderOffline` used to end the timeline on every call: it released every
 * sounding note and flushed the PDC / alignment delay lines whether or not more
 * chunks were coming. Bouncing a long project in chunks therefore lost any pad
 * that spanned a chunk boundary — its note-off fired at the end of chunk N and
 * no note-on was ever re-sent in chunk N+1 — and dropped out at every boundary
 * from the cleared delay lines. `finalize: false` is what a chunk passes;
 * `finishOfflineRender()` releases what is left after the last one.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, RealtimeEngine } from '../dist/index.js';

const SR = 48000;
const BLOCK = 128;

beforeAll(async () => {
  await init();
});

function midi1Word(status: number, channel: number, data1: number, data2: number): number {
  return (
    (0x2 << 28) | ((status & 0xf) << 20) | ((channel & 0xf) << 16) | ((data1 & 0x7f) << 8) | data2
  );
}

/**
 * An engine holding a pad that outlives any render span: note-on at frame 0 and
 * no note-off, so whether it is still sounding at the end of a chunk is exactly
 * the state a non-finalizing render has to preserve.
 */
function padEngine(): RealtimeEngine {
  const engine = new RealtimeEngine(SR, BLOCK);
  engine.setBuiltinInstrument({ gain: 0.5 }, 5);
  engine.setMidiClips([
    {
      id: 1,
      trackId: 5,
      destinationId: 5,
      lengthSamples: 1 << 20,
      events: [{ renderFrame: 0, word0: midi1Word(0x9, 0, 60, 100), wordCount: 1 }],
    },
  ]);
  engine.play();
  return engine;
}

const rms = (data: ArrayLike<number>): number => {
  let sum = 0;
  for (let i = 0; i < data.length; i++) {
    sum += data[i] * data[i];
  }
  return Math.sqrt(sum / data.length);
};

/** Largest per-sample deviation between two equal-length renders. */
function maxAbsDiff(a: ArrayLike<number>, b: ArrayLike<number>): number {
  expect(a.length).toBe(b.length);
  let worst = 0;
  for (let i = 0; i < a.length; i++) {
    worst = Math.max(worst, Math.abs(a[i] - b[i]));
  }
  return worst;
}

describe('chunked renderOffline', () => {
  it('accepts a request object identically to the positional form', () => {
    const frames = 2048;
    const positionalEngine = padEngine();
    const positional = positionalEngine.renderOffline(
      [new Float32Array(frames), new Float32Array(frames)],
      BLOCK,
    );
    positionalEngine.destroy();

    const requestEngine = padEngine();
    const request = requestEngine.renderOffline({
      channels: [new Float32Array(frames), new Float32Array(frames)],
      blockSize: BLOCK,
    });
    requestEngine.destroy();

    // Non-vacuity: comparing two silent buffers would prove nothing.
    expect(rms(positional[0])).toBeGreaterThan(0);
    expect(maxAbsDiff(request[0], positional[0])).toBeLessThan(1e-6);
    expect(maxAbsDiff(request[1], positional[1])).toBeLessThan(1e-6);
  });

  it('concatenates to one continuous render', () => {
    const chunk = 4096;
    const chunks = 3;
    const total = chunk * chunks;

    const continuousEngine = padEngine();
    const [continuous] = continuousEngine.renderOffline([
      new Float32Array(total),
      new Float32Array(total),
    ]);
    continuousEngine.destroy();
    expect(rms(continuous)).toBeGreaterThan(0);

    const renderChunks = (finalize: boolean): number[] => {
      const engine = padEngine();
      const joined: number[] = [];
      for (let i = 0; i < chunks; i++) {
        const [left] = engine.renderOffline({
          channels: [new Float32Array(chunk), new Float32Array(chunk)],
          blockSize: BLOCK,
          finalize,
        });
        joined.push(...left);
      }
      engine.finishOfflineRender();
      engine.destroy();
      return joined;
    };

    expect(maxAbsDiff(renderChunks(false), continuous)).toBeLessThan(1e-6);

    // Non-vacuity: finalizing every chunk is the defect the flag exists to fix.
    // The pad's note-off fires at the end of chunk 1 and no note-on is re-sent,
    // so the tail decays away instead of holding and the join diverges.
    const finalizedPerChunk = renderChunks(true);
    const lastChunkRms = (samples: ArrayLike<number>): number =>
      rms(Array.prototype.slice.call(samples, total - chunk));
    expect(maxAbsDiff(finalizedPerChunk, continuous)).toBeGreaterThan(1e-3);
    expect(lastChunkRms(finalizedPerChunk)).toBeLessThan(0.5 * lastChunkRms(continuous));
  });
});
