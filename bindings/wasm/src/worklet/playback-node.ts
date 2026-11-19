import { ErrorCode, SonareError } from '../errors.js';
import { init, isInitialized } from '../index.js';
import { entryClass } from '../internal/entry.js';
import type {
  SonarePlaybackDiagnosticsReplyMessage,
  SonarePlaybackErrorMessage,
  SonarePlaybackMessage,
} from '../internal/worklet_messages.js';
import { getSonareModule } from '../module_state.js';
import type { PlaybackRendererConfig } from '../public_types_playback.js';
import type {
  SonarePlaybackNodeOptions,
  SonarePlaybackWorkletProcessorDiagnostics,
  SonarePlaybackWorkletProcessorOptions,
} from '../public_types_worklet.js';
import { assertKnownProcessorOptions, resolveContextSampleRate } from './guards.js';
import { isRecord } from './protocol.js';

function configObject(config: PlaybackRendererConfig | undefined): PlaybackRendererConfig {
  if (config === undefined) {
    return {};
  }
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError('SonarePlaybackNode.create: config must be a configuration object');
  }
  return config;
}

/**
 * The playback renderer as an AudioWorklet node. Its input follows its source's
 * channel count without any browser up/down mix (`channelCountMode: "max"`,
 * `channelInterpretation: "discrete"`), and its output carries the target's
 * channel count, which the library reads from the configuration before the
 * processor exists. Connect a source to {@link SonarePlaybackNode.node} and the
 * node to the destination.
 *
 * @example
 * ```ts
 * const playbackWorkletUrl = '/assets/playback-worklet.js'; // the app's worklet bundle
 * const hrtfUrl = '/assets/hrtf/default.shrf'; // where the app serves the packaged asset
 * const video = document.querySelector('video') as HTMLVideoElement;
 * const hrtf = await (await fetch(hrtfUrl)).arrayBuffer();
 * const playback = await SonarePlaybackNode.create(context, {
 *   config: {},
 *   hrtf,
 *   moduleUrl: playbackWorkletUrl, // calls registerSonarePlaybackWorkletProcessor()
 * });
 * context.createMediaElementSource(video).connect(playback.node).connect(context.destination);
 * video.addEventListener('seeking', () => playback.reset());
 * ```
 */
@entryClass('SonarePlaybackNode')
export class SonarePlaybackNode {
  /** AudioWorkletNode that renders the playback; connect a source to it and it to the destination. */
  readonly node: AudioWorkletNode;
  /** Resolves once the processor has started; rejects if the worklet could not start it. */
  readonly ready: Promise<void>;
  private readonly errorListeners = new Set<(error: SonareError) => void>();
  private readonly pendingDiagnostics: {
    resolve: (diagnostics: SonarePlaybackWorkletProcessorDiagnostics) => void;
    reject: (reason: unknown) => void;
  }[] = [];
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private destroyed = false;
  private destroying: Promise<void> | undefined;

  private constructor(node: AudioWorkletNode) {
    this.node = node;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    // A caller that never awaits `ready` must not see an unhandled rejection.
    this.ready.catch(() => undefined);
    // A processor constructor that throws surfaces only as this event.
    this.node.addEventListener?.('processorerror', () => {
      this.rejectReady(
        new SonareError(ErrorCode.Unknown, 'SonarePlaybackNode: the processor failed to start'),
      );
    });
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      if (this.destroyed || !isRecord(event.data)) {
        return;
      }
      if (event.data.type === 'ready') {
        this.resolveReady();
      } else if (event.data.type === 'diagnostics') {
        const reply = event.data as unknown as SonarePlaybackDiagnosticsReplyMessage;
        this.pendingDiagnostics.shift()?.resolve(reply.diagnostics);
      } else if (event.data.type === 'error') {
        this.refused(event.data as unknown as SonarePlaybackErrorMessage);
      }
    };
  }

  /**
   * Creates the node on `context`. The module is initialized on the main
   * thread first when it is not yet (with `options.wasmBinary` when given),
   * since the output channel count is read from the configuration there. The
   * processor must already be registered, or `options.moduleUrl` names the
   * worklet module that registers it.
   *
   * @throws TypeError naming an undeclared option, or a config that is not an object.
   * @throws SonareError `InvalidFormat` when the configuration names no known target layout.
   */
  static async create(
    context: BaseAudioContext,
    options: SonarePlaybackNodeOptions = {},
  ): Promise<SonarePlaybackNode> {
    assertKnownProcessorOptions('SonarePlaybackNode.create', 'playbackNode', options);
    const config = configObject(options.config);
    const sampleRate = resolveContextSampleRate(
      options.sampleRate,
      context,
      'SonarePlaybackNode.create',
    );
    if (!isInitialized()) {
      await init(options.wasmBinary === undefined ? undefined : { wasmBinary: options.wasmBinary });
    }
    const outputChannels = getSonareModule().playbackConfigOutputChannels(JSON.stringify(config));
    if (options.moduleUrl && context.audioWorklet?.addModule) {
      await context.audioWorklet.addModule(options.moduleUrl);
    }
    const processorOptions: SonarePlaybackWorkletProcessorOptions = {
      config,
      hrtf: options.hrtf,
      maxBlockSize: options.maxBlockSize,
      sampleRate,
      wasmBinary: options.wasmBinary,
    };
    const factory =
      options.nodeFactory ??
      ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
        new AudioWorkletNode(ctx, name, nodeOptions));
    const node = factory(context, options.processorName ?? 'sonare-playback-processor', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [outputChannels],
      channelCountMode: 'max',
      channelInterpretation: 'discrete',
      processorOptions,
    });
    return new SonarePlaybackNode(node);
  }

  /** Replaces the renderer configuration; a refused one is reported through {@link onError}. */
  setConfig(config: PlaybackRendererConfig): void {
    this.post({ type: 'config', config: configObject(config) });
  }

  /** Sets the listener's head orientation in degrees; see `PlaybackRenderer.setHeadOrientation`. */
  setHeadOrientation(yaw: number, pitch = 0, roll = 0): void {
    this.post({ type: 'orientation', yaw, pitch, roll });
  }

  /** Clears the pipeline, typically after a seek; applied between render quanta. */
  reset(): void {
    this.post({ type: 'reset' });
  }

  /**
   * The renderer's diagnostics plus the worklet's unsupported-block counter.
   * Rejects with `InvalidState` once the node is destroyed.
   */
  diagnostics(): Promise<SonarePlaybackWorkletProcessorDiagnostics> {
    return new Promise((resolve, reject) => {
      this.post({ type: 'diagnostics' });
      this.pendingDiagnostics.push({ resolve, reject });
    });
  }

  /**
   * Subscribes to control messages the processor refused (a configuration it
   * could not apply, for one). Returns the unsubscribe function.
   */
  onError(callback: (error: SonareError) => void): () => void {
    this.errorListeners.add(callback);
    return () => {
      this.errorListeners.delete(callback);
    };
  }

  /**
   * Stops the node and releases its processor; pending diagnostics reject with
   * `InvalidState`. A second call returns the same promise.
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

  private post(message: SonarePlaybackMessage): void {
    if (this.destroyed) {
      throw new SonareError(ErrorCode.InvalidState, 'SonarePlaybackNode is destroyed.');
    }
    this.node.port.postMessage(message);
  }

  private refused(message: SonarePlaybackErrorMessage): void {
    const error = new SonareError(
      ErrorCode.Unknown,
      String(message.message ?? 'AudioWorklet error'),
    );
    if (message.request === 'init') {
      this.rejectReady(error);
    } else if (message.request === 'diagnostics') {
      this.pendingDiagnostics.shift()?.reject(error);
    }
    for (const listener of this.errorListeners) {
      listener(error);
    }
  }

  private shutdown(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    try {
      this.node.port.postMessage({ type: 'destroy' } satisfies SonarePlaybackMessage);
    } catch {
      // Best effort: local ownership is released even when the port rejects dispatch.
    }
    this.node.disconnect();
    this.node.port.onmessage = null;
    this.errorListeners.clear();
    const released = new SonareError(ErrorCode.InvalidState, 'SonarePlaybackNode is destroyed.');
    for (const pending of this.pendingDiagnostics.splice(0)) {
      pending.reject(released);
    }
    this.rejectReady(released);
  }
}
