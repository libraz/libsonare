import { ErrorCode } from '../errors.js';
import type { StreamFrames, StreamStats } from '../index.js';
import { init, isInitialized, StreamAnalyzer } from '../index.js';
import { codedError, entryClass, withEntry } from '../internal/entry.js';
import type { SonareStreamAnalyzerChunkMessage } from '../internal/stream_analyzer_messages.js';
import type {
  SonareStreamAnalyzerNodeOptions,
  SonareStreamAnalyzerWorkletProcessorOptions,
} from '../public_types_worklet.js';
import {
  assertKnownProcessorOptions,
  isStreamAnalyzerChunkMessage,
  resolveContextSampleRate,
} from './guards.js';
import { isRecord } from './protocol.js';
import { resolveStreamAnalyzerChunkFrames } from './stream-analyzer-processor.js';

/**
 * Streams analysis of an audio graph branch. The AudioWorklet side only
 * downmixes to mono and forwards chunks; the `StreamAnalyzer` runs on the main
 * thread inside this object, so analysis competes with other main-thread work
 * and lags the audio by about one chunk.
 *
 * The node is a sink with one input and no output: connect a source to
 * {@link SonareStreamAnalyzerNode.node}. Audio lost between the processor and
 * the analyzer (a dropped chunk or a processor restart) resets the analyzer's
 * sample offset instead of splicing the streams together.
 *
 * @example
 * ```ts
 * const processorUrl = '/assets/stream-analyzer-worklet.js'; // the app's worklet bundle
 * const source = context.createMediaElementSource(document.querySelector('audio') as HTMLAudioElement);
 * const draw = (mel: unknown, nFrames: number) => console.log(mel, nFrames); // the app's renderer
 * await context.audioWorklet.addModule(processorUrl); // calls registerSonareStreamAnalyzerWorkletProcessor()
 * const analyzer = await SonareStreamAnalyzerNode.create(context, { config: { nMels: 64 } });
 * source.connect(analyzer.node);
 * const stop = analyzer.onFrames((frames) => draw(frames.mel, frames.nFrames));
 * ```
 */
@entryClass('SonareStreamAnalyzerNode', ['shutdown', 'consume'])
export class SonareStreamAnalyzerNode {
  /** AudioWorkletNode that receives the audio; connect a source to it. */
  readonly node: AudioWorkletNode;
  /** Resolves once the processor has started; rejects if the worklet reports an error. */
  readonly ready: Promise<void>;
  private readonly analyzer: StreamAnalyzer;
  private readonly frameListeners = new Set<(frames: StreamFrames) => void>();
  private readonly statsListeners = new Set<(stats: StreamStats) => void>();
  private expectedSample = 0;
  private lastFrameCount = 0;
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private destroyed = false;
  private destroying: Promise<void> | undefined;

  private constructor(node: AudioWorkletNode, analyzer: StreamAnalyzer) {
    this.node = node;
    this.analyzer = analyzer;
    // The consumer awaits the field, so a startup failure is named by it.
    this.ready = withEntry(
      'SonareStreamAnalyzerNode.ready',
      () =>
        new Promise<void>((resolve, reject) => {
          this.resolveReady = resolve;
          this.rejectReady = reject;
        }),
    );
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      if (this.destroyed) {
        return;
      }
      if (isStreamAnalyzerChunkMessage(event.data)) {
        this.consume(event.data);
      } else if (isRecord(event.data) && event.data.type === 'ready') {
        this.resolveReady();
      } else if (isRecord(event.data) && event.data.type === 'error') {
        this.rejectReady(
          codedError(ErrorCode.Unknown, String(event.data.message ?? 'AudioWorklet error')),
        );
      }
    };
  }

  /**
   * Creates the analyzer and its AudioWorkletNode on `context`, initializing the module first
   * when it is not yet. The processor module must already be registered, or `options.moduleUrl` names it.
   */
  static async create(
    context: BaseAudioContext,
    options: SonareStreamAnalyzerNodeOptions = {},
  ): Promise<SonareStreamAnalyzerNode> {
    assertKnownProcessorOptions('streamAnalyzerNode', options);
    const chunkFrames = resolveStreamAnalyzerChunkFrames(options.chunkFrames);
    const processorName = options.processorName ?? 'sonare-stream-analyzer-processor';
    if (options.moduleUrl && context.audioWorklet?.addModule) {
      await context.audioWorklet.addModule(options.moduleUrl);
    }
    // The analyzer runs on this thread, so the module is initialized here when it is not yet.
    if (!isInitialized()) {
      await init();
    }
    // Built before the node so an invalid configuration leaves nothing behind.
    const analyzer = new StreamAnalyzer({
      ...options.config,
      sampleRate: resolveContextSampleRate(options.config?.sampleRate, context),
    });
    const processorOptions: SonareStreamAnalyzerWorkletProcessorOptions = { chunkFrames };
    const factory =
      options.nodeFactory ??
      ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
        new AudioWorkletNode(ctx, name, nodeOptions));
    let node: AudioWorkletNode;
    try {
      node = factory(context, processorName, {
        numberOfInputs: 1,
        numberOfOutputs: 0,
        // The processor averages the channels itself, so the browser must not remix first.
        channelCountMode: 'max',
        channelInterpretation: 'discrete',
        processorOptions,
      });
    } catch (error) {
      analyzer.destroy();
      throw error;
    }
    return new SonareStreamAnalyzerNode(node, analyzer);
  }

  /** Subscribes to analysis frames, delivered in the analyzer's `readFrames` format. */
  onFrames(callback: (frames: StreamFrames) => void): () => void {
    this.frameListeners.add(callback);
    return () => {
      this.frameListeners.delete(callback);
    };
  }

  /**
   * Subscribes to analyzer statistics, delivered when the key or tempo estimate
   * was re-estimated (`estimate.updated`).
   */
  onStats(callback: (stats: StreamStats) => void): () => void {
    this.statsListeners.add(callback);
    return () => {
      this.statsListeners.delete(callback);
    };
  }

  /**
   * Stops the node and releases its resources; pending requests reject with
   * `InvalidState`. Resolves once the node can no longer deliver a message to
   * the caller. A second call returns the same promise.
   */
  destroy(): Promise<void> {
    this.shutdown();
    this.destroying ??= Promise.resolve();
    return this.destroying;
  }

  /** Same as {@link destroy}, for `await using`. */
  [Symbol.asyncDispose](): Promise<void> {
    return this.destroy();
  }

  private shutdown(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    try {
      this.node.port.postMessage({ type: 'destroy' });
    } catch {
      // Best effort: local ownership is released even when the port rejects dispatch.
    }
    this.node.disconnect();
    this.frameListeners.clear();
    this.statsListeners.clear();
    // A chunk already in flight must not reach the released analyzer.
    this.node.port.onmessage = null;
    this.analyzer.destroy();
  }

  private consume(chunk: SonareStreamAnalyzerChunkMessage): void {
    const { samples, startSample } = chunk;
    if (chunk.discontinuity || startSample !== this.expectedSample) {
      this.analyzer.reset(startSample);
      this.lastFrameCount = 0;
    }
    this.expectedSample = startSample + samples.length;
    this.analyzer.processWithOffset(samples, startSample);
    this.node.port.postMessage({ type: 'recycle', samples }, [samples.buffer]);
    // Frames wait in the analyzer's bounded queue until someone listens.
    if (this.frameListeners.size > 0) {
      const available = this.analyzer.availableFrames;
      if (available > 0) {
        const frames = this.analyzer.readFrames(available);
        for (const listener of this.frameListeners) {
          listener(frames);
        }
      }
    }
    if (this.statsListeners.size > 0) {
      const frameCount = this.analyzer.frameCount;
      if (frameCount !== this.lastFrameCount) {
        this.lastFrameCount = frameCount;
        const stats = this.analyzer.stats();
        if (stats.estimate.updated) {
          for (const listener of this.statsListeners) {
            listener(stats);
          }
        }
      }
    }
  }
}
