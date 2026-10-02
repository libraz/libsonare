import { expect, it } from 'vitest';
import { SonareRealtimeEngineNode } from '../src/worklet/engine-node';

it('queues a master loudness reset through the postMessage node transport', async () => {
  const posted: unknown[] = [];
  const context = { sampleRate: 48000 } as unknown as BaseAudioContext;
  const node = await SonareRealtimeEngineNode.create(context, {
    mode: 'postMessage',
    engineAbiVersion: 1,
    nodeFactory: () => {
      const port = {
        onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
        postMessage: (message: unknown) => posted.push(message),
      };
      queueMicrotask(() => {
        port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent);
      });
      return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
    },
  });
  try {
    expect(node.resetMasterLoudnessMeter(512)).toBe(true);
    expect(posted).toContainEqual({
      type: 28,
      sampleTime: 512,
    });
  } finally {
    node.destroy();
  }
});
