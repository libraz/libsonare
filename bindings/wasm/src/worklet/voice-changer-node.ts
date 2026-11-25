import { ErrorCode } from '../errors.js';
import { codedError, entryClass, refuse, refuseType, withEntry } from '../internal/entry.js';
import { failureFromPayload, isFailurePayload } from '../internal/failure_payload.js';
import { releasedHandleError } from '../internal/handle_guard.js';
import type {
  SonareRealtimeVoiceChangerErrorMessage,
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerNodeStatusReplyMessage,
  SonareRealtimeVoiceChangerReadyMessage,
} from '../internal/voice_changer_messages.js';
import type { RealtimeVoiceChangerConfig } from '../public_types.js';
import type {
  SonareRealtimeVoiceChangerNodeOptions,
  SonareRealtimeVoiceChangerNodeStatus,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
} from '../public_types_worklet.js';
import { assertKnownProcessorOptions, resolveContextSampleRate } from './guards.js';
import { isRecord } from './protocol.js';

/**
 * The realtime voice changer as an AudioWorklet node. Input 0 carries the voice,
 * mixed by the browser to the node's `numChannels`; input 1, when connected,
 * carries an external vocoder carrier (its first channel). The output has
 * `numChannels` channels. Connect a source to {@link SonareRealtimeVoiceChangerNode.node}
 * and the node to the destination.
 *
 * @example
 * ```ts
 * const voiceWorkletUrl = '/assets/voice-worklet.js'; // calls registerSonareRealtimeVoiceChangerWorkletProcessor()
 * const wasmUrl = '/assets/sonare.wasm'; // where the app serves @libraz/libsonare/wasm
 * const wasmBinary = await (await fetch(wasmUrl)).arrayBuffer();
 * const microphone = await navigator.mediaDevices.getUserMedia({ audio: true });
 * const voice = await SonareRealtimeVoiceChangerNode.create(context, {
 *   numChannels: 1,
 *   config: 'hardTune',
 *   moduleUrl: voiceWorkletUrl,
 *   wasmBinary,
 * });
 * await voice.ready;
 * context.createMediaStreamSource(microphone).connect(voice.node).connect(context.destination);
 * voice.onError((error) => console.warn(error.message));
 * ```
 */
@entryClass('SonareRealtimeVoiceChangerNode', ['post', 'refused', 'shutdown'])
export class SonareRealtimeVoiceChangerNode {
  /** AudioWorkletNode that runs the changer; connect a source to it and it to the destination. */
  readonly node: AudioWorkletNode;
  /** Resolves once the processor has prepared the changer; rejects if the worklet could not. */
  readonly ready: Promise<void>;
  private readonly errorListeners = new Set<(error: Error) => void>();
  private readonly pendingStatus: {
    resolve: (status: SonareRealtimeVoiceChangerNodeStatus) => void;
    reject: (reason: unknown) => void;
  }[] = [];
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private readyLatencySamples: number | undefined;
  private destroyed = false;
  private destroying: Promise<void> | undefined;

  private constructor(node: AudioWorkletNode) {
    this.node = node;
    // The consumer awaits the field, so a startup failure is named by it.
    this.ready = withEntry(
      'SonareRealtimeVoiceChangerNode.ready',
      () =>
        new Promise<void>((resolve, reject) => {
          this.resolveReady = resolve;
          this.rejectReady = reject;
        }),
    );
    // A caller that never awaits `ready` must not see an unhandled rejection.
    this.ready.catch(() => undefined);
    // A processor constructor that throws surfaces only as this event.
    this.node.addEventListener?.('processorerror', () => {
      this.rejectReady(codedError(ErrorCode.Unknown, 'the processor failed to start'));
    });
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      if (this.destroyed || !isRecord(event.data)) {
        return;
      }
      if (event.data.type === 'ready') {
        const ready = event.data as unknown as SonareRealtimeVoiceChangerReadyMessage;
        this.readyLatencySamples = ready.latencySamples;
        this.resolveReady();
      } else if (event.data.type === 'status') {
        const reply = event.data as unknown as SonareRealtimeVoiceChangerNodeStatusReplyMessage;
        this.pendingStatus.shift()?.resolve(reply.status);
      } else if (event.data.type === 'error') {
        this.refused(event.data as unknown as SonareRealtimeVoiceChangerErrorMessage);
      }
    };
  }

  /**
   * Creates the node on `context`. The processor must already be registered, or
   * `options.moduleUrl` names the worklet module that registers it. The worklet
   * initializes the module itself, from `options.wasmBinary` when the realm has
   * not done so.
   *
   * @throws TypeError naming an undeclared option, or a `numChannels` that is not a number;
   *   RangeError for a `numChannels` other than 1 or 2.
   */
  static async create(
    context: BaseAudioContext,
    options: SonareRealtimeVoiceChangerNodeOptions,
  ): Promise<SonareRealtimeVoiceChangerNode> {
    if (!isRecord(options)) {
      throw refuseType('options must be an object with numChannels');
    }
    assertKnownProcessorOptions('voiceChangerNode', options);
    const numChannels = options.numChannels;
    if (numChannels !== 1 && numChannels !== 2) {
      const message = 'numChannels must be 1 or 2';
      throw typeof numChannels === 'number' ? refuse(message) : refuseType(message);
    }
    const sampleRate = resolveContextSampleRate(options.sampleRate, context);
    if (options.moduleUrl && context.audioWorklet?.addModule) {
      await context.audioWorklet.addModule(options.moduleUrl);
    }
    const processorOptions: SonareRealtimeVoiceChangerWorkletProcessorOptions = {
      config: options.config,
      sampleRate,
      maxBlockSize: options.maxBlockSize,
      numChannels,
      wasmBinary: options.wasmBinary,
    };
    const factory =
      options.nodeFactory ??
      ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
        new AudioWorkletNode(ctx, name, nodeOptions));
    const node = factory(
      context,
      options.processorName ?? 'sonare-realtime-voice-changer-processor',
      {
        numberOfInputs: 2,
        numberOfOutputs: 1,
        outputChannelCount: [numChannels],
        channelCount: numChannels,
        channelCountMode: 'explicit',
        channelInterpretation: 'speakers',
        processorOptions,
      },
    );
    return new SonareRealtimeVoiceChangerNode(node);
  }

  /**
   * Latency of the chain in samples as the processor prepared it, or `undefined`
   * before {@link ready} resolves. A later {@link setConfig} can change it (the
   * true-peak limiter adds its look-ahead); {@link status} reads the current one.
   */
  get latencySamples(): number | undefined {
    return this.readyLatencySamples;
  }

  /**
   * Replaces the configuration with a complete flat config, the form
   * `RealtimeVoiceChanger.config()` and {@link status} return; the audio thread
   * does not resolve a preset. A refusal — a field outside its domain, or one of
   * the fields fixed at construction — leaves the previous configuration in
   * effect and is reported through {@link onError} with the text
   * `RealtimeVoiceChanger.setConfig` throws.
   */
  setConfig(config: RealtimeVoiceChangerConfig): void {
    if (!isRecord(config)) {
      throw refuseType('config must be a complete flat voice changer configuration');
    }
    this.post({ type: 'setConfig', config });
  }

  /** Holds a MIDI note for the stages that follow notes; `velocity` is in (0, 1]. Default `1`. */
  noteOn(note: number, velocity?: number): void {
    this.post(
      velocity === undefined ? { type: 'noteOn', note } : { type: 'noteOn', note, velocity },
    );
  }

  /** Releases a held MIDI note. */
  noteOff(note: number): void {
    this.post({ type: 'noteOff', note });
  }

  /** Releases every held MIDI note. */
  allNotesOff(): void {
    this.post({ type: 'allNotesOff' });
  }

  /** Clears the streaming DSP state and keeps the configuration; applied between render quanta. */
  reset(): void {
    this.post({ type: 'reset' });
  }

  /**
   * Reads the changer's latency, non-finite discard counter and applied
   * configuration from the worklet. Rejects with `InvalidState` once the node is
   * destroyed.
   */
  status(): Promise<SonareRealtimeVoiceChangerNodeStatus> {
    return new Promise((resolve, reject) => {
      this.post({ type: 'status' });
      this.pendingStatus.push({ resolve, reject });
    });
  }

  /**
   * Subscribes to control messages the processor refused, each with the class
   * and text the main-thread `RealtimeVoiceChanger` throws for the same call.
   * Returns the unsubscribe function.
   */
  onError(callback: (error: Error) => void): () => void {
    this.errorListeners.add(callback);
    return () => {
      this.errorListeners.delete(callback);
    };
  }

  /**
   * Stops the node and releases its processor; pending status reads reject with
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

  private post(message: SonareRealtimeVoiceChangerMessage): void {
    if (this.destroyed) {
      throw releasedHandleError('SonareRealtimeVoiceChangerNode');
    }
    this.node.port.postMessage(message);
  }

  private refused(message: SonareRealtimeVoiceChangerErrorMessage): void {
    const error = isFailurePayload(message.failure)
      ? failureFromPayload(message.failure)
      : codedError(ErrorCode.Unknown, String(message.message ?? 'AudioWorklet error'));
    if (message.request === 'init') {
      this.rejectReady(error);
    } else if (message.request === 'status') {
      this.pendingStatus.shift()?.reject(error);
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
      this.node.port.postMessage({ type: 'destroy' } satisfies SonareRealtimeVoiceChangerMessage);
    } catch {
      // Best effort: local ownership is released even when the port rejects dispatch.
    }
    this.node.disconnect();
    this.node.port.onmessage = null;
    this.errorListeners.clear();
    const released = releasedHandleError('SonareRealtimeVoiceChangerNode');
    for (const pending of this.pendingStatus.splice(0)) {
      pending.reject(released);
    }
    this.rejectReady(released);
  }
}
