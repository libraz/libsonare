import { ErrorCode, SonareError } from '../errors.js';
import { HrtfSet, PlaybackRenderer } from '../index.js';
import type {
  SonarePlaybackDiagnosticsReplyMessage,
  SonarePlaybackErrorMessage,
  SonarePlaybackMessage,
} from '../internal/worklet_messages.js';
import type { PlaybackRendererConfig } from '../public_types_playback.js';
import type {
  PlaybackNodeOptions,
  SonarePlaybackWorkletProcessorDiagnostics,
  SonarePlaybackWorkletProcessorOptions,
  WorkletPort,
} from '../public_types_worklet.js';
import type { WasmPlaybackRenderer } from '../sonare.js';
import {
  copyPlanesToOutput,
  leadingSamples,
  type WorkletInput,
  type WorkletOutput,
} from './audio_types.js';
import { initializeEmbindInWorklet, isEmbindReady } from './embind-init.js';
import {
  assertKnownProcessorOptions,
  isPlaybackMessage,
  requireIntegerOption,
  resolveContextSampleRate,
  resolveScopeSampleRate,
} from './guards.js';

/** Largest input channel count any layout accepts (7.1). */
const MAX_INPUT_CHANNELS = 8;
const SPEAKER_LAYOUT_CHANNELS: Readonly<Record<string, number>> = { stereo: 2, '5.1': 6, '7.1': 8 };

function configObject(config: PlaybackRendererConfig | undefined): PlaybackRendererConfig {
  if (config === undefined) {
    return {};
  }
  if (typeof config !== 'object' || config === null || Array.isArray(config)) {
    throw new TypeError('config must be a configuration object');
  }
  return config;
}

/** The target's channel count, known before any renderer exists (headphones: 2). */
function outputChannelCount(config: PlaybackRendererConfig): number {
  if (config.target?.kind !== 'speakers') {
    return 2;
  }
  const count = SPEAKER_LAYOUT_CHANNELS[config.target.layout ?? ''];
  if (count === undefined) {
    throw new RangeError('target.layout must be "stereo", "5.1" or "7.1" for a speakers target');
  }
  return count;
}

/**
 * The playback renderer inside an AudioWorklet.
 *
 * `inputs[0].length` is the block's input channel count, so a
 * `MediaElementAudioSourceNode` whose channel count follows the media drives
 * `input.layout: "auto"` directly. A block with no input channels (no active
 * connection: paused, ended, loading) renders silence on the active layout. A
 * block whose channel count the renderer refuses renders silence the same way
 * and counts toward `unsupported_input_blocks`; either way the renderer
 * advances by the block, so the timeline never shifts. Nothing throws on the
 * audio thread.
 *
 * Every buffer is allocated in the constructor: `process()` copies into WASM
 * heap planes and allocates nothing.
 */
export class SonarePlaybackWorkletProcessor {
  private static warnedBlockOverflow = false;
  private readonly renderer: PlaybackRenderer;
  private readonly native: WasmPlaybackRenderer;
  private readonly port?: WorkletPort;
  private readonly maxBlockSize: number;
  private readonly outputChannels: number;
  private inputPlanes: Float32Array[] = [];
  private outputPlanes: Float32Array[] = [];
  private unsupportedInputBlocks = 0;
  private destroyed = false;

  constructor(options: SonarePlaybackWorkletProcessorOptions = {}, port?: WorkletPort) {
    assertKnownProcessorOptions('SonarePlaybackWorkletProcessor', 'playbackProcessor', options);
    this.port = port;
    this.maxBlockSize = requireIntegerOption(options.maxBlockSize, 128, 'maxBlockSize', 1);
    const sampleRate = resolveScopeSampleRate(options.sampleRate, 'SonarePlaybackWorkletProcessor');
    const hrtf = options.hrtf ? HrtfSet.fromBytes(new Uint8Array(options.hrtf)) : undefined;
    try {
      this.renderer = new PlaybackRenderer({
        config: options.config ?? {},
        hrtf,
        sampleRate,
        maxBlockSize: this.maxBlockSize,
      });
    } finally {
      // The renderer keeps its own copy of the set.
      hrtf?.destroy();
    }
    // One reviewed read of the facade's private handle, for the heap-plane path.
    this.native = (this.renderer as unknown as { native: WasmPlaybackRenderer }).native;
    this.outputChannels = this.native.outputChannels();
    this.acquirePlanes();
  }

  /**
   * Handles a control-plane message. AudioWorklet port handlers run on the
   * rendering thread between `process()` calls, which is what makes `reset`
   * safe here. A refused message is answered with an `error` message.
   *
   * @internal
   */
  receiveMessage(message: SonarePlaybackMessage): void {
    if (this.destroyed) {
      return;
    }
    try {
      switch (message.type) {
        case 'config':
          this.renderer.setConfig(message.config);
          break;
        case 'orientation':
          this.renderer.setHeadOrientation(message.yaw, message.pitch ?? 0, message.roll ?? 0);
          break;
        case 'reset':
          this.renderer.reset();
          break;
        case 'diagnostics':
          this.port?.postMessage?.({
            type: 'diagnostics',
            diagnostics: this.diagnostics(),
          } satisfies SonarePlaybackDiagnosticsReplyMessage);
          break;
        case 'destroy':
          this.destroy();
          break;
      }
    } catch (error) {
      this.port?.postMessage?.({
        type: 'error',
        request: message.type,
        message: error instanceof Error ? error.message : String(error),
      } satisfies SonarePlaybackErrorMessage);
    }
  }

  /** The renderer's diagnostics plus `unsupported_input_blocks`, as a plain object. */
  diagnostics(): SonarePlaybackWorkletProcessorDiagnostics {
    return {
      ...this.renderer.diagnostics(),
      unsupported_input_blocks: this.unsupportedInputBlocks,
    };
  }

  /**
   * Renders one quantum from `inputs[0]` through the playback renderer into `outputs[0]`. A block
   * the renderer refuses renders silence and the timeline still advances; nothing throws.
   *
   * @returns `false` once the processor is destroyed, else `true`.
   */
  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.destroyed) {
      return false;
    }
    const output = outputs[0];
    const requested = output?.[0]?.length ?? 0;
    if (!output || requested === 0) {
      return true;
    }
    const frames = this.clampFrames(requested);
    // Heap views detach when WASM linear memory grows; the storage behind them
    // never moves, so re-acquiring is allocation-free on the native side.
    if (this.inputPlanes[0]?.byteLength === 0 || this.outputPlanes[0]?.byteLength === 0) {
      this.acquirePlanes();
    }

    const input = inputs[0];
    const inChannels = input?.length ?? 0;
    let code: number;
    if (inChannels === 0) {
      code = this.native.processPreparedSilence(frames);
    } else if (inChannels > MAX_INPUT_CHANNELS) {
      this.unsupportedInputBlocks++;
      code = this.native.processPreparedSilence(frames);
    } else {
      for (let ch = 0; ch < inChannels; ch++) {
        const source = input[ch];
        const plane = this.inputPlanes[ch];
        const copied = Math.min(frames, source.length);
        plane.set(leadingSamples(source, copied));
        if (copied < frames) {
          plane.fill(0, copied, frames);
        }
      }
      code = this.native.processPrepared(inChannels, frames);
      if (code !== 0) {
        // A refused block leaves the renderer untouched; render silence instead.
        this.unsupportedInputBlocks++;
        code = this.native.processPreparedSilence(frames);
      }
    }
    if (code === 0) {
      copyPlanesToOutput(output, this.outputPlanes, frames);
    } else {
      for (const channel of output) {
        channel.fill(0);
      }
    }
    return true;
  }

  /** Releases the renderer; later `process()` calls return `false`. Safe to call twice. */
  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.renderer.destroy();
  }

  /** Same as {@link destroy}, for `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private acquirePlanes(): void {
    this.inputPlanes = [];
    for (let ch = 0; ch < MAX_INPUT_CHANNELS; ch++) {
      this.inputPlanes.push(this.native.inputPlane(ch));
    }
    this.outputPlanes = [];
    for (let ch = 0; ch < this.outputChannels; ch++) {
      this.outputPlanes.push(this.native.outputPlane(ch));
    }
  }

  // Frames past the construction-time capacity are left silent rather than
  // reallocating on the audio thread.
  private clampFrames(frames: number): number {
    if (frames <= this.maxBlockSize) {
      return frames;
    }
    if (!SonarePlaybackWorkletProcessor.warnedBlockOverflow) {
      SonarePlaybackWorkletProcessor.warnedBlockOverflow = true;
      // biome-ignore lint/suspicious/noConsole: realtime-safety diagnostic.
      console.warn(
        `SonarePlaybackWorkletProcessor: requested ${frames} frames exceeds maxBlockSize ` +
          `${this.maxBlockSize}; clamping.`,
      );
    }
    return this.maxBlockSize;
  }
}

/**
 * Registers the playback processor in an AudioWorkletGlobalScope.
 *
 * @param name Processor name. Default `'sonare-playback-processor'`.
 * @throws SonareError `NotSupported` outside an AudioWorkletGlobalScope.
 */
export function registerSonarePlaybackWorkletProcessor(name = 'sonare-playback-processor'): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new SonareError(
      ErrorCode.NotSupported,
      'AudioWorkletProcessor is not available in this context.',
    );
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonarePlaybackWorkletProcessor extends Base {
    private bridge?: SonarePlaybackWorkletProcessor;
    private readonly pendingMessages: SonarePlaybackMessage[] = [];
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonarePlaybackWorkletProcessorOptions }) {
      super();
      const port = this.port;
      const processorOptions = options?.processorOptions ?? {};
      assertKnownProcessorOptions(name, 'playbackProcessor', processorOptions);
      const start = () => {
        this.bridge = new SonarePlaybackWorkletProcessor(processorOptions, port);
        for (const message of this.pendingMessages.splice(0)) {
          this.bridge.receiveMessage(message);
        }
      };
      if (isEmbindReady()) {
        start();
      } else {
        // The module is not initialized in this realm: build it the way the
        // realtime engine processor does, then construct the renderer.
        initializeEmbindInWorklet(processorOptions.wasmBinary)
          .then(() => {
            start();
            port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
          })
          .catch((error: unknown) => {
            port?.postMessage?.({
              type: 'error',
              request: 'init',
              message: error instanceof Error ? error.message : String(error),
            } satisfies SonarePlaybackErrorMessage);
          });
      }
      const onMessage = (event: { data: unknown }) => {
        if (!isPlaybackMessage(event.data)) {
          return;
        }
        if (this.bridge) {
          this.bridge.receiveMessage(event.data);
        } else if (this.pendingMessages.length < 1024) {
          this.pendingMessages.push(event.data);
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
      if (this.bridge) {
        return this.bridge.process(inputs, outputs);
      }
      for (const channel of outputs[0] ?? []) {
        channel.fill(0);
      }
      return true;
    }
  }
  scope.registerProcessor(name, RegisteredSonarePlaybackWorkletProcessor);
}

/**
 * Creates the AudioWorkletNode for a processor registered with
 * {@link registerSonarePlaybackWorkletProcessor}. The node's input follows its
 * source's channel count without any browser up/down mix
 * (`channelCountMode: "max"`, `channelInterpretation: "discrete"`), and its
 * output carries the target's channel count. The configuration is serialized
 * here, on the main thread.
 *
 * @example
 * ```ts
 * const playbackWorkletUrl = '/assets/playback-worklet.js'; // the app's worklet bundle
 * const hrtfUrl = '/assets/hrtf/default.shrf'; // where the app serves the packaged asset
 * const video = document.querySelector('video') as HTMLVideoElement;
 * await context.audioWorklet.addModule(playbackWorkletUrl);
 * const hrtf = await (await fetch(hrtfUrl)).arrayBuffer();
 * const node = createPlaybackNode(context, { config: {}, hrtf });
 * context.createMediaElementSource(video).connect(node).connect(context.destination);
 * video.addEventListener('seeking', () => node.port.postMessage({ type: 'reset' }));
 * ```
 */
export function createPlaybackNode(
  context: BaseAudioContext,
  options: PlaybackNodeOptions = {},
): AudioWorkletNode {
  assertKnownProcessorOptions('createPlaybackNode', 'playbackNode', options);
  const config = configObject(options.config);
  const factory =
    options.nodeFactory ??
    ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
      new AudioWorkletNode(ctx, name, nodeOptions));
  const processorOptions: SonarePlaybackWorkletProcessorOptions = {
    config,
    hrtf: options.hrtf,
    maxBlockSize: options.maxBlockSize,
    sampleRate: resolveContextSampleRate(options.sampleRate, context, 'createPlaybackNode'),
    wasmBinary: options.wasmBinary,
  };
  return factory(context, options.processorName ?? 'sonare-playback-processor', {
    numberOfInputs: 1,
    numberOfOutputs: 1,
    outputChannelCount: [outputChannelCount(config)],
    channelCountMode: 'max',
    channelInterpretation: 'discrete',
    processorOptions,
  });
}
