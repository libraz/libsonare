import { afterEach, describe, expect, it, vi } from 'vitest';
import { SonareRealtimeEngineNode } from '../../dist/worklet.js';

function fakeContext(): BaseAudioContext {
  return { sampleRate: 48000 } as unknown as BaseAudioContext;
}

function fakeNode(): AudioWorkletNode {
  return {
    port: { postMessage: () => undefined, onmessage: undefined },
    disconnect: () => undefined,
  } as unknown as AudioWorkletNode;
}

async function createNode(): Promise<SonareRealtimeEngineNode> {
  return SonareRealtimeEngineNode.create(fakeContext(), {
    mode: 'sab',
    engineAbiVersion: 1,
    nodeFactory: () => fakeNode(),
  });
}

describe('SonareRealtimeEngineNode MIDI ring polling', () => {
  afterEach(() => {
    vi.useRealTimers();
  });

  it('stops the ring polling timer after the last MIDI listener unsubscribes', async () => {
    vi.useFakeTimers();
    const node = await createNode();
    try {
      const unsubscribe = node.onMidiOut(() => undefined);
      expect(vi.getTimerCount()).toBe(1);

      unsubscribe();

      expect(vi.getTimerCount()).toBe(0);
    } finally {
      node.destroy();
    }
  });

  it('keeps polling while another ring listener remains subscribed', async () => {
    vi.useFakeTimers();
    const node = await createNode();
    try {
      const unsubscribeMidi = node.onMidiOut(() => undefined);
      const unsubscribeTelemetry = node.onTelemetry(() => undefined);
      expect(vi.getTimerCount()).toBe(1);

      unsubscribeMidi();

      expect(vi.getTimerCount()).toBe(1);
      unsubscribeTelemetry();
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      node.destroy();
    }
  });
});
