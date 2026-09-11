import { Worker as NodeWorker } from 'node:worker_threads';
import { describe, expect, it } from 'vitest';
import { ErrorCode, isSonareError, OfflineWorkerClient } from '../dist/index.js';
import { installOfflineWorkerEndpoint } from '../dist/worker.js';

type MessageListener = (event: MessageEvent) => void;

/** The client's worker option; the interface itself is not exported. */
type OfflineWorkerLike = NonNullable<
  NonNullable<ConstructorParameters<typeof OfflineWorkerClient>[0]>['worker']
>;

/**
 * A structured-clone loopback for the Worker protocol. It mirrors the boundary
 * that Node's worker_threads provides while keeping the test portable to the
 * browser-oriented Vitest environment.
 */
class LoopbackWorker {
  private clientMessage: MessageListener | undefined;
  private clientError: MessageListener | undefined;
  private workerMessage: MessageListener | undefined;
  terminated = false;

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
    if (type === 'error') {
      this.clientError = listener as unknown as MessageListener;
    }
  }

  removeEventListener(type: string, listener: EventListener): void {
    if (type === 'message' && this.clientMessage === listener) {
      this.clientMessage = undefined;
    }
    if (type === 'error' && this.clientError === listener) {
      this.clientError = undefined;
    }
  }

  postMessage(message: unknown, transfer?: Transferable[]): void {
    try {
      const cloned = structuredClone(message, { transfer });
      queueMicrotask(() => this.workerMessage?.({ data: cloned } as MessageEvent));
    } catch (error) {
      queueMicrotask(() =>
        this.clientError?.({ data: error, message: String(error) } as unknown as MessageEvent),
      );
    }
  }

  terminate(): void {
    this.terminated = true;
  }
}

function makeTone(seconds = 2, sampleRate = 22050): Float32Array {
  const samples = new Float32Array(seconds * sampleRate);
  for (let i = 0; i < samples.length; ++i) {
    samples[i] = 0.35 * Math.sin((2 * Math.PI * 220 * i) / sampleRate);
  }
  return samples;
}

function clientWith(worker: LoopbackWorker): OfflineWorkerClient {
  return new OfflineWorkerClient({
    workerFactory: () => worker as unknown as Worker,
  });
}

describe('OfflineWorkerClient', () => {
  it('runs analysis off the calling endpoint and relays progress', async () => {
    const worker = new LoopbackWorker();
    const client = clientWith(worker);
    const progress: string[] = [];
    const result = await client.analyze(
      { samples: makeTone() },
      { copy: true, onProgress: ({ stage }) => progress.push(stage) },
    );

    expect(result.bpm).toBeGreaterThan(0);
    expect(progress.length).toBeGreaterThan(0);
    client.dispose();
    expect(worker.terminated).toBe(true);
  });

  it('transfers inputs by default and preserves them with copy: true', async () => {
    const worker = new LoopbackWorker();
    const client = clientWith(worker);
    const transferred = makeTone();
    const task = client.detectBpm({ samples: transferred });
    expect(transferred.byteLength).toBe(0);
    expect(await task).toBeGreaterThan(0);

    const copied = makeTone();
    expect(await client.detectBpm({ samples: copied }, { copy: true })).toBeGreaterThan(0);
    expect(copied.byteLength).toBeGreaterThan(0);
    client.dispose();
  });

  it('cancels before native work begins through the shared control flag', async () => {
    const worker = new LoopbackWorker();
    const client = clientWith(worker);
    const task = client.analyze({ samples: makeTone(4) }, { copy: true });
    task.cancel();

    await expect(task.result).rejects.toSatisfy(
      (error: unknown) => isSonareError(error) && error.code === ErrorCode.Cancelled,
    );
    client.dispose();
  });

  it('cancels work already running inside the native call', async () => {
    // The case above cancels BEFORE the native call starts, which the pre-call
    // guard already handled. This one cancels while the call is running, which
    // needs a real thread: on the loopback the cancel message and the progress
    // callback both arrive through queueMicrotask and cannot interleave with a
    // synchronous native call, so only the SharedArrayBuffer flag written from
    // another thread can reach it mid-flight.
    //
    // Note what this must NOT assert on its own. Before the fix the cancelled
    // run still rejected with Cancelled — the post-call guard saw the flag once
    // the work had finished — so an error-class assertion passes against the
    // defect. Measured on the pre-fix bundle: 11 progress ticks and Cancelled,
    // versus 11 ticks and completed uncancelled. The tick COUNT is the only
    // thing that separates "unwound early" from "ran to the end and then
    // reported cancelled", so it is the assertion that carries this test.
    const analyzeTicks = async (cancelOnFirstTick: boolean): Promise<number> => {
      const worker = new NodeWorker(
        new URL('./fixtures/offline-worker-node-entry.mjs', import.meta.url),
      );
      const client = new OfflineWorkerClient({
        worker: worker as unknown as OfflineWorkerLike,
        terminateWorkerOnDispose: true,
      });
      try {
        let ticks = 0;
        const task = client.analyze(
          { samples: makeTone(20) },
          {
            copy: true,
            onProgress: () => {
              ticks += 1;
              if (cancelOnFirstTick && ticks === 1) {
                task.cancel();
              }
            },
          },
        );
        if (cancelOnFirstTick) {
          await expect(task.result).rejects.toSatisfy(
            (error: unknown) => isSonareError(error) && error.code === ErrorCode.Cancelled,
          );
        } else {
          expect((await task.result).bpm).toBeGreaterThan(0);
        }
        return ticks;
      } finally {
        client.dispose();
      }
    };

    const uncancelled = await analyzeTicks(false);
    // Positive control: without several ticks to cut short there is nothing for
    // the comparison below to measure.
    expect(uncancelled).toBeGreaterThan(4);

    const cancelled = await analyzeTicks(true);
    expect(cancelled).toBeLessThan(uncancelled / 2);
  });

  it('works through a real Node worker_threads boundary', async () => {
    const worker = new NodeWorker(
      new URL('./fixtures/offline-worker-node-entry.mjs', import.meta.url),
    );
    const client = new OfflineWorkerClient({
      // `OfflineWorker.postMessage` takes DOM `Transferable[]`, worker_threads
      // takes its own `Transferable[]`; the two unions are not subsets of each
      // other (OffscreenCanvas vs X509Certificate), so neither side is
      // assignable even under method bivariance. The runtime contract holds —
      // this test is what proves it.
      worker: worker as unknown as OfflineWorkerLike,
      terminateWorkerOnDispose: true,
    });
    const samples = makeTone();
    const bpm = await client.detectBpm({ samples });

    expect(samples.byteLength).toBe(0);
    expect(bpm).toBeGreaterThan(0);
    client.dispose();
  });
});
