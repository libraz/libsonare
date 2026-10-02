/**
 * Regressions for the allocation-free Mixer realtime bridge.
 *
 * The bridge owns one WASM heap view per strip. Topology changes may grow the
 * native scratch arrays and WebAssembly memory growth detaches every previous
 * view, so both cases must refresh before the next prepared block.
 */

import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';

const SR = 48000;
const BLOCK = 128;

function energy(channel: Float32Array): number {
  let sum = 0;
  for (const sample of channel) {
    sum += sample * sample;
  }
  return sum;
}

describe('Mixer realtime topology and heap views (WASM)', () => {
  beforeAll(async () => {
    await init();
  });

  it('refreshes an existing realtime buffer after adding a strip and processes its audio', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
    const realtime = mixer.createRealtimeBuffer();
    try {
      const existingLeft = realtime.leftInputs[0];
      const existingRight = realtime.rightInputs[0];
      expect(realtime.leftInputs).toHaveLength(3);

      mixer.addStrip('late-arrival', { enabled: false });
      mixer.compile();

      const leftInputs = realtime.leftInputs;
      const rightInputs = realtime.rightInputs;
      expect(leftInputs).toHaveLength(4);
      expect(rightInputs).toHaveLength(4);
      expect(leftInputs[0]).toBe(existingLeft);
      expect(rightInputs[0]).toBe(existingRight);
      leftInputs[3].fill(0.5);
      rightInputs[3].fill(0.5);
      realtime.process();

      expect(energy(realtime.outLeft)).toBeGreaterThan(1e-3);
      expect(energy(realtime.outRight)).toBeGreaterThan(1e-3);

      // A buffer acquired after the topology change must see the same newly
      // allocated native scratch plane as an older buffer.
      const fresh = mixer.createRealtimeBuffer();
      fresh.leftInputs[3].fill(0.25);
      fresh.rightInputs[3].fill(0.25);
      fresh.process();
      expect(energy(fresh.outLeft)).toBeGreaterThan(1e-3);
      expect(energy(fresh.outRight)).toBeGreaterThan(1e-3);
    } finally {
      mixer.delete();
    }
  });

  it('grows native scratch pointer arrays before processStereoInto uses the new strip count', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
    try {
      mixer.addStrip('direct-late-arrival', { enabled: false });
      mixer.compile();

      const leftChannels = Array.from({ length: 4 }, () => new Float32Array(BLOCK));
      const rightChannels = Array.from({ length: 4 }, () => new Float32Array(BLOCK));
      leftChannels[3].fill(0.5);
      rightChannels[3].fill(0.5);
      const outLeft = new Float32Array(BLOCK);
      const outRight = new Float32Array(BLOCK);
      expect(() =>
        mixer.processStereoInto(leftChannels, rightChannels, outLeft, outRight),
      ).not.toThrow();
      expect(energy(outLeft)).toBeGreaterThan(1e-3);
      expect(energy(outRight)).toBeGreaterThan(1e-3);
    } finally {
      mixer.delete();
    }
  });

  it('reacquires detached views before resolving the implicit prepared block length', () => {
    const mixer = Mixer.fromSceneJson(mixingScenePresetJson('commentaryDucking'), SR, BLOCK);
    const realtime = mixer.createRealtimeBuffer();
    try {
      const oldOutput = realtime.outLeft;
      let added = 0;
      while (oldOutput.byteLength !== 0 && added < 32) {
        mixer.addStrip(`heap-growth-${added}`);
        added += 1;
      }
      expect(oldOutput.byteLength).toBe(0);

      mixer.compile();
      // Calling process directly is intentional: the default argument must be
      // resolved after the detached view has been refreshed.
      expect(() => realtime.process()).not.toThrow();
      const leftInputs = realtime.leftInputs;
      const rightInputs = realtime.rightInputs;
      expect(leftInputs.length).toBe(mixer.stripCount());
      leftInputs[leftInputs.length - 1].fill(0.25);
      rightInputs[rightInputs.length - 1].fill(0.25);
      realtime.process();
      expect(energy(realtime.outLeft)).toBeGreaterThan(1e-3);
      expect(energy(realtime.outRight)).toBeGreaterThan(1e-3);

      const fresh = mixer.createRealtimeBuffer();
      fresh.leftInputs[fresh.leftInputs.length - 1].fill(0.125);
      fresh.rightInputs[fresh.rightInputs.length - 1].fill(0.125);
      fresh.process();
      expect(energy(fresh.outLeft)).toBeGreaterThan(1e-3);
      expect(energy(fresh.outRight)).toBeGreaterThan(1e-3);
    } finally {
      mixer.delete();
    }
  });
});
