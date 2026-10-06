/**
 * The AudioWorklet engine at channelCount 6: a lane placed with
 * `SonareEngine.setTrackStripSurroundPan` lands in the matching output plane of
 * the processor's render, after the sync message crosses the guarded handler.
 */

import { describe, expect, it } from 'vitest';
import type { RealtimeEngine } from '../dist/index.js';
import { isEngineSyncMessage } from '../src/worklet/guards';
import {
  SonareEngine,
  SonareRealtimeEngineWorkletProcessor,
  setupWorklet,
} from './_worklet_helpers';

type OfflineEngineOption = NonNullable<
  NonNullable<Parameters<typeof SonareEngine.create>[1]>['offlineEngine']
>;

const SR = 48000;
const BLOCK = 128;
const PLANES = 6;
const TRACK = 10;
const [PLANE_L, PLANE_R, PLANE_C, PLANE_LFE, PLANE_LS, PLANE_RS] = [0, 1, 2, 3, 4, 5];

function fakeContext(): BaseAudioContext {
  return {
    sampleRate: SR,
    audioWorklet: { addModule: () => Promise.resolve() },
  } as unknown as BaseAudioContext;
}

function readyWorkletNode(port: {
  onmessage?: ((event: MessageEvent<unknown>) => void) | null;
  [member: string]: unknown;
}): AudioWorkletNode {
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

const energy = (plane: Float32Array): number => plane.reduce((sum, v) => sum + v * v, 0);

describe('SonareEngine surround pan at channelCount 6', () => {
  setupWorklet();

  it('routes a surround-panned lane to the matching output plane', async () => {
    const posted: unknown[] = [];
    const offline = new (await import('../dist/index.js')).RealtimeEngine(
      SR,
      BLOCK,
    ) as unknown as OfflineEngineOption;
    const engine = await SonareEngine.create(fakeContext(), {
      mode: 'postMessage',
      offlineEngine: offline,
      channelCount: PLANES,
      nodeFactory: () =>
        readyWorkletNode({
          postMessage: (message: unknown) => posted.push(message),
          onmessage: undefined,
        }),
    });
    const frames = BLOCK * 64;
    engine.addClip(TRACK, [new Float32Array(frames).fill(0.5)], 0, { lengthSamples: frames });
    engine.setTrackLanes([TRACK]);
    engine.setTrackStripJson(
      TRACK,
      '{"version":1,"strips":[{"id":"track-10"}],"buses":[],"connections":[]}',
    );
    engine.setTrackStripSurroundPan(TRACK, { azimuth: -110 });
    const surroundMessage = posted.find(
      (m) => (m as { type?: unknown }).type === 'syncTrackStripSurroundPan',
    );
    expect(surroundMessage).toEqual({
      type: 'syncTrackStripSurroundPan',
      trackId: TRACK,
      pan: { azimuth: -110, elevation: 0, divergence: 0, lfe: 0, distance: 1 },
    });

    const processor = new SonareRealtimeEngineWorkletProcessor(
      { sampleRate: SR, blockSize: BLOCK, channelCount: PLANES },
      { postMessage: () => undefined },
    );
    try {
      const replay = (messages: unknown[]): void => {
        for (const message of messages) {
          if (isEngineSyncMessage(message)) {
            processor.receiveSync(message);
          }
        }
      };
      const render = (): Float32Array[][] => {
        const outputs = [Array.from({ length: PLANES }, () => new Float32Array(BLOCK))];
        expect(processor.process([[]], outputs)).toBe(true);
        return outputs;
      };
      replay(posted);
      const rig = (processor as unknown as { engine: RealtimeEngine }).engine;
      rig.play();
      let planes = render()[0];
      for (let i = 0; i < 24; i += 1) {
        planes = render()[0];
      }
      const ls = energy(planes[PLANE_LS]);
      expect(ls).toBeGreaterThan(0.1);
      for (const quiet of [PLANE_L, PLANE_R, PLANE_C, PLANE_LFE, PLANE_RS]) {
        expect(energy(planes[quiet])).toBeLessThan(ls * 1e-6);
      }

      posted.length = 0;
      engine.setTrackStripSurroundPan(TRACK, { azimuth: 110 });
      replay(posted);
      for (let i = 0; i < 24; i += 1) {
        planes = render()[0];
      }
      const rs = energy(planes[PLANE_RS]);
      expect(rs).toBeGreaterThan(0.1);
      expect(energy(planes[PLANE_LS])).toBeLessThan(rs * 1e-6);
    } finally {
      processor.destroy();
      engine.destroy();
    }
  });
});
