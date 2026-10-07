import { HrtfSet, PlaybackRenderer } from '../index.js';
import type { PlaybackRendererConfig } from '../public_types_playback.js';
import type { WasmPlaybackRenderer } from '../sonare.js';
import { copyPlanesToOutput, type WorkletInput, type WorkletOutput } from './audio_types.js';
import { isPlaybackMessage, requireIntegerOption } from './guards.js';
import type {
  SonarePlaybackDiagnosticsReplyMessage,
  SonarePlaybackErrorMessage,
  SonarePlaybackMessage,
  SonarePlaybackNodeOptions,
  SonarePlaybackWorkletProcessorOptions,
  WorkletPort,
} from './messages.js';

/** Largest input channel count any layout accepts (7.1). */
const MAX_INPUT_CHANNELS = 8;
const SPEAKER_LAYOUT_CHANNELS: Readonly<Record<string, number>> = { stereo: 2, '5.1': 6, '7.1': 8 };

function configObject(config: PlaybackRendererConfig | string | undefined): PlaybackRendererConfig {
  if (config === undefined) {
    return {};
  }
  return typeof config === 'string' ? (JSON.parse(config) as PlaybackRendererConfig) : config;
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
    this.port = port;
    this.maxBlockSize = requireIntegerOption(options.maxBlockSize, 128, 'maxBlockSize', 1);
    const scopeRate = (globalThis as { sampleRate?: unknown }).sampleRate;
    const sampleRate = options.sampleRate ?? (typeof scopeRate === 'number' ? scopeRate : 48000);
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
      hrtf?.delete();
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
  diagnostics(): SonarePlaybackDiagnosticsReplyMessage['diagnostics'] {
    return {
      ...this.renderer.diagnostics(),
      unsupported_input_blocks: this.unsupportedInputBlocks,
    };
  }

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
        plane.set(copied === source.length ? source : source.subarray(0, copied));
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

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.renderer.delete();
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

export function registerSonarePlaybackWorkletProcessor(name = 'sonare-playback-processor'): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonarePlaybackWorkletProcessor extends Base {
    private bridge: SonarePlaybackWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonarePlaybackWorkletProcessorOptions }) {
      super();
      const port = this.port;
      this.bridge = new SonarePlaybackWorkletProcessor(options?.processorOptions ?? {}, port);
      const onMessage = (event: { data: unknown }) => {
        if (isPlaybackMessage(event.data)) {
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
 * await context.audioWorklet.addModule(playbackWorkletUrl);
 * const hrtf = await (await fetch(hrtfUrl)).arrayBuffer();
 * const node = createSonarePlaybackNode(context, { config: {}, hrtf });
 * context.createMediaElementSource(video).connect(node).connect(context.destination);
 * video.addEventListener('seeking', () => node.port.postMessage({ type: 'reset' }));
 * ```
 */
export function createSonarePlaybackNode(
  context: BaseAudioContext,
  options: SonarePlaybackNodeOptions = {},
): AudioWorkletNode {
  const config = configObject(options.config);
  const factory =
    options.nodeFactory ??
    ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
      new AudioWorkletNode(ctx, name, nodeOptions));
  const processorOptions: SonarePlaybackWorkletProcessorOptions = {
    config: JSON.stringify(config),
    hrtf: options.hrtf,
    maxBlockSize: options.maxBlockSize,
    sampleRate: options.sampleRate ?? context.sampleRate,
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
