import { RealtimeVoiceChanger } from '../index';
import type { WorkletInput, WorkletOutput } from './audio_types';
import { isRealtimeVoiceChangerMessage } from './guards';
import type {
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  WorkletPort,
} from './messages';

export class SonareRealtimeVoiceChangerWorkletProcessor {
  private static warnedMonoOverflow = false;
  private static warnedInterleavedOverflow = false;
  private changer: RealtimeVoiceChanger;
  private readonly sampleRate: number;
  private readonly blockSize: number;
  private readonly channelCount: number;
  // WASM-heap typed-memory views, sized to the worst case (blockSize *
  // channelCount). Acquired on the main thread (constructor) so the
  // audio-thread process() never crosses an allocation boundary.
  private monoInput: Float32Array;
  private monoOutput: Float32Array;
  // Planar heap-backed views (one Float32Array per channel) used by the
  // multi-channel path. AudioWorklet inputs/outputs are already planar
  // Float32Arrays, so this avoids the per-sample interleave/deinterleave
  // passes that the older interleaved path needed.
  private planarChannels: Float32Array[];
  private destroyed = false;

  constructor(options: SonareRealtimeVoiceChangerWorkletProcessorOptions = {}) {
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.channelCount = Math.max(1, Math.floor(options.channelCount ?? 1));
    this.changer = new RealtimeVoiceChanger(options.preset ?? 'neutral-monitor');
    this.changer.prepare(this.sampleRate, this.blockSize, this.channelCount);
    // Acquire WASM-heap views once, sized to the worst case. These are alive
    // for the lifetime of the changer; if the host requests more frames per
    // process() than blockSize, we clamp (see ensure*Capacity).
    this.monoInput = this.changer.getMonoInputBuffer(this.blockSize);
    this.monoOutput = this.changer.getMonoOutputBuffer(this.blockSize);
    this.planarChannels = [];
    if (this.channelCount > 1) {
      for (let ch = 0; ch < this.channelCount; ch++) {
        this.planarChannels.push(this.changer.getPlanarChannelBuffer(ch, this.blockSize));
      }
    }
  }

  /**
   * Handles a control-plane message from the main thread. Runs on the
   * AudioWorklet global scope but OUTSIDE of `process()` (i.e. outside the
   * realtime audio callback), so it is safe to perform JSON parsing and
   * DSP coefficient recomputation here. `setConfig` MUST NOT be deferred
   * into `process()` because that would block the audio thread for longer
   * than one render quantum (e.g. 128 samples / 44.1 kHz = ~2.9 ms).
   */
  receiveMessage(message: SonareRealtimeVoiceChangerMessage): void {
    if (this.destroyed) {
      return;
    }
    if (message.type === 'setConfig') {
      // Apply synchronously on the message-handler thread. `setConfig` may
      // allocate and parse JSON internally; doing it here keeps `process()`
      // realtime-safe.
      this.changer.setConfig(message.preset);
    } else if (message.type === 'reset') {
      this.changer.reset();
    } else if (message.type === 'destroy') {
      this.destroy();
    }
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    const output = outputs[0];
    if (this.destroyed || !output || output.length === 0) {
      return !this.destroyed;
    }

    // The cached heap views can detach if WASM linear memory grows (the embind
    // module is built ALLOW_MEMORY_GROWTH). Re-acquire them if detached
    // (byteLength === 0) before touching them; in the common no-growth case this
    // is a cheap branch with no allocation.
    if (this.monoInput.byteLength === 0) {
      this.reacquireBuffers();
    }

    const input = inputs[0];
    const requestedFrames = output[0]?.length ?? 0;
    const requestedChannels = Math.min(this.channelCount, output.length);
    if (requestedFrames === 0 || requestedChannels === 0) {
      return true;
    }

    if (requestedChannels === 1) {
      // Clamp to the pre-allocated capacity; warn (once) if the host violated
      // the contract. We never reallocate on the audio thread.
      const frames = this.ensureMonoCapacity(requestedFrames);
      const source = input?.[0];
      if (source) {
        this.monoInput.set(source.subarray(0, frames));
      } else {
        this.monoInput.fill(0, 0, frames);
      }
      this.changer.processMonoInto(
        this.monoInput.subarray(0, frames),
        this.monoOutput.subarray(0, frames),
      );
      output[0].set(this.monoOutput.subarray(0, frames));
      return true;
    }

    const frames = this.ensureInterleavedCapacity(requestedFrames, requestedChannels);
    const channels = requestedChannels;
    // Planar zero-copy path: AudioWorklet's input[ch] is already a
    // Float32Array per channel, so we set() straight into the heap-backed
    // planar view and processPreparedPlanar runs in place.
    for (let ch = 0; ch < channels; ch++) {
      const src = input?.[ch];
      const dst = this.planarChannels[ch];
      if (!dst) {
        continue;
      }
      if (src) {
        dst.set(src.subarray(0, frames));
      } else {
        dst.fill(0, 0, frames);
      }
    }
    this.changer.processPreparedPlanar(frames);
    for (let ch = 0; ch < channels; ch++) {
      const src = this.planarChannels[ch];
      if (src) {
        output[ch].set(src.subarray(0, frames));
      }
      // No `for frame` inner loop needed; output[ch] is a Float32Array.
    }
    return true;
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.changer.delete();
  }

  // Re-acquires the cached WASM-heap views after a memory-growth detachment.
  // The underlying C++ vectors are pre-warmed (ensure_*_capacity ran at prepare
  // time), so getMono*/getPlanar* return fresh views onto the SAME storage
  // without reallocating it.
  private reacquireBuffers(): void {
    this.monoInput = this.changer.getMonoInputBuffer(this.blockSize);
    this.monoOutput = this.changer.getMonoOutputBuffer(this.blockSize);
    if (this.channelCount > 1) {
      for (let ch = 0; ch < this.channelCount; ch++) {
        this.planarChannels[ch] = this.changer.getPlanarChannelBuffer(ch, this.blockSize);
      }
    }
  }

  /**
   * Returns the number of frames we can actually process given the
   * pre-allocated capacity. If the host requests more frames than the
   * worst-case block size declared at construction time, we clamp to the
   * available capacity and warn once — we MUST NOT reallocate on the
   * realtime audio thread.
   */
  private ensureMonoCapacity(frames: number): number {
    const capacity = this.monoInput.length;
    if (frames <= capacity) {
      return frames;
    }
    if (!SonareRealtimeVoiceChangerWorkletProcessor.warnedMonoOverflow) {
      SonareRealtimeVoiceChangerWorkletProcessor.warnedMonoOverflow = true;
      // biome-ignore lint/suspicious/noConsole: realtime-safety diagnostic.
      console.warn(
        `SonareRealtimeVoiceChangerWorkletProcessor: requested ${frames} mono frames ` +
          `exceeds pre-allocated capacity ${capacity}; clamping. ` +
          'Increase blockSize at construction time to avoid this.',
      );
    }
    return capacity;
  }

  /**
   * Same contract as ensureMonoCapacity but for the planar per-channel
   * scratch. Returns the number of frames that fit in the available capacity.
   */
  private ensureInterleavedCapacity(frames: number, channels: number): number {
    const capacity = this.planarChannels[0]?.length ?? 0;
    if (frames <= capacity) {
      return frames;
    }
    if (!SonareRealtimeVoiceChangerWorkletProcessor.warnedInterleavedOverflow) {
      SonareRealtimeVoiceChangerWorkletProcessor.warnedInterleavedOverflow = true;
      // biome-ignore lint/suspicious/noConsole: realtime-safety diagnostic.
      console.warn(
        `SonareRealtimeVoiceChangerWorkletProcessor: requested ${frames}x${channels} ` +
          `planar frames exceeds pre-allocated capacity ${capacity}; clamping. ` +
          'Increase blockSize or channelCount at construction time to avoid this.',
      );
    }
    return capacity;
  }
}

export function registerSonareRealtimeVoiceChangerWorkletProcessor(
  name = 'sonare-realtime-voice-changer-processor',
): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareRealtimeVoiceChangerWorkletProcessor extends Base {
    private bridge: SonareRealtimeVoiceChangerWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: {
      processorOptions?: SonareRealtimeVoiceChangerWorkletProcessorOptions;
    }) {
      super();
      const port = this.port;
      this.bridge = new SonareRealtimeVoiceChangerWorkletProcessor(options?.processorOptions ?? {});
      const onMessage = (event: { data: unknown }) => {
        if (isRealtimeVoiceChangerMessage(event.data)) {
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
  scope.registerProcessor(name, RegisteredSonareRealtimeVoiceChangerWorkletProcessor);
}
