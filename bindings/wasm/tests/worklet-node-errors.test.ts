/**
 * Every stock worklet node reports what their processor reports: an error
 * message reaches `onError` with the class the audio thread captured, and a processor constructor
 * that throws (`processorerror`) rejects `ready`.
 */

import { describe, expect, it } from 'vitest';
import { init } from '../src/index';
import { SonareRealtimeEngineNode } from '../src/worklet/engine-node';
import { SonarePlaybackNode } from '../src/worklet/playback-node';
import { SonareStreamAnalyzerNode } from '../src/worklet/stream-analyzer-node';
import { SonareRealtimeVoiceChangerNode } from '../src/worklet/voice-changer-node';

interface FakeNode {
  port: {
    onmessage?: (event: MessageEvent<unknown>) => void;
    postMessage: () => void;
  };
  listeners: Map<string, () => void>;
  addEventListener: (type: string, listener: () => void) => void;
  disconnect: () => void;
}

function fakeNode(): FakeNode {
  const node: FakeNode = {
    port: { postMessage: () => undefined },
    listeners: new Map(),
    addEventListener: (type, listener) => {
      node.listeners.set(type, listener);
    },
    disconnect: () => undefined,
  };
  return node;
}

const send = (node: FakeNode, data: unknown) =>
  node.port.onmessage?.({ data } as MessageEvent<unknown>);

const renderFailure = {
  type: 'error',
  request: 'process',
  message: 'render failed',
  failure: { name: 'RangeError', message: 'render failed' },
};

const context = { sampleRate: 48000 } as unknown as BaseAudioContext;

async function engineNode(fake: FakeNode) {
  return SonareRealtimeEngineNode.create(context, {
    numChannels: 2,
    mode: 'postMessage',
    engineAbiVersion: 1,
    nodeFactory: () => fake as unknown as AudioWorkletNode,
  });
}

async function analyzerNode(fake: FakeNode) {
  await init();
  return SonareStreamAnalyzerNode.create(context, {
    nodeFactory: () => fake as unknown as AudioWorkletNode,
  });
}

async function playbackNode(fake: FakeNode) {
  return SonarePlaybackNode.create(context, {
    config: { input: { layout: 'auto' }, target: { kind: 'speakers', layout: '5.1' } },
    nodeFactory: () => fake as unknown as AudioWorkletNode,
  });
}

async function voiceChangerNode(fake: FakeNode) {
  return SonareRealtimeVoiceChangerNode.create(context, {
    numChannels: 1,
    nodeFactory: () => fake as unknown as AudioWorkletNode,
  });
}

describe.each([
  ['SonareRealtimeEngineNode', engineNode],
  ['SonareStreamAnalyzerNode', analyzerNode],
  ['SonarePlaybackNode', playbackNode],
  ['SonareRealtimeVoiceChangerNode', voiceChangerNode],
] as const)('%s processor errors', (_label, create) => {
  it('passes an error the processor posts after ready to onError with its class', async () => {
    const fake = fakeNode();
    const node = await create(fake);
    const errors: Error[] = [];
    const unsubscribe = node.onError((error) => errors.push(error));
    send(fake, { type: 'ready', runtimeTarget: 'embind' });
    await node.ready;
    send(fake, renderFailure);
    expect(errors).toHaveLength(1);
    expect(errors[0]).toBeInstanceOf(RangeError);
    expect(errors[0].message).toBe('render failed');
    unsubscribe();
    send(fake, renderFailure);
    expect(errors).toHaveLength(1);
    await node.destroy();
  });

  it('rejects ready when the processor constructor throws', async () => {
    const fake = fakeNode();
    const node = await create(fake);
    fake.listeners.get('processorerror')?.();
    await expect(node.ready).rejects.toThrow('the processor failed to start');
    await node.destroy();
  });
});
