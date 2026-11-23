/**
 * A count the entry derives from an array's length was never written by the caller, so a refusal
 * of it names that array's length, under the predicate the C ABI states.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Project, RealtimeEngine } from '../dist/index.js';

const REFUSAL = Symbol.for('libsonare.refusal');

const caught = (run: () => unknown): Error => {
  try {
    run();
  } catch (error) {
    return error as Error;
  }
  throw new Error('expected the call to throw');
};

describe('derived counts are named by the array they come from', () => {
  beforeAll(async () => {
    await init();
  });

  it.each([
    [
      'anchorCount',
      () => {
        using project = new Project();
        project.setWarpMap({ id: 1, anchors: [{ sourceSample: 0, warpSample: 0 }] });
      },
      'Project.setWarpMap: anchors.length must be an integer in [2, 500000000]',
      ['anchors.length'],
    ],
    [
      'laneCount',
      () => {
        using engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        engine.setTrackLanes(Array.from({ length: 33 }, (_, i) => ({ trackId: i + 1 })));
      },
      'RealtimeEngine.setTrackLanes: lanes.length must be an integer in [0, 32]',
      ['lanes.length'],
    ],
    [
      'numChannels',
      () => {
        using engine = new RealtimeEngine({ sampleRate: 48000, maxBlockSize: 128 });
        engine.renderOffline({ channels: Array.from({ length: 65 }, () => new Float32Array(16)) });
      },
      'RealtimeEngine.renderOffline: channels.length must be an integer in [1, 64]',
      ['channels.length'],
    ],
  ] as const)('%s', (_count, run, text, subjects) => {
    const error = caught(run);
    expect(error).toBeInstanceOf(RangeError);
    expect(error.message).toBe(text);
    expect((error as never as Record<symbol, { subjects: string[] }>)[REFUSAL].subjects).toEqual(
      subjects,
    );
  });
});
