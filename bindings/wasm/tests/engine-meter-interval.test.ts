import { describe, expect, it } from 'vitest';
import type { RealtimeEngine } from '../src/index';
import { SonareEngine } from '../src/worklet/engine';
import { SonareRealtimeEngineNode } from '../src/worklet/engine-node';
import type { SonareRealtimeEngineNodeOptions } from '../src/worklet/messages';

type CapturedNodeOptions = AudioWorkletNodeOptions & {
  processorOptions?: Record<string, unknown>;
};

function fakeContext(): BaseAudioContext {
  return { sampleRate: 48000 } as unknown as BaseAudioContext;
}

function readyNode(): AudioWorkletNode {
  const port = {
    onmessage: undefined as ((event: MessageEvent<unknown>) => void) | undefined,
    postMessage: () => undefined,
  };
  queueMicrotask(() => {
    port.onmessage?.({ data: { type: 'ready', runtimeTarget: 'embind' } } as MessageEvent<unknown>);
  });
  return { port, disconnect: () => undefined } as unknown as AudioWorkletNode;
}

function emptyOfflineEngine(): RealtimeEngine {
  return {
    parameterCount: () => 0,
    destroy: () => undefined,
  } as unknown as RealtimeEngine;
}

async function captureNodeOptions(
  options: Pick<SonareRealtimeEngineNodeOptions, 'meterIntervalFrames'>,
): Promise<CapturedNodeOptions> {
  let captured: CapturedNodeOptions | undefined;
  const node = await SonareRealtimeEngineNode.create(fakeContext(), {
    mode: 'postMessage',
    engineAbiVersion: 1,
    ...options,
    nodeFactory: (_context, _processorName, nodeOptions) => {
      captured = nodeOptions as CapturedNodeOptions;
      return readyNode();
    },
  });
  node.destroy();
  if (!captured) {
    throw new Error('nodeFactory did not receive AudioWorkletNodeOptions');
  }
  return captured;
}

const invalidMeterIntervals = [
  -1,
  2.5,
  Number.NaN,
  Number.POSITIVE_INFINITY,
  Number.NEGATIVE_INFINITY,
];

describe('meterIntervalFrames forwarding', () => {
  it.each([0, 128, 2048])('forwards %p to the source node factory', async (meterIntervalFrames) => {
    const options = await captureNodeOptions({ meterIntervalFrames });
    expect(options.processorOptions?.meterIntervalFrames).toBe(meterIntervalFrames);
  });

  it.each(invalidMeterIntervals)('rejects invalid node option %p', async (meterIntervalFrames) => {
    await expect(captureNodeOptions({ meterIntervalFrames })).rejects.toThrow(
      'meterIntervalFrames must be an integer of at least 0',
    );
  });

  it('forwards meterIntervalFrames through the high-level SonareEngine facade', async () => {
    let captured: CapturedNodeOptions | undefined;
    const engine = await SonareEngine.create(fakeContext(), {
      mode: 'postMessage',
      engineAbiVersion: 1,
      meterIntervalFrames: 321,
      offlineEngine: emptyOfflineEngine(),
      nodeFactory: (_context, _processorName, nodeOptions) => {
        captured = nodeOptions as CapturedNodeOptions;
        return readyNode();
      },
    });
    try {
      expect(captured?.processorOptions?.meterIntervalFrames).toBe(321);
    } finally {
      engine.destroy();
    }
  });

  it.each(invalidMeterIntervals)(
    'rejects invalid facade option %p',
    async (meterIntervalFrames) => {
      await expect(
        SonareEngine.create(fakeContext(), {
          mode: 'postMessage',
          engineAbiVersion: 1,
          meterIntervalFrames,
          offlineEngine: emptyOfflineEngine(),
          nodeFactory: () => {
            throw new Error('nodeFactory should not be reached');
          },
        }),
      ).rejects.toThrow('meterIntervalFrames must be an integer of at least 0');
    },
  );
});
