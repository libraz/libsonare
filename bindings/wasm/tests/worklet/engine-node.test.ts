import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  SonareRealtimeEngineNode,
  type SonareRealtimeEngineNodeOptions,
} from '../../dist/worklet.js';

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

  it.each([2.7, 0, -1, Number.NaN, Number.POSITIVE_INFINITY])(
    'refuses %p rather than resolving it to a count the caller did not ask for',
    async (channelCount) => {
      await expect(capture(channelCount)).rejects.toThrow(RangeError);
      await expect(capture(channelCount)).rejects.toThrow(
        /channelCount must be an integer of at least 1/,
      );
    },
  );

  it('leaves the ceiling to the engine', async () => {
    expect((await capture(1e9)).outputChannelCount).toEqual([1e9]);
  });
});

describe('SonareRealtimeEngineNode integer ring options', () => {
  async function capture(
    options: SonareRealtimeEngineNodeOptions,
  ): Promise<AudioWorkletNodeOptions> {
    let captured: AudioWorkletNodeOptions | undefined;
    const node = await SonareRealtimeEngineNode.create(fakeContext(), {
      mode: 'sab',
      engineAbiVersion: 1,
      ...options,
      nodeFactory: (_ctx, _name, nodeOptions) => {
        captured = nodeOptions;
        return fakeNode();
      },
    });
    node.destroy();
    return captured as AudioWorkletNodeOptions;
  }

  it('carries scopeIntervalFrames through, and 0 leaves the scope ring uncreated', async () => {
    const off = await capture({ scopeIntervalFrames: 0 });
    expect(off.processorOptions?.scopeIntervalFrames).toBeUndefined();
    expect(off.processorOptions?.scopeSharedBuffer).toBeUndefined();
    const on = await capture({ scopeIntervalFrames: 256 });
    expect(on.processorOptions?.scopeIntervalFrames).toBe(256);
    expect(on.processorOptions?.scopeSharedBuffer).toBeInstanceOf(SharedArrayBuffer);
    expect(
      (await capture({ scopeIntervalFrames: 512 })).processorOptions?.scopeIntervalFrames,
    ).toBe(512);
  });

  it.each([2048.5, -1, Number.NaN, Number.POSITIVE_INFINITY])(
    'refuses a scopeIntervalFrames of %p',
    async (scopeIntervalFrames) => {
      await expect(capture({ scopeIntervalFrames })).rejects.toThrow(
        /scopeIntervalFrames must be an integer of at least 0/,
      );
    },
  );

  it('sizes the meter ring by the requested capacity', async () => {
    const meterRing = async (meterRingCapacity: number) => {
      const options = (await capture({ meterRingCapacity })).processorOptions as {
        meterRingCapacity: number;
        meterSharedBuffer: SharedArrayBuffer;
      };
      return { capacity: options.meterRingCapacity, bytes: options.meterSharedBuffer.byteLength };
    };
    const small = await meterRing(16);
    const large = await meterRing(256);
    expect(small.capacity).toBe(16);
    expect(large.capacity).toBe(256);
    expect(large.bytes).toBeGreaterThan(small.bytes);
  });

  it.each([16.5, 0, -8, Number.NaN])(
    'refuses a meterRingCapacity of %p rather than rounding it into range',
    async (meterRingCapacity) => {
      await expect(capture({ meterRingCapacity })).rejects.toThrow(RangeError);
      await expect(capture({ meterRingCapacity })).rejects.toThrow(
        /meterRingCapacity must be an integer of at least 1/,
      );
    },
  );

  it('refuses a scope capacity even when the scope ring is never created', async () => {
    await expect(capture({ scopeRingCapacity: 8.5 })).rejects.toThrow(
      /scopeRingCapacity must be an integer of at least 1/,
    );
    await expect(capture({ mode: 'postMessage', scopeBands: 0 })).rejects.toThrow(
      /scopeBands must be an integer of at least 1/,
    );
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
