import { describe, expect, expectTypeOf, it } from 'vitest';
import * as root from '../../dist/index.js';
import * as worklet from '../../dist/worklet.js';

type Constructor = { prototype: object };

const SYNC: ReadonlyArray<readonly [string, Record<string, unknown>]> = [
  ...[
    'Audio',
    'ClipPageProvider',
    'ClipPageStreamer',
    'HrtfSet',
    'Mixer',
    'PlaybackLoudnessMeter',
    'PlaybackRenderer',
    'PolyphonicAnalysis',
    'Project',
    'ProjectTimeline',
    'RealtimeEngine',
    'RealtimeVoiceChanger',
    'SampleBank',
    'StreamAnalyzer',
    'StreamingEqualizer',
    'StreamingMasteringChain',
    'StreamingRetune',
    'VocalEditDraft',
    'VocalEditSession',
    'VocalRenderJob',
    'VocalRenderSnapshot',
  ].map((name) => [name, root as Record<string, unknown>] as const),
  ...[
    'SonareEngine',
    'SonarePlaybackWorkletProcessor',
    'SonareRealtimeEngineWorkletProcessor',
    'SonareRealtimeVoiceChangerWorkletProcessor',
    'SonareWorkletProcessor',
    'StreamingMasteringChain',
  ].map((name) => [name, worklet as Record<string, unknown>] as const),
];

const ASYNC: ReadonlyArray<readonly [string, Record<string, unknown>]> = [
  ...['OfflineWorkerClient', 'VocalEditWorkerClient', 'VocalEditWorkerSession'].map(
    (name) => [name, root as Record<string, unknown>] as const,
  ),
  ...['SonareRealtimeEngineNode', 'SonareStreamAnalyzerNode'].map(
    (name) => [name, worklet as Record<string, unknown>] as const,
  ),
];

const RETIRED = ['delete', 'dispose', 'close', 'terminate'] as const;

function prototypeOf(entry: Record<string, unknown>, name: string): Record<PropertyKey, unknown> {
  const ctor = entry[name] as Constructor | undefined;
  expect(typeof ctor, name).toBe('function');
  return (ctor as Constructor).prototype as Record<PropertyKey, unknown>;
}

describe('teardown is destroy() plus Symbol.dispose on every synchronous class', () => {
  it.each(SYNC)('%s', (name, entry) => {
    const proto = prototypeOf(entry, name);
    expect(typeof proto.destroy).toBe('function');
    expect(typeof proto[Symbol.dispose]).toBe('function');
    expect(Symbol.asyncDispose in proto).toBe(false);
    expect(RETIRED.filter((method) => method in proto)).toEqual([]);
  });
});

describe('teardown is destroy(): Promise plus Symbol.asyncDispose on every worker-backed class', () => {
  it.each(ASYNC)('%s', (name, entry) => {
    const proto = prototypeOf(entry, name);
    expect(typeof proto.destroy).toBe('function');
    expect(typeof proto[Symbol.asyncDispose]).toBe('function');
    expect(Symbol.dispose in proto).toBe(false);
    expect(RETIRED.filter((method) => method in proto)).toEqual([]);
  });

  it('declares destroy() as returning Promise<void> on every worker-backed class', () => {
    expectTypeOf<ReturnType<root.OfflineWorkerClient['destroy']>>().toEqualTypeOf<Promise<void>>();
    expectTypeOf<ReturnType<root.VocalEditWorkerClient['destroy']>>().toEqualTypeOf<
      Promise<void>
    >();
    expectTypeOf<ReturnType<root.VocalEditWorkerSession['destroy']>>().toEqualTypeOf<
      Promise<void>
    >();
    expectTypeOf<ReturnType<worklet.SonareRealtimeEngineNode['destroy']>>().toEqualTypeOf<
      Promise<void>
    >();
    expectTypeOf<ReturnType<worklet.SonareStreamAnalyzerNode['destroy']>>().toEqualTypeOf<
      Promise<void>
    >();
  });

  it('no longer declares the retired teardown names', () => {
    const typeOnly = (
      mixer: root.Mixer,
      timeline: root.ProjectTimeline,
      streamer: root.ClipPageStreamer,
      client: root.VocalEditWorkerClient,
    ) => {
      // @ts-expect-error delete() is gone; destroy() releases the mixer
      mixer.delete();
      // @ts-expect-error dispose() is gone; destroy() releases the timeline
      timeline.dispose();
      // @ts-expect-error close() is gone; destroy() releases the streamer
      streamer.close();
      // @ts-expect-error dispose() is gone; destroy() stops the client
      client.dispose();
    };
    expect(typeof typeOnly).toBe('function');
  });
});

/** Records posted messages and never answers. */
class SilentWorker {
  readonly posted: Array<{ type: string }> = [];
  terminated = 0;
  addEventListener(): void {}
  removeEventListener(): void {}
  postMessage(message: { type: string }): void {
    this.posted.push(message);
  }
  terminate(): void {
    this.terminated += 1;
  }
}

describe('worker client destroy()', () => {
  it('OfflineWorkerClient returns the same promise from a second destroy()', async () => {
    const client = new root.OfflineWorkerClient({ worker: new SilentWorker() as never });
    const first = client.destroy();
    expect(client.destroy()).toBe(first);
    await expect(first).resolves.toBeUndefined();
  });

  it('VocalEditWorkerClient rejects a pending call with InvalidState, terminates its worker and resolves', async () => {
    const worker = new SilentWorker();
    const client = new root.VocalEditWorkerClient({
      worker: worker as never,
      terminateWorkerOnDestroy: true,
    });
    const pending = client.create({ samples: new Float32Array(16), sampleRate: 16000 }).then(
      () => undefined,
      (error: unknown) => error,
    );
    const done = client.destroy();
    expect(done).toBeInstanceOf(Promise);
    expect(client.destroy()).toBe(done);
    await expect(done).resolves.toBeUndefined();
    const error = await pending;
    expect(root.isSonareError(error)).toBe(true);
    expect((error as root.SonareError).code).toBe(root.ErrorCode.InvalidState);
    expect(worker.terminated).toBe(1);
  });

  it('VocalEditWorkerClient leaves a caller-owned worker running by default', async () => {
    const worker = new SilentWorker();
    const client = new root.VocalEditWorkerClient({ worker: worker as never });
    await client[Symbol.asyncDispose]();
    expect(worker.terminated).toBe(0);
  });
});

describe('ABI probes for optional features', () => {
  it('declare number | undefined, undefined when the feature is compiled out', () => {
    expectTypeOf(root.vocalEditAbiVersion).returns.toEqualTypeOf<number | undefined>();
    expectTypeOf(root.projectAbiVersion).returns.toEqualTypeOf<number | undefined>();
  });

  it('answer a positive integer in the full build', async () => {
    await root.init();
    for (const value of [root.vocalEditAbiVersion(), root.projectAbiVersion()]) {
      expect(Number.isInteger(value)).toBe(true);
      expect(value).toBeGreaterThan(0);
    }
  });
});
