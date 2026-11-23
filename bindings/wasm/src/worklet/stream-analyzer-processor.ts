import { ErrorCode } from '../errors.js';
import { codedError, withEntry } from '../internal/entry.js';
import type { SonareStreamAnalyzerChunkMessage } from '../internal/stream_analyzer_messages.js';
import { WorkletProcessorLifecycle } from '../internal/worklet_lifecycle.js';
import type { WorkletPort } from '../internal/worklet_messages.js';
import type {
  SonareStreamAnalyzerWorkletProcessorConstructorOptions,
  SonareStreamAnalyzerWorkletProcessorOptions,
} from '../public_types_worklet.js';
import type { WorkletInput, WorkletOutput } from './audio_types.js';
import {
  assertKnownProcessorOptions,
  isStreamAnalyzerMessage,
  requireIntegerOption,
} from './guards.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './render_quantum.js';

/** Default and minimum mono samples per posted chunk. */
export const STREAM_ANALYZER_DEFAULT_CHUNK_FRAMES = 4096;
// A chunk holds at least one render quantum.
const STREAM_ANALYZER_MIN_CHUNK_FRAMES = AUDIO_WORKLET_RENDER_QUANTUM;
// Chunk buffers in flight at once: one filling, the rest posted or awaiting return.
const POOL_BUFFERS = 4;
const EMPTY = new Float32Array(0);

/** Resolves the `chunkFrames` option shared by the processor and the node. */
export function resolveStreamAnalyzerChunkFrames(chunkFrames: number | undefined): number {
  return requireIntegerOption(
    chunkFrames,
    STREAM_ANALYZER_DEFAULT_CHUNK_FRAMES,
    'chunkFrames',
    STREAM_ANALYZER_MIN_CHUNK_FRAMES,
  );
}

/**
 * Audio-thread tap for `SonareStreamAnalyzerNode`: averages the input channels
 * to mono, fills a preallocated chunk and transfers it to the main thread once
 * full. The analyzer itself never runs here.
 *
 * Buffers come from a fixed pool the main thread refills through `recycle`
 * messages. When the pool is empty the chunk being filled is dropped rather than
 * allocated, and the next posted chunk reports the discontinuity.
 */
export class SonareStreamAnalyzerWorkletProcessor {
  private readonly chunkFrames: number;
  private readonly pool: Array<Float32Array | undefined> = new Array(POOL_BUFFERS);
  private poolCount = 0;
  private current: Float32Array | undefined;
  private fill = 0;
  private chunkStart = 0;
  private discontinuity = false;
  private destroyed = false;
  // Reused for every post: the clone is taken synchronously, so no per-chunk objects.
  private readonly chunkMessage: SonareStreamAnalyzerChunkMessage;
  private readonly transferList: ArrayBuffer[] = [];

  private readonly port?: WorkletPort;

  constructor(options: SonareStreamAnalyzerWorkletProcessorConstructorOptions = {}) {
    assertKnownProcessorOptions('streamAnalyzerProcessorConstructor', options);
    this.port = options.port;
    this.chunkFrames = resolveStreamAnalyzerChunkFrames(options.chunkFrames);
    for (let i = 0; i < POOL_BUFFERS; i++) {
      this.recycle(new Float32Array(this.chunkFrames));
    }
    this.current = this.takeBuffer();
    this.chunkMessage = {
      type: 'chunk',
      samples: EMPTY,
      startSample: 0,
      discontinuity: false,
    };
    this.port?.postMessage?.({ type: 'ready' });
  }

  /** Handles a control message; runs between render quanta. Anything that is not a control message is ignored. */
  receiveMessage(message: unknown): void {
    if (this.destroyed || !isStreamAnalyzerMessage(message)) {
      return;
    }
    if (message.type === 'recycle') {
      this.recycle(message.samples);
    } else if (message.type === 'destroy') {
      this.destroyed = true;
      this.current = undefined;
      this.poolCount = 0;
    }
  }

  /** Downmixes the first input to mono and forwards it in chunks of `chunkFrames`; returns `false` once destroyed. */
  process(inputs: WorkletInput, _outputs: WorkletOutput): boolean {
    if (this.destroyed) {
      return false;
    }
    const input = inputs[0];
    if (!input || input.length === 0) {
      return true;
    }
    const channels = input.length;
    const frames = input[0].length;
    const scale = 1 / channels;
    let done = 0;
    while (done < frames) {
      const take = Math.min(frames - done, this.chunkFrames - this.fill);
      const target = this.current;
      if (target) {
        const base = this.fill;
        const first = input[0];
        for (let i = 0; i < take; i++) {
          target[base + i] = first[done + i];
        }
        for (let ch = 1; ch < channels; ch++) {
          const plane = input[ch];
          for (let i = 0; i < take; i++) {
            target[base + i] += plane[done + i];
          }
        }
        if (channels > 1) {
          for (let i = 0; i < take; i++) {
            target[base + i] *= scale;
          }
        }
      }
      this.fill += take;
      done += take;
      if (this.fill === this.chunkFrames) {
        this.completeChunk();
      }
    }
    return true;
  }

  private completeChunk(): void {
    const full = this.current;
    if (full) {
      const message = this.chunkMessage;
      message.samples = full;
      message.startSample = this.chunkStart;
      message.discontinuity = this.discontinuity;
      this.transferList[0] = full.buffer as ArrayBuffer;
      this.port?.postMessage?.(message, this.transferList);
      message.samples = EMPTY;
      this.discontinuity = false;
    } else {
      this.discontinuity = true;
    }
    this.chunkStart += this.chunkFrames;
    this.fill = 0;
    this.current = this.takeBuffer();
  }

  private takeBuffer(): Float32Array | undefined {
    if (this.poolCount === 0) {
      return undefined;
    }
    this.poolCount--;
    const buffer = this.pool[this.poolCount];
    this.pool[this.poolCount] = undefined;
    return buffer;
  }

  private recycle(samples: Float32Array): void {
    if (samples.length !== this.chunkFrames || this.poolCount >= POOL_BUFFERS) {
      return;
    }
    // A buffer returned while a chunk is being dropped waits for the next boundary.
    this.pool[this.poolCount++] = samples;
  }
}

/**
 * Registers {@link SonareStreamAnalyzerWorkletProcessor} under `name`. Call it
 * from an AudioWorklet module; the processor needs no WASM in that realm.
 */
export function registerSonareStreamAnalyzerWorkletProcessor(
  name = 'sonare-stream-analyzer-processor',
): void {
  withEntry('registerSonareStreamAnalyzerWorkletProcessor', () => {
    const scope = globalThis as unknown as {
      AudioWorkletProcessor?: new () => object;
      registerProcessor?: (processorName: string, processorCtor: unknown) => void;
    };
    if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
      throw codedError(
        ErrorCode.NotSupported,
        'AudioWorkletProcessor is not available in this context.',
      );
    }
    const Base = scope.AudioWorkletProcessor;
    class RegisteredSonareStreamAnalyzerWorkletProcessor extends Base {
      private readonly lifecycle: WorkletProcessorLifecycle<SonareStreamAnalyzerWorkletProcessor>;
      readonly port?: WorkletPort;

      constructor(options?: { processorOptions?: SonareStreamAnalyzerWorkletProcessorOptions }) {
        super();
        const port = this.port;
        const processorOptions = options?.processorOptions ?? {};
        assertKnownProcessorOptions('streamAnalyzerProcessor', processorOptions);
        this.lifecycle = new WorkletProcessorLifecycle({
          port,
          usesModule: false,
          // The processor posts its own ready from its constructor.
          create: () => new SonareStreamAnalyzerWorkletProcessor({ ...processorOptions, port }),
          ready: () => undefined,
          deliver: (bridge, data) => {
            if (isStreamAnalyzerMessage(data)) {
              bridge.receiveMessage(data);
            }
          },
        });
      }

      process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
        return this.lifecycle.process(inputs, outputs);
      }
    }
    scope.registerProcessor(name, RegisteredSonareStreamAnalyzerWorkletProcessor);
  });
}
