import { ErrorCode } from '../errors.js';
import { RealtimeVoiceChanger } from '../index.js';
import { codedError, refuse, refuseType, withEntry } from '../internal/entry.js';
import type {
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerNodeStatusReplyMessage,
  SonareRealtimeVoiceChangerReadyMessage,
} from '../internal/voice_changer_messages.js';
import { WorkletProcessorLifecycle } from '../internal/worklet_lifecycle.js';
import type { WorkletPort } from '../internal/worklet_messages.js';
import type {
  SonareRealtimeVoiceChangerNodeStatus,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
} from '../public_types_worklet.js';
import type {
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerPlanarBuffer,
} from '../realtime_voice_changer.js';
import {
  copyPlanesToOutput,
  leadingSamples,
  type WorkletInput,
  type WorkletOutput,
} from './audio_types.js';
import {
  assertKnownProcessorOptions,
  isRealtimeVoiceChangerMessage,
  requireIntegerOption,
  resolveScopeSampleRate,
} from './guards.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './render_quantum.js';

/** AudioWorklet-side bridge that runs a {@link RealtimeVoiceChanger} over the worklet's input and output blocks and applies control messages from the main thread. */
export class SonareRealtimeVoiceChangerWorkletProcessor {
  private static warnedMonoOverflow = false;
  private static warnedPlanarOverflow = false;
  private changer: RealtimeVoiceChanger;
  private readonly sampleRate: number;
  private readonly maxBlockSize: number;
  private readonly numChannels: number;
  // Zero-copy heap views, acquired in the constructor so process() never allocates on the
  // full-block path. A block shorter than maxBlockSize gets its own set on first use.
  private readonly monoBuffers = new Map<number, RealtimeVoiceChangerMonoBuffer>();
  private readonly planarBuffers = new Map<number, RealtimeVoiceChangerPlanarBuffer>();
  // Reused so the mono fan-out never builds an array on the audio thread.
  private readonly monoPlane: Float32Array[] = [new Float32Array(0)];
  private destroyed = false;
  /** Where a status request is answered; the registered processor points it at its port. */
  reply?: (message: SonareRealtimeVoiceChangerNodeStatusReplyMessage) => void;

  constructor(options: SonareRealtimeVoiceChangerWorkletProcessorOptions) {
    assertKnownProcessorOptions('voiceChangerProcessor', options);
    this.sampleRate = resolveScopeSampleRate(options.sampleRate);
    this.maxBlockSize = requireIntegerOption(
      options.maxBlockSize,
      AUDIO_WORKLET_RENDER_QUANTUM,
      'maxBlockSize',
      1,
    );
    const numChannels = options.numChannels;
    if (numChannels !== 1 && numChannels !== 2) {
      const message = 'SonareRealtimeVoiceChangerWorkletProcessor: numChannels must be 1 or 2';
      throw typeof numChannels === 'number' ? refuse(message) : refuseType(message);
    }
    this.numChannels = numChannels;
    this.changer = new RealtimeVoiceChanger({
      sampleRate: this.sampleRate,
      maxBlockSize: this.maxBlockSize,
      numChannels,
      config: options.config ?? 'neutralMonitor',
    });
    try {
      this.monoBuffer(this.maxBlockSize);
      if (this.numChannels > 1) {
        this.planarBuffer(this.maxBlockSize);
      }
    } catch (error) {
      this.changer.destroy();
      throw error;
    }
  }

  /**
   * Handles a control-plane message from the main thread. AudioWorklet port
   * handlers run on the same audio rendering thread as `process()`: this path
   * therefore accepts only a complete flat config and never a preset document. Anything that is
   * not a control message is ignored.
   */
  receiveMessage(message: unknown): void {
    if (this.destroyed || !isRealtimeVoiceChangerMessage(message)) {
      return;
    }
    if (message.type === 'status') {
      this.reply?.({ type: 'status', status: this.status() });
    } else if (message.type === 'setConfig') {
      this.changer.setConfig(message.config);
    } else if (message.type === 'noteOn') {
      this.changer.noteOn(message.note, message.velocity);
    } else if (message.type === 'noteOff') {
      this.changer.noteOff(message.note);
    } else if (message.type === 'allNotesOff') {
      this.changer.allNotesOff();
    } else if (message.type === 'reset') {
      this.changer.reset();
    } else if (message.type === 'destroy') {
      this.destroy();
    }
  }

  /** The changer's latency, discard counter and applied configuration. */
  status(): SonareRealtimeVoiceChangerNodeStatus {
    return {
      latencySamples: this.changer.latencySamples,
      nonFiniteDiscardCount: this.changer.nonFiniteDiscardCount,
      config: this.changer.config(),
    };
  }

  /** Processes one render quantum: input 0 is the signal, input 1 (first plane) the optional external vocoder carrier. Returns `false` once destroyed. */
  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    const output = outputs[0];
    if (this.destroyed || !output || output.length === 0) {
      return !this.destroyed;
    }

    const input = inputs[0];
    const carrier = inputs[1]?.[0];
    const requestedFrames = output[0]?.length ?? 0;
    const requestedChannels = Math.min(this.numChannels, output.length);
    if (requestedFrames === 0 || requestedChannels === 0) {
      return true;
    }

    if (requestedChannels === 1) {
      // Clamp to the pre-allocated capacity; warn (once) if the host violated
      // the contract. We never reallocate on the audio thread.
      const frames = this.clampFrames(requestedFrames, 'mono');
      const buffer = this.monoBuffer(frames);
      const source = input?.[0];
      const target = buffer.input;
      if (source) {
        target.set(leadingSamples(source, frames));
      } else {
        target.fill(0, 0, frames);
      }
      this.loadCarrier(buffer.carrier, carrier, frames);
      buffer.processWithCarrier();
      // One plane fans out to every output channel and zeroes the rest: a mono
      // processor under AudioWorkletNode's default `channelCountMode: 'max'`
      // still gets a 2-channel output, which writing only output[0] hard-panned.
      this.monoPlane[0] = buffer.output;
      copyPlanesToOutput(output, this.monoPlane, frames);
      return true;
    }

    const frames = this.clampFrames(requestedFrames, 'planar');
    const buffer = this.planarBuffer(frames);
    const planes = buffer.channels;
    // Planar zero-copy path: AudioWorklet's input[ch] is already a Float32Array per channel, so
    // it is set() straight into the heap-backed plane and processed in place.
    for (let ch = 0; ch < requestedChannels; ch++) {
      const src = input?.[ch];
      const dst = planes[ch];
      if (!dst) {
        continue;
      }
      if (src) {
        dst.set(leadingSamples(src, frames));
      } else {
        dst.fill(0, 0, frames);
      }
    }
    this.loadCarrier(buffer.carrier, carrier, frames);
    buffer.processWithCarrier();
    // Same rule as the mono branch: an output channel past the last processed
    // plane is silence rather than a duplicate of plane 0, and the tail past
    // `frames` is zeroed.
    copyPlanesToOutput(output, planes, frames);
    return true;
  }

  /** Releases the changer and its buffers. Idempotent; later `process()` calls return `false`. */
  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.changer.destroy();
  }

  /** Same as {@link destroy}, for `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private monoBuffer(frames: number): RealtimeVoiceChangerMonoBuffer {
    let buffer = this.monoBuffers.get(frames);
    if (!buffer) {
      buffer = this.changer.createRealtimeMonoBuffer(frames);
      this.monoBuffers.set(frames, buffer);
    }
    return buffer;
  }

  private planarBuffer(frames: number): RealtimeVoiceChangerPlanarBuffer {
    let buffer = this.planarBuffers.get(frames);
    if (!buffer) {
      buffer = this.changer.createRealtimePlanarBuffer(frames, this.numChannels as 1 | 2);
      this.planarBuffers.set(frames, buffer);
    }
    return buffer;
  }

  // A missing or short carrier plane is silence; the carrier is only read for an external source.
  private loadCarrier(
    target: Float32Array,
    source: Float32Array | undefined,
    frames: number,
  ): void {
    if (source) {
      const copied = Math.min(source.length, frames);
      target.set(leadingSamples(source, copied));
      if (copied < frames) {
        target.fill(0, copied, frames);
      }
    } else {
      target.fill(0, 0, frames);
    }
  }

  /**
   * Returns the number of frames we can actually process given the
   * pre-allocated capacity. If the host requests more frames than the
   * worst-case block size declared at construction time, we clamp to the
   * available capacity and warn once; we MUST NOT reallocate on the
   * realtime audio thread.
   */
  private clampFrames(frames: number, kind: 'mono' | 'planar'): number {
    if (frames <= this.maxBlockSize) {
      return frames;
    }
    const mono = kind === 'mono';
    if (
      mono
        ? !SonareRealtimeVoiceChangerWorkletProcessor.warnedMonoOverflow
        : !SonareRealtimeVoiceChangerWorkletProcessor.warnedPlanarOverflow
    ) {
      if (mono) {
        SonareRealtimeVoiceChangerWorkletProcessor.warnedMonoOverflow = true;
      } else {
        SonareRealtimeVoiceChangerWorkletProcessor.warnedPlanarOverflow = true;
      }
      // biome-ignore lint/suspicious/noConsole: realtime-safety diagnostic.
      console.warn(
        `SonareRealtimeVoiceChangerWorkletProcessor: requested ${frames} ${kind} frames ` +
          `exceeds pre-allocated capacity ${this.maxBlockSize}; clamping. ` +
          'Increase maxBlockSize at construction time to avoid this.',
      );
    }
    return this.maxBlockSize;
  }
}

/**
 * Registers the voice-changer processor under `name` in the AudioWorklet global scope. Default `'sonare-realtime-voice-changer-processor'`.
 * @throws SonareError `NotSupported` outside an AudioWorklet scope.
 */
export function registerSonareRealtimeVoiceChangerWorkletProcessor(
  name = 'sonare-realtime-voice-changer-processor',
): void {
  withEntry('registerSonareRealtimeVoiceChangerWorkletProcessor', () => {
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
    class RegisteredSonareRealtimeVoiceChangerWorkletProcessor extends Base {
      private readonly lifecycle: WorkletProcessorLifecycle<
        SonareRealtimeVoiceChangerWorkletProcessor,
        SonareRealtimeVoiceChangerMessage
      >;
      readonly port?: WorkletPort;

      constructor(options?: {
        processorOptions?: SonareRealtimeVoiceChangerWorkletProcessorOptions;
      }) {
        super();
        const port = this.port;
        const processorOptions: Partial<SonareRealtimeVoiceChangerWorkletProcessorOptions> =
          options?.processorOptions ?? {};
        assertKnownProcessorOptions('voiceChangerProcessor', processorOptions);
        this.lifecycle = new WorkletProcessorLifecycle({
          port,
          usesModule: true,
          wasmBinary: processorOptions.wasmBinary,
          create: () => {
            // Options cross from the main thread unchecked; the constructor refuses a missing field.
            const bridge = new SonareRealtimeVoiceChangerWorkletProcessor(
              processorOptions as SonareRealtimeVoiceChangerWorkletProcessorOptions,
            );
            bridge.reply = (message) => port?.postMessage?.(message);
            return bridge;
          },
          ready: (bridge) =>
            ({
              type: 'ready',
              runtimeTarget: 'embind',
              latencySamples: bridge.status().latencySamples,
            }) satisfies SonareRealtimeVoiceChangerReadyMessage,
          kind: 'voice changer',
          accepts: isRealtimeVoiceChangerMessage,
          deliver: (bridge, message) => bridge.receiveMessage(message),
        });
      }

      process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
        return this.lifecycle.process(inputs, outputs);
      }
    }
    scope.registerProcessor(name, RegisteredSonareRealtimeVoiceChangerWorkletProcessor);
  });
}
