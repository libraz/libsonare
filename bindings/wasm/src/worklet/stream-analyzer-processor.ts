import type { WorkletInput, WorkletOutput } from './audio_types.js';
import { isStreamAnalyzerMessage, requireIntegerOption } from './guards.js';
import type {
  SonareStreamAnalyzerChunkMessage,
  SonareStreamAnalyzerMessage,
  SonareStreamAnalyzerWorkletProcessorOptions,
  WorkletPort,
} from './messages.js';

/** Default and minimum mono samples per posted chunk. */
export const STREAM_ANALYZER_DEFAULT_CHUNK_FRAMES = 4096;
const STREAM_ANALYZER_MIN_CHUNK_FRAMES = 128;
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

  constructor(
    options: SonareStreamAnalyzerWorkletProcessorOptions = {},
    private readonly port?: WorkletPort,
  ) {
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

  /** Handles a control message; runs between render quanta. */
  receiveMessage(message: SonareStreamAnalyzerMessage): void {
    if (this.destroyed) {
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
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareStreamAnalyzerWorkletProcessor extends Base {
    private bridge: SonareStreamAnalyzerWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareStreamAnalyzerWorkletProcessorOptions }) {
      super();
      const port = this.port;
      this.bridge = new SonareStreamAnalyzerWorkletProcessor(options?.processorOptions ?? {}, port);
      const onMessage = (event: { data: unknown }) => {
        if (isStreamAnalyzerMessage(event.data)) {
          this.bridge.receiveMessage(event.data);
        }
      };
      if (port?.addEventListener) {
        port.addEventListener('message', onMessage);
        port.start?.();
      } else if (port) {
        port.onmessage = onMessage;
      }
    }

    process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
      return this.bridge.process(inputs, outputs);
    }
  }
  scope.registerProcessor(name, RegisteredSonareStreamAnalyzerWorkletProcessor);
}
