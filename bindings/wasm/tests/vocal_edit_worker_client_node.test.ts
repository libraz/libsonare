/**
 * `VocalEditWorkerClient` runs headlessly under Node over a `worker_threads`
 * Worker, passed either directly or through a factory, with no global `Worker`.
 */

import { Worker as NodeWorker } from 'node:worker_threads';
import { beforeAll, describe, expect, it } from 'vitest';
import { capabilities, ErrorCode, init, VocalEditWorkerClient } from '../dist/index.js';

const SR = 22050;
const entry = new URL('./fixtures/vocal-edit-worker-node-entry.mjs', import.meta.url);

function voice(seconds = 1): Float32Array {
  const samples = new Float32Array(Math.floor(seconds * SR));
  for (let i = 0; i < samples.length; i++) {
    samples[i] = 0.3 * Math.sin((2 * Math.PI * 220 * i) / SR);
  }
  return samples;
}

describe('VocalEditWorkerClient under Node', () => {
  beforeAll(async () => {
    await init();
  });

  it('has no global Worker, so the options are the only way in', () => {
    expect(typeof (globalThis as { Worker?: unknown }).Worker).toBe('undefined');
    expect(() => new VocalEditWorkerClient()).toThrow(
      expect.objectContaining({
        code: ErrorCode.NotSupported,
        message: expect.stringMatching(/worker/i),
      }),
    );
  });

  const spellings: ReadonlyArray<[string, () => VocalEditWorkerClient]> = [
    [
      'a worker_threads Worker',
      () =>
        new VocalEditWorkerClient({
          worker: new NodeWorker(entry),
          terminateWorkerOnDestroy: true,
        }),
    ],
    ['a factory', () => new VocalEditWorkerClient({ workerFactory: () => new NodeWorker(entry) })],
  ];

  it.each(spellings)('completes one round trip through %s', async (_label, build) => {
    const client = build();
    try {
      if (!capabilities().features.vocalEdit) {
        // The message still crosses the thread and comes back as the module's refusal.
        await expect(client.create({ samples: voice(), sampleRate: SR })).rejects.toThrow();
        return;
      }
      const session = await client.create({ samples: voice(), sampleRate: SR });
      expect(session.sessionId).toMatch(/\S/);
      expect(session.created.capabilities.abiVersion).toBeGreaterThan(0);
      await session.destroy();
    } finally {
      await client.destroy();
    }
  });
});
