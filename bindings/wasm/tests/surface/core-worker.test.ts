import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, OfflineWorkerClient } from '../../dist/index.js';
import { installOfflineWorkerEndpoint } from '../../dist/worker.js';

type MessageListener = (event: MessageEvent) => void;
type OfflineWorkerLike = NonNullable<
  NonNullable<ConstructorParameters<typeof OfflineWorkerClient>[0]>['worker']
>;

/** Records posted messages and answers nothing until told to. */
class SilentWorker {
  readonly posted: Array<{ type: string; id: number }> = [];
  terminated = false;
  private listener: MessageListener | undefined;
  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.listener = listener as unknown as MessageListener;
    }
  }
  removeEventListener(): void {}
  postMessage(message: { type: string; id: number }): void {
    this.posted.push(message);
  }
  terminate(): Promise<void> {
    this.terminated = true;
    return Promise.resolve();
  }
  reply(data: unknown): void {
    this.listener?.({ data } as MessageEvent);
  }
}

/** A structured-clone loopback to an in-process endpoint. */
class LoopbackWorker {
  private clientMessage: MessageListener | undefined;
  private workerMessage: MessageListener | undefined;
  constructor() {
    installOfflineWorkerEndpoint({
      postMessage: (message, transfer) => {
        const cloned = structuredClone(message, { transfer });
        queueMicrotask(() => this.clientMessage?.({ data: cloned } as MessageEvent));
      },
      addEventListener: (_type, listener) => {
        this.workerMessage = listener as unknown as MessageListener;
      },
    });
  }
  addEventListener(type: string, listener: EventListener): void {
    if (type === 'message') {
      this.clientMessage = listener as unknown as MessageListener;
    }
  }
  removeEventListener(): void {}
  postMessage(message: unknown, transfer?: Transferable[]): void {
    const cloned = structuredClone(message, { transfer });
    queueMicrotask(() => this.workerMessage?.({ data: cloned } as MessageEvent));
  }
  terminate(): void {}
}

const clientOver = (worker: object, extra = {}): OfflineWorkerClient =>
  new OfflineWorkerClient({ worker: worker as OfflineWorkerLike, ...extra });

const tiny = (): { samples: Float32Array } => ({ samples: new Float32Array([0, 0]) });
const isCode = (code: number) => (error: unknown) => isSonareError(error) && error.code === code;

function tone(seconds: number, sampleRate = 22050): Float32Array {
  const samples = new Float32Array(seconds * sampleRate);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = 0.35 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  }
  return samples;
}

describe('OfflineWorkerClient cancellation', () => {
  it('abort mid-call rejects with Cancelled carrying signal.reason and tells the worker', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const controller = new AbortController();
    const call = client.detectBpm(tiny(), { signal: controller.signal, copy: true });
    const reason = new Error('user navigated away');
    controller.abort(reason);
    const error = await call.catch((e: unknown) => e);
    expect(isSonareError(error) && error.code).toBe(ErrorCode.Cancelled);
    expect((error as { cause?: unknown }).cause).toBe(reason);
    expect(worker.posted.map((m) => m.type)).toEqual([
      'sonare:offline-run',
      'sonare:offline-cancel',
    ]);
    await client.destroy();
  });

  it('a pre-aborted signal rejects without posting work', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const reason = new Error('already aborted');
    const signal = AbortSignal.abort(reason);
    const samples = new Float32Array([0, 0]);
    const error = await client.detectBpm({ samples }, { signal }).catch((e: unknown) => e);
    expect(isSonareError(error) && error.code).toBe(ErrorCode.Cancelled);
    expect((error as { cause?: unknown }).cause).toBe(reason);
    expect(worker.posted).toHaveLength(0);
    expect(samples.byteLength).toBe(8);
    await client.destroy();
  });

  it('removes its abort listener once the call settles', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const controller = new AbortController();
    let removed = 0;
    const remove = controller.signal.removeEventListener.bind(controller.signal);
    controller.signal.removeEventListener = ((...args: Parameters<typeof remove>) => {
      removed += 1;
      return remove(...args);
    }) as typeof remove;
    const call = client.detectBpm(tiny(), { signal: controller.signal, copy: true });
    worker.reply({ type: 'sonare:offline-result', id: 1, result: 120 });
    expect(await call).toBe(120);
    expect(removed).toBe(1);
    await client.destroy();
  });

  it('the returned value is a plain promise without cancel()', () => {
    const client = clientOver(new SilentWorker());
    const call = client.detectBpm(tiny(), { copy: true });
    // @ts-expect-error a worker call returns a Promise; cancel through a signal
    expect(call.cancel).toBeUndefined();
    void client.destroy();
    return call.catch(() => undefined);
  });
});

describe('OfflineWorkerClient progress', () => {
  it('onProgress on the request receives (number, string) and is not posted', async () => {
    const client = clientOver(new LoopbackWorker());
    const seen: Array<[unknown, unknown]> = [];
    const result = await client.analyze({
      samples: tone(2),
      onProgress: (progress, stage) => seen.push([progress, stage]),
    });
    expect(result.bpm).toBeGreaterThan(0);
    expect(seen.length).toBeGreaterThan(0);
    for (const [progress, stage] of seen) {
      expect(typeof progress).toBe('number');
      expect(typeof stage).toBe('string');
    }
    await client.destroy();
  });

  it('refuses a function-valued cancel field with a TypeError before dispatch', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const samples = new Float32Array([0, 0]);
    await expect(client.masterAudio({ samples, cancel: () => false } as never)).rejects.toThrow(
      TypeError,
    );
    expect(worker.posted).toHaveLength(0);
    expect(samples.byteLength).toBe(8);
    await client.destroy();
  });
});

describe('OfflineWorkerClient teardown', () => {
  it('destroy() returns a Promise, rejects pending calls with InvalidState, and is idempotent', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker, { terminateWorkerOnDestroy: true });
    const call = client.detectBpm(tiny(), { copy: true });
    const done = client.destroy();
    expect(done).toBeInstanceOf(Promise);
    await expect(call).rejects.toSatisfy(isCode(ErrorCode.InvalidState));
    await done;
    expect(worker.terminated).toBe(true);
    await expect(client.destroy()).resolves.toBeUndefined();
    await expect(client.detectBpm(tiny())).rejects.toSatisfy(isCode(ErrorCode.InvalidState));
  });

  it('[Symbol.asyncDispose] behaves like destroy()', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const call = client.detectBpm(tiny(), { copy: true });
    const done = client[Symbol.asyncDispose]();
    expect(done).toBeInstanceOf(Promise);
    await expect(call).rejects.toSatisfy(isCode(ErrorCode.InvalidState));
    await done;
    await expect(client[Symbol.asyncDispose]()).resolves.toBeUndefined();
  });

  it('does not terminate a host-owned worker unless asked', async () => {
    const worker = new SilentWorker();
    await clientOver(worker).destroy();
    expect(worker.terminated).toBe(false);
  });
});

describe('OfflineWorkerClient errors', () => {
  it('no Worker global and no worker option is NotSupported', () => {
    const saved = (globalThis as { Worker?: unknown }).Worker;
    (globalThis as { Worker?: unknown }).Worker = undefined;
    try {
      expect(() => new OfflineWorkerClient()).toThrow(
        expect.objectContaining({ code: ErrorCode.NotSupported }),
      );
    } finally {
      (globalThis as { Worker?: unknown }).Worker = saved;
    }
  });

  it('a malformed message rejects the pending call with InvalidFormat', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const call = client.detectBpm(tiny(), { copy: true });
    worker.reply({ type: 'nonsense' });
    await expect(call).rejects.toSatisfy(isCode(ErrorCode.InvalidFormat));
    await client.destroy();
  });

  it('keeps the class and code of an error the worker posts', async () => {
    const worker = new SilentWorker();
    const client = clientOver(worker);
    const call = client.detectBpm(tiny(), { copy: true });
    worker.reply({
      type: 'sonare:offline-error',
      id: 1,
      error: { name: 'SonareError', message: 'no codec', code: 6, codeName: 'NotSupported' },
    });
    await expect(call).rejects.toSatisfy(isCode(ErrorCode.NotSupported));
    await client.destroy();
  });
});

describe('OfflineWorkerClient types', () => {
  it('rejects the removed names at compile time', () => {
    const run = (client: OfflineWorkerClient): void => {
      // @ts-expect-error teardown is destroy()
      client.dispose();
      // @ts-expect-error renamed to terminateWorkerOnDestroy
      new OfflineWorkerClient({ terminateWorkerOnDispose: true });
      // @ts-expect-error the synchronous cancel predicate cannot cross the thread
      void client.masterAudio({ samples: new Float32Array(1), cancel: () => false });
      // @ts-expect-error progress is a request field, not a call option
      void client.detectBpm({ samples: new Float32Array(1) }, { onProgress: () => {} });
    };
    expect(typeof run).toBe('function');
  });
});
