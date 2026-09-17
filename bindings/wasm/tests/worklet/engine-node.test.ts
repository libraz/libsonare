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

describe('SonareRealtimeEngineNode channelCount', () => {
  async function capture(channelCount: number | undefined): Promise<AudioWorkletNodeOptions> {
    let captured: AudioWorkletNodeOptions | undefined;
    const node = await SonareRealtimeEngineNode.create(fakeContext(), {
      mode: 'sab',
      engineAbiVersion: 1,
      channelCount,
      nodeFactory: (_ctx, _name, nodeOptions) => {
        captured = nodeOptions;
        return fakeNode();
      },
    });
    node.destroy();
    return captured as AudioWorkletNodeOptions;
  }

  it('carries the requested count through to the node and the processor', async () => {
    expect((await capture(2)).outputChannelCount).toEqual([2]);
    expect((await capture(3)).outputChannelCount).toEqual([3]);
    expect((await capture(3)).processorOptions?.channelCount).toBe(3);
  });

  it.each([
    2.7,
    0,
    -1,
    Number.NaN,
    Number.POSITIVE_INFINITY,
  ])('refuses %p rather than resolving it to a count the caller did not ask for', async (channelCount) => {
    await expect(capture(channelCount)).rejects.toThrow(RangeError);
    await expect(capture(channelCount)).rejects.toThrow(
      /channelCount must be an integer of at least 1/,
    );
  });

  it('leaves the ceiling to the engine', async () => {
    expect((await capture(1e9)).outputChannelCount).toEqual([1e9]);
  });
});

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
