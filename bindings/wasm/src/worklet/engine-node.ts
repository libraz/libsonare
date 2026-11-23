import { ErrorCode } from '../errors.js';
import type { Articulation, EngineCaptureStatus, EngineTransportState } from '../index.js';
import {
  activeEntry,
  codedError,
  entryClass,
  invalidState,
  nameNativeError,
  refuse,
  withEntry,
} from '../internal/entry.js';
import { externalMidiEvent } from '../internal/external_midi.js';
import { failureFromPayload, isFailurePayload } from '../internal/failure_payload.js';
import { assertHandleLive, releasedHandleError } from '../internal/handle_guard.js';
import type {
  EngineClipPageRequestMessage,
  EngineSyncErrorMessage,
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessageInternal,
  SonareEngineReadRequestMessage,
  SonareEngineSyncMessage,
  SonareEngineTransportResponseMessage,
} from '../internal/worklet_messages.js';
import { OwnerEpoch } from '../owner_epoch.js';
import type { EngineTelemetry, ExternalMidiEvent } from '../public_types_engine.js';
import type {
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
} from '../public_types_worklet.js';
import { engineCapabilities } from '../realtime_engine.js';
import { resolveRenderFrame } from '../validation.js';
import { externalMidiEventFromWire, telemetryFromRecord } from './engine-records.js';
import {
  assertKnownProcessorOptions,
  engineCaptureResponseRequestId,
  isClipPageRequestMessage,
  isEngineCaptureResponseForOperation,
  isEngineCaptureResponseMessage,
  isEngineInsertGainReductionResponseMessage,
  isEngineReadResponseMessage,
  isEngineTelemetryRecord,
  isEngineTransportResponseMessage,
  isExternalMidiBatchMessage,
  isMeterSnapshot,
  isScopeSnapshot,
  requireChannelCount,
  requireIntegerOption,
  resolveContextSampleRate,
  resolveScopeBandCount,
} from './guards.js';
import {
  type ClipPageRequestRingBuffer,
  createClipPageRequestRingBuffer,
  createEngineTelemetryRingBuffer,
  createExternalMidiRingBuffer,
  createMeterRingBuffer,
  createScopeRingBuffer,
  createSonareEngineCommandRingBuffer,
  type EngineTelemetryRingBuffer,
  type ExternalMidiRingBuffer,
  isRecord,
  isUint32Slot,
  type MeterRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readClipPageRequestRingBuffer,
  readEngineTelemetryRingBuffer,
  readExternalMidiRingBuffer,
  readMeterRingBuffer,
  readScopeRingBuffer,
  type ScopeRingBuffer,
  SONARE_MAX_PUBLIC_PPQ,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  sonareEngineCommandRingHasRoom,
  type WorkletMeterSnapshot,
  type WorkletScopeSnapshot,
} from './protocol.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './render_quantum.js';

// The record writer stores 64-bit lanes as safe integers, so a bigint past that range is refused.
function isFiniteInteger(value: number | bigint | undefined): boolean {
  if (value === undefined) {
    return true;
  }
  return Number.isSafeInteger(typeof value === 'bigint' ? Number(value) : value);
}

function isPublicPpq(value: number | undefined): boolean {
  return (
    value !== undefined && Number.isFinite(value) && value >= 0 && value <= SONARE_MAX_PUBLIC_PPQ
  );
}

function isTrackMonitorMode(value: number | bigint | undefined): boolean {
  if (value === undefined) {
    return false;
  }
  const mode = typeof value === 'bigint' ? Number(value) : value;
  return typeof mode === 'number' && Number.isSafeInteger(mode) && mode >= 0 && mode <= 2;
}

/** Whether `command` is a well-formed record the ring and the worklet both accept. */
export function isValidCommandRecord(command: SonareEngineCommandRecord): boolean {
  const type = Number(command.type);
  if (
    !Number.isSafeInteger(type) ||
    type < SonareEngineCommandType.SetParam ||
    (type > SonareEngineCommandType.SeekMarker &&
      type !== SonareEngineCommandType.SetTrackMonitorMode &&
      type !== SonareEngineCommandType.ResetMasterLoudnessMeter &&
      type !== SonareEngineCommandType.ResetProcessorState)
  ) {
    return false;
  }
  return (
    // The record writer refuses a targetId outside the slot by throwing, and
    // this path answers false instead, so it must reject the same domain here.
    (command.targetId === undefined || isUint32Slot(command.targetId)) &&
    (command.argFloat === undefined || Number.isFinite(command.argFloat)) &&
    isFiniteInteger(command.argInt) &&
    isFiniteInteger(command.renderFrame) &&
    (command.syncSequence === undefined || isUint32Slot(command.syncSequence)) &&
    // A track-monitor command cannot infer a lane: its target is required.
    (type !== SonareEngineCommandType.SetTrackMonitorMode ||
      (isUint32Slot(command.targetId) && isTrackMonitorMode(command.argInt))) &&
    // The live engine refuses a negative position and a PPQ outside the public range.
    (type !== SonareEngineCommandType.TransportSeekSample || Number(command.argInt ?? 0) >= 0) &&
    (type !== SonareEngineCommandType.TransportSeekPpq || isPublicPpq(command.argFloat)) &&
    // A punch window carries whole samples: in in argInt, out in argFloat, 0 <= in <= out.
    (type !== SonareEngineCommandType.Punch ||
      (Number(command.argInt ?? 0) >= 0 &&
        Number.isSafeInteger(command.argFloat ?? 0) &&
        (command.argFloat ?? 0) >= Number(command.argInt ?? 0))) &&
    (type !== SonareEngineCommandType.SetLoop ||
      (command.argInt === undefined &&
        isPublicPpq(command.argFloat) &&
        isPublicPpq(command.argFloat2)))
  );
}

/** Rebuilds the error a worklet reply described, keeping a SonareError's code and class. */
function workletFailure(
  data: { failure?: unknown; error?: unknown; message?: unknown },
  fallback = 'AudioWorklet request failed',
  messageKey: 'error' | 'message' = 'error',
): Error {
  if (isFailurePayload(data.failure)) {
    return failureFromPayload(data.failure);
  }
  return codedError(ErrorCode.Unknown, String(data[messageKey] ?? fallback));
}

function workletBlockSize(maxBlockSize: number | undefined): number {
  const resolved = maxBlockSize ?? AUDIO_WORKLET_RENDER_QUANTUM;
  if (!Number.isSafeInteger(resolved) || resolved < AUDIO_WORKLET_RENDER_QUANTUM) {
    throw refuse(
      `maxBlockSize must be an integer in [${AUDIO_WORKLET_RENDER_QUANTUM}, ${Number.MAX_SAFE_INTEGER}]`,
    );
  }
  return resolved;
}

/**
 * AudioWorkletNode wrapper that hosts a realtime engine on the audio thread.
 *
 * Commands travel over a SharedArrayBuffer ring in `'sab'` mode and over the node's message port in
 * `'postMessage'` mode; reads that need the worklet's answer return a `Promise`. Create it with
 * {@link SonareRealtimeEngineNode.create}; release it with {@link destroy}. After `destroy`, command
 * methods throw `SonareError` with `ErrorCode.InvalidState`.
 */
@entryClass('SonareRealtimeEngineNode', [
  'assertOpen',
  'postOrdered',
  'shutdown',
  'emitTelemetry',
  'startRingPolling',
  'stopRingPollingIfUnused',
  'emitMeter',
  'emitMidiOut',
  'emitClipPageRequests',
  'emitScope',
  'sendCaptureRequest',
  'sendTransportRequest',
  'sendReadRequest',
])
export class SonareRealtimeEngineNode {
  /** The underlying AudioWorkletNode; connect it into the audio graph. */
  readonly node: AudioWorkletNode;
  /** Transport mode and feature support detected when the node was created. */
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  /** @internal */
  readonly commandRing?: SonareEngineCommandRingBuffer;
  /** Telemetry ring drained by {@link pollTelemetry}; present only in `'sab'` mode. */
  readonly telemetryRing?: EngineTelemetryRingBuffer;
  /** Meter ring drained by {@link pollMeters}; present only in `'sab'` mode. */
  readonly meterRing?: MeterRingBuffer;
  /** Scope ring drained by {@link pollScope}; present only in `'sab'` mode with `scopeIntervalFrames` above 0. */
  readonly scopeRing?: ScopeRingBuffer;
  /** Paged-clip miss ring drained by `pollClipPageRequests`; present only in `'sab'` mode. */
  readonly clipPageRequestRing?: ClipPageRequestRingBuffer;
  /** External-MIDI output ring drained by `pollMidiOut`; present only in `'sab'` mode. */
  readonly externalMidiRing?: ExternalMidiRingBuffer;
  /**
   * Settles when the worklet processor has reported ready (already resolved when the processor sends no
   * ready message). Rejects if the worklet reports an error, or with `InvalidState` if the node is
   * destroyed first.
   */
  readonly ready: Promise<void>;
  private telemetryReadIndex = 0;
  private meterReadIndex = 0;
  private scopeReadIndex = 0;
  private clipPageRequestDroppedRead = 0;
  private ringPollTimer: ReturnType<typeof setInterval> | undefined;
  private telemetryListeners = new Set<(telemetry: EngineTelemetry) => void>();
  private meterListeners = new Set<(meter: WorkletMeterSnapshot) => void>();
  private scopeListeners = new Set<(scope: WorkletScopeSnapshot) => void>();
  private midiOutListeners = new Set<(events: ExternalMidiEvent[]) => void>();
  private clipPageRequestListeners = new Set<(message: EngineClipPageRequestMessage) => void>();
  private syncErrorListeners = new Set<(message: EngineSyncErrorMessage) => void>();
  private errorListeners = new Set<(error: Error) => void>();
  private captureRequestId = 1;
  private readonly captureRequests = new Map<
    number,
    {
      op: SonareEngineCaptureRequestMessage['op'];
      resolve: (response: SonareEngineCaptureResponseMessageInternal) => void;
      reject: (reason?: unknown) => void;
    }
  >();
  private transportRequestId = 1;
  private readonly transportRequests = new Map<
    number,
    {
      resolve: (response: SonareEngineTransportResponseMessage) => void;
      reject: (reason?: unknown) => void;
    }
  >();
  private readRequestId = 1;
  private readonly readRequests = new Map<
    number,
    {
      resolve: (value: Articulation | number | undefined) => void;
      reject: (reason?: unknown) => void;
    }
  >();
  private insertGainReductionRequestId = 1;
  private readonly insertGainReductionRequests = new Map<
    number,
    {
      resolve: (values: number[]) => void;
      reject: (reason?: unknown) => void;
    }
  >();
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private readonly epoch = new OwnerEpoch('SonareRealtimeEngineNode');
  private destroying: Promise<void> | undefined;
  /** Sequence number of the last ordered port message (sync or request); SAB ring commands carry it. */
  private syncSequence = 0;

  private constructor(
    node: AudioWorkletNode,
    capabilities: SonareRealtimeEngineNodeCapabilities,
    commandRing?: SonareEngineCommandRingBuffer,
    telemetryRing?: EngineTelemetryRingBuffer,
    meterRing?: MeterRingBuffer,
    scopeRing?: ScopeRingBuffer,
    clipPageRequestRing?: ClipPageRequestRingBuffer,
    externalMidiRing?: ExternalMidiRingBuffer,
  ) {
    this.node = node;
    this.capabilities = capabilities;
    this.commandRing = commandRing;
    this.telemetryRing = telemetryRing;
    this.meterRing = meterRing;
    this.scopeRing = scopeRing;
    this.clipPageRequestRing = clipPageRequestRing;
    this.externalMidiRing = externalMidiRing;
    // The consumer awaits the field, so a startup failure is named by it.
    this.ready = withEntry(
      'SonareRealtimeEngineNode.ready',
      () =>
        new Promise<void>((resolve, reject) => {
          this.resolveReady = resolve;
          this.rejectReady = reject;
        }),
    );
    if (!capabilities.readyMessage) {
      this.resolveReady();
    }
    // Destroy settles a pending ready wait; rejecting an already settled
    // promise is a no-op, so a node that became ready stays resolved.
    this.ready.catch(() => undefined);
    // A processor constructor that throws surfaces only as this event.
    this.node.addEventListener?.('processorerror', () => {
      this.rejectReady(codedError(ErrorCode.Unknown, 'the processor failed to start'));
    });
    this.epoch.onClose(() =>
      this.rejectReady(
        invalidState('SonareRealtimeEngineNode was destroyed before it became ready'),
      ),
    );
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      // A host that kept its own reference to this handler (or a port
      // implementation that ignores the detach in destroy()) must not be able
      // to reach listeners of a node the host already disposed.
      if (this.epoch.closed) {
        return;
      }
      const captureRequestId = engineCaptureResponseRequestId(event.data);
      if (captureRequestId !== undefined) {
        const pending = this.captureRequests.get(captureRequestId);
        if (pending) {
          this.captureRequests.delete(captureRequestId);
          if (!isEngineCaptureResponseMessage(event.data)) {
            pending.reject(codedError(ErrorCode.InvalidFormat, 'Malformed capture response.'));
          } else if (!isEngineCaptureResponseForOperation(event.data, pending.op)) {
            pending.reject(
              codedError(
                ErrorCode.InvalidFormat,
                'Capture response does not match request operation.',
              ),
            );
          } else if (event.data.ok) {
            pending.resolve(event.data);
          } else {
            pending.reject(workletFailure(event.data));
          }
        }
      } else if (isEngineTransportResponseMessage(event.data)) {
        const pending = this.transportRequests.get(event.data.requestId);
        if (pending) {
          this.transportRequests.delete(event.data.requestId);
          if (event.data.ok) {
            pending.resolve(event.data);
          } else {
            pending.reject(workletFailure(event.data, 'Transport request failed'));
          }
        }
      } else if (isEngineReadResponseMessage(event.data)) {
        const pending = this.readRequests.get(event.data.requestId);
        if (pending) {
          this.readRequests.delete(event.data.requestId);
          if (event.data.ok) {
            pending.resolve(event.data.value);
          } else {
            pending.reject(workletFailure(event.data, 'Engine read request failed'));
          }
        }
      } else if (isEngineInsertGainReductionResponseMessage(event.data)) {
        const pending = this.insertGainReductionRequests.get(event.data.requestId);
        if (pending) {
          this.insertGainReductionRequests.delete(event.data.requestId);
          if (event.data.ok && Array.isArray(event.data.values)) {
            pending.resolve(event.data.values);
          } else {
            pending.reject(workletFailure(event.data, 'Insert gain reduction request failed'));
          }
        }
      } else if (isEngineTelemetryRecord(event.data)) {
        this.emitTelemetry(telemetryFromRecord(event.data));
      } else if (isMeterSnapshot(event.data)) {
        this.emitMeter(event.data);
      } else if (isScopeSnapshot(event.data)) {
        this.emitScope(event.data);
      } else if (isExternalMidiBatchMessage(event.data)) {
        this.emitMidiOut(event.data.events.map(externalMidiEventFromWire));
      } else if (isClipPageRequestMessage(event.data)) {
        this.emitClipPageRequests(event.data);
      } else if (isRecord(event.data) && event.data.type === 'syncError') {
        const entry = typeof event.data.entry === 'string' ? event.data.entry : undefined;
        const raw = String(event.data.message ?? 'AudioWorklet sync failed');
        const failure = isFailurePayload(event.data.failure)
          ? failureFromPayload(event.data.failure)
          : undefined;
        // The audio thread's text may carry its own inner entry; the failure keeps the bare reason.
        let message = raw;
        if (entry !== undefined && failure !== undefined) {
          nameNativeError(failure, entry);
          message = failure.message;
        } else if (entry !== undefined) {
          message = `${entry}: ${raw}`;
        }
        const syncError: EngineSyncErrorMessage = {
          type: 'syncError',
          syncType: String(event.data.syncType),
          message,
          ...(failure === undefined ? {} : { failure }),
        };
        for (const listener of this.syncErrorListeners) {
          listener(syncError);
        }
      } else if (isRecord(event.data) && event.data.type === 'ready') {
        this.resolveReady();
      } else if (isRecord(event.data) && event.data.type === 'error') {
        const error = workletFailure(event.data, 'AudioWorklet error', 'message');
        this.rejectReady(error);
        for (const listener of this.errorListeners) {
          listener(error);
        }
      }
    };
  }

  /** Subscribes to control-plane sync rejections reported by the worklet. */
  onSyncError(listener: (message: EngineSyncErrorMessage) => void): () => void {
    this.syncErrorListeners.add(listener);
    return () => this.syncErrorListeners.delete(listener);
  }

  /**
   * Subscribes to failures the processor reports: a start it could not complete, a port message it
   * refused outside the sync protocol, or a render quantum that threw, after which it renders
   * silence and stops. A refused value arrives as `TypeError` / `RangeError`, anything else as a
   * coded `SonareError`. Returns the unsubscribe function.
   */
  onError(listener: (error: Error) => void): () => void {
    this.errorListeners.add(listener);
    return () => this.errorListeners.delete(listener);
  }

  /**
   * Registers the processor module (when `options.moduleUrl` is given) and creates the node.
   *
   * The node accepts commands immediately but the processor may still be starting; await
   * {@link ready} before relying on a reply.
   *
   * @param context Audio context that owns the node.
   * @param options Transport mode, block size, ring capacities and processor settings.
   * @throws TypeError for an unknown option key or a wrong-typed option.
   * @throws RangeError for an out-of-range option (`maxBlockSize` below 128 frames, `commandRingCapacity`
   * above 65536, a non-integer capacity or interval).
   * @throws SonareError `AbiMismatch` when the module's engine ABI differs from the expected one (unless
   * `requireAbiCompatible` is `false`), and `NotSupported` when `mode: 'sab'` is requested without
   * SharedArrayBuffer and Atomics.
   */
  static async create(
    context: BaseAudioContext,
    options: SonareRealtimeEngineNodeOptions,
  ): Promise<SonareRealtimeEngineNode> {
    assertKnownProcessorOptions('engineNode', options);
    const maxBlockSize = workletBlockSize(options.maxBlockSize);
    const sampleRate = resolveContextSampleRate(options.sampleRate, context);
    const processorName = options.processorName ?? 'sonare-realtime-engine-processor';
    const moduleUrl = options.moduleUrl;
    if (moduleUrl && context.audioWorklet?.addModule) {
      await context.audioWorklet.addModule(moduleUrl);
    }
    const detectedCapabilities =
      options.engineAbiVersion !== undefined
        ? {
            engineAbiVersion: options.engineAbiVersion,
            expectedEngineAbiVersion: options.expectedEngineAbiVersion ?? options.engineAbiVersion,
            abiCompatible:
              options.engineAbiVersion ===
              (options.expectedEngineAbiVersion ?? options.engineAbiVersion),
          }
        : engineCapabilities();
    if (options.requireAbiCompatible !== false && detectedCapabilities?.abiCompatible === false) {
      throw codedError(
        ErrorCode.AbiMismatch,
        `Engine ABI mismatch: wasm=${detectedCapabilities.engineAbiVersion}, expected=${detectedCapabilities.expectedEngineAbiVersion}`,
      );
    }
    const sharedArrayBuffer = typeof globalThis.SharedArrayBuffer === 'function';
    const atomics = typeof globalThis.Atomics === 'object';
    const audioWorklet = typeof AudioWorkletNode !== 'undefined' || !!options.nodeFactory;
    const degradedReason =
      options.mode !== 'postMessage' && (!sharedArrayBuffer || !atomics)
        ? 'SharedArrayBuffer or Atomics unavailable; using postMessage transport.'
        : undefined;
    const mode =
      options.mode === 'postMessage' || !sharedArrayBuffer || !atomics ? 'postMessage' : 'sab';
    if (options.mode === 'sab' && mode !== 'sab') {
      throw codedError(
        ErrorCode.NotSupported,
        'SharedArrayBuffer mode requested but SharedArrayBuffer/Atomics are unavailable.',
      );
    }

    const meterIntervalFrames = requireIntegerOption(
      options.meterIntervalFrames,
      2048,
      'meterIntervalFrames',
      0,
    );

    // Resolved before the transport branch, so a capacity the ring could not
    // hold is refused by name whichever transport the caller ends up with.
    const commandRingCapacity = requireIntegerOption(
      options.commandRingCapacity,
      128,
      'commandRingCapacity',
      1,
    );
    if (commandRingCapacity > 65536) {
      throw refuse('commandRingCapacity must be at most 65536');
    }
    const telemetryRingCapacity = requireIntegerOption(
      options.telemetryRingCapacity,
      128,
      'telemetryRingCapacity',
      1,
    );
    const meterRingCapacity = requireIntegerOption(
      options.meterRingCapacity,
      128,
      'meterRingCapacity',
      1,
    );
    const scopeRingCapacity = requireIntegerOption(
      options.scopeRingCapacity,
      64,
      'scopeRingCapacity',
      1,
    );
    const scopeBands = resolveScopeBandCount(options.scopeBands);
    const clipPageRequestRingCapacity = requireIntegerOption(
      options.clipPageRequestRingCapacity,
      128,
      'clipPageRequestRingCapacity',
      1,
    );
    const externalMidiRingCapacity = requireIntegerOption(
      options.externalMidiRingCapacity,
      256,
      'externalMidiRingCapacity',
      1,
    );
    const commandRing =
      mode === 'sab'
        ? createSonareEngineCommandRingBuffer({ capacity: commandRingCapacity })
        : undefined;
    const telemetryRing =
      mode === 'sab'
        ? createEngineTelemetryRingBuffer({ capacity: telemetryRingCapacity })
        : undefined;
    // Meter ring: the engine publishes meters into a SAB ring. Lock-free meter
    // delivery matches the telemetry path and keeps the audio render callback
    // allocation-free in SAB mode.
    const meterRing =
      mode === 'sab' ? createMeterRingBuffer({ capacity: meterRingCapacity }) : undefined;
    // Scope ring (FFT spectrum + goniometer): opt-in. The per-block FFT is
    // heavier than the meter path, so it is created only when the caller
    // requests scope telemetry via scopeIntervalFrames > 0. Zero is the off
    // switch, so the interval's floor is 0 rather than the capacities' 1.
    const scopeIntervalFrames = requireIntegerOption(
      options.scopeIntervalFrames,
      0,
      'scopeIntervalFrames',
      0,
    );
    const scopeRing =
      mode === 'sab' && scopeIntervalFrames > 0
        ? createScopeRingBuffer({ capacity: scopeRingCapacity, bands: scopeBands })
        : undefined;
    // Paged-clip cache misses are a worklet-to-main-thread control signal. A
    // dedicated SPSC SAB ring keeps process() free of both embind request
    // objects and postMessage structured cloning.
    const clipPageRequestRing =
      mode === 'sab'
        ? createClipPageRequestRingBuffer({ capacity: clipPageRequestRingCapacity })
        : undefined;
    const externalMidiRing =
      mode === 'sab'
        ? createExternalMidiRingBuffer({ capacity: externalMidiRingCapacity })
        : undefined;
    const channelCount = requireChannelCount(options.numChannels, 'numChannels');
    const cueOutput = options.cueOutput === true;
    const processorOptions: SonareRealtimeEngineWorkletProcessorOptions = {
      sampleRate,
      maxBlockSize,
      numChannels: channelCount,
      meterIntervalFrames,
      cueOutput,
      commandSharedBuffer: commandRing?.sharedBuffer,
      commandRingCapacity: commandRing?.capacity,
      telemetrySharedBuffer: telemetryRing?.sharedBuffer,
      telemetryRingCapacity: telemetryRing?.capacity,
      meterSharedBuffer: meterRing?.sharedBuffer,
      meterRingCapacity: meterRing?.capacity,
      scopeSharedBuffer: scopeRing?.sharedBuffer,
      scopeRingCapacity: scopeRing?.capacity,
      scopeBands: scopeIntervalFrames > 0 ? (scopeRing?.bands ?? scopeBands) : undefined,
      scopeIntervalFrames: scopeIntervalFrames > 0 ? scopeIntervalFrames : undefined,
      clipPageRequestSharedBuffer: clipPageRequestRing?.sharedBuffer,
      clipPageRequestRingCapacity: clipPageRequestRing?.capacity,
      externalMidiSharedBuffer: externalMidiRing?.sharedBuffer,
      externalMidiRingCapacity: externalMidiRing?.capacity,
      wasmBinary: options.wasmBinary,
      initialSyncMessages: options.initialSyncMessages,
      initialCommands: options.initialCommands,
    };
    const factory =
      options.nodeFactory ??
      ((ctx: BaseAudioContext, name: string, nodeOptions: AudioWorkletNodeOptions) =>
        new AudioWorkletNode(ctx, name, nodeOptions));
    const node = factory(context, processorName, {
      numberOfInputs: 1,
      // The cue bus needs its own output; a single-output node keeps the
      // historical mix where process() folds the cue into the program.
      numberOfOutputs: cueOutput ? 2 : 1,
      outputChannelCount: cueOutput ? [channelCount, channelCount] : [channelCount],
      processorOptions,
    });
    return new SonareRealtimeEngineNode(
      node,
      {
        mode,
        runtimeTarget: 'embind',
        sharedArrayBuffer,
        atomics,
        audioWorklet,
        clipPageRequestsRealtimeSafe: mode === 'sab',
        externalMidiRealtimeSafe: mode === 'sab',
        cueOutput,
        engineAbiVersion: detectedCapabilities?.engineAbiVersion,
        expectedEngineAbiVersion: detectedCapabilities?.expectedEngineAbiVersion,
        abiCompatible: detectedCapabilities?.abiCompatible,
        degradedReason,
        // A processor posts ready/error irrespective of whether its module was
        // loaded here or registered by the host beforehand. Waiting in both
        // cases is the only way to surface a failed worklet-side WASM init.
        readyMessage: true,
      },
      commandRing,
      telemetryRing,
      meterRing,
      scopeRing,
      clipPageRequestRing,
      externalMidiRing,
    );
  }

  /**
   * Queues transport start. `renderFrame` is the absolute render frame to apply at; omitted: applied immediately.
   *
   * @returns `false` when the command ring is full; `true` once queued.
   * @throws RangeError for a negative `renderFrame`.
   * @throws SonareError `InvalidState` once the node is destroyed.
   */
  play(renderFrame?: number): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportPlay,
      renderFrame: resolveRenderFrame(renderFrame),
    });
  }

  /** Queues a reset of the master's integrated-loudness accumulator. */
  resetMasterLoudnessMeter(renderFrame?: number): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.ResetMasterLoudnessMeter,
      renderFrame: resolveRenderFrame(renderFrame),
    });
  }

  /** Queues a reset of every mixer and effect processor to its prepared state. */
  resetProcessorState(renderFrame?: number): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.ResetProcessorState,
      renderFrame: resolveRenderFrame(renderFrame),
    });
  }

  /**
   * Queues transport stop. `renderFrame` is the absolute render frame to apply at; omitted: applied immediately.
   *
   * @returns `false` when the command ring is full; `true` once queued.
   * @throws RangeError for a negative `renderFrame`.
   * @throws SonareError `InvalidState` once the node is destroyed.
   */
  stop(renderFrame?: number): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportStop,
      renderFrame: resolveRenderFrame(renderFrame),
    });
  }

  /**
   * Queues a transport seek to `timelineSample`, a position in timeline samples. `renderFrame` is the absolute render frame to apply at; omitted: applied immediately.
   *
   * @returns `false` when the command ring is full or the position is not a valid sample count.
   * @throws RangeError for a negative `renderFrame`.
   * @throws SonareError `InvalidState` once the node is destroyed.
   */
  seekSample(timelineSample: number, renderFrame?: number): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekSample,
      renderFrame: resolveRenderFrame(renderFrame),
      argInt: timelineSample,
    });
  }

  /**
   * Queues a transport seek to `ppq`, a position in quarter notes, in [0, 1e12]. `renderFrame` is the absolute render frame to apply at; omitted: applied immediately.
   *
   * @returns `false` when the command ring is full or `ppq` is not a valid position.
   * @throws RangeError for a negative `renderFrame`.
   * @throws SonareError `InvalidState` once the node is destroyed.
   */
  seekPpq(ppq: number, renderFrame?: number): boolean {
    const frame = resolveRenderFrame(renderFrame);
    if (!isPublicPpq(ppq) || !Number.isSafeInteger(frame)) {
      this.assertOpen();
      return false;
    }
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekPpq,
      renderFrame: frame,
      argFloat: ppq,
    });
  }

  /**
   * Whether the node would queue `command` now: a well-formed record and room
   * for it. Queues nothing.
   * @throws SonareError `InvalidState` once the node is destroyed.
   *
   * @internal
   */
  canSendCommand(command: SonareEngineCommandRecord): boolean {
    this.assertOpen();
    return (
      isValidCommandRecord(command) &&
      (!this.commandRing || sonareEngineCommandRingHasRoom(this.commandRing))
    );
  }

  /**
   * Queues `command` live; false when the record is malformed or the ring is full.
   * @throws SonareError `InvalidState` once the node is destroyed.
   *
   * @internal
   */
  sendCommand(command: SonareEngineCommandRecord): boolean {
    this.assertOpen();
    if (!isValidCommandRecord(command)) {
      return false;
    }
    if (this.commandRing) {
      // The ring and the port are separate channels, so the record names the last port message it follows.
      return pushSonareEngineCommandRingBuffer(this.commandRing, {
        ...command,
        syncSequence: this.syncSequence,
      });
    }
    this.node.port.postMessage(command);
    return true;
  }

  /**
   * Posts a control-plane sync to the worklet, in call order with the commands around it.
   * @throws SonareError `InvalidState` once the node is destroyed.
   *
   * @internal
   */
  postSync(message: SonareEngineSyncMessage, transfer?: Transferable[]): void {
    this.assertOpen();
    // The entry that posted the sync names a refusal the worklet reports back for it.
    const entry = activeEntry();
    this.postOrdered({ ...message, ...(entry === undefined ? {} : { entry }) }, transfer);
  }

  /**
   * Posts a port message in call order with the commands around it. The worklet applies every
   * command sent before it first; on the SAB ring a command sent after it is held until it has
   * been handled, and over the port message order already guarantees both.
   */
  private postOrdered(message: object, transfer?: Transferable[]): void {
    const syncSequence = (this.syncSequence + 1) >>> 0;
    const posted = this.commandRing ? { ...message, syncSequence } : message;
    if (transfer && transfer.length > 0) {
      this.node.port.postMessage(posted, transfer);
    } else {
      this.node.port.postMessage(posted);
    }
    this.syncSequence = syncSequence;
  }

  private assertOpen(): void {
    assertHandleLive(this.epoch.closed, 'SonareRealtimeEngineNode');
  }

  /**
   * Asks the worklet for the capture state; the answer arrives asynchronously.
   *
   * @throws SonareError `InvalidState` (as a rejection) once the node is destroyed.
   */
  requestCaptureStatus(): Promise<EngineCaptureStatus> {
    return this.sendCaptureRequest('status').then((response) => {
      if (!response.ok || !('status' in response)) {
        throw codedError(ErrorCode.InvalidFormat, 'Capture status response is missing status.');
      }
      return response.status;
    });
  }

  /**
   * Asks the worklet for the audio captured so far; resolves with one `Float32Array` per channel.
   *
   * @throws SonareError `InvalidState` (as a rejection) once the node is destroyed.
   */
  requestCapturedAudio(): Promise<Float32Array[]> {
    return this.sendCaptureRequest('read').then((response) => {
      if (!response.ok || !('channels' in response)) {
        throw codedError(ErrorCode.InvalidFormat, 'Capture read response is missing channels.');
      }
      return response.channels;
    });
  }

  /**
   * Asks the worklet to discard the captured audio; resolves once it has done so.
   *
   * @throws SonareError `InvalidState` (as a rejection) once the node is destroyed.
   */
  requestCaptureReset(): Promise<void> {
    return this.sendCaptureRequest('reset').then(() => undefined);
  }

  /**
   * Reads the transport from the worklet's live engine; the answer arrives asynchronously.
   *
   * @throws SonareError `InvalidState` (as a rejection) once the node is destroyed.
   */
  requestTransportState(): Promise<EngineTransportState> {
    return this.sendTransportRequest().then((response) => {
      if (!response.state) {
        throw codedError(ErrorCode.InvalidFormat, 'Transport state response is missing state.');
      }
      return response.state;
    });
  }

  /**
   * Drains telemetry records published since the last poll and forwards each to {@link onTelemetry}
   * listeners. Returns an empty array outside `'sab'` mode, where telemetry arrives through the port.
   */
  pollTelemetry(): EngineTelemetry[] {
    if (!this.telemetryRing) {
      return [];
    }
    const read = readEngineTelemetryRingBuffer(this.telemetryRing, this.telemetryReadIndex);
    this.telemetryReadIndex = read.nextReadIndex;
    const records = read.telemetry.map(telemetryFromRecord);
    for (const telemetry of records) {
      this.emitTelemetry(telemetry);
    }
    return records;
  }

  // Drains any meters published into the SAB meter ring (embind SAB mode) and
  // forwards them to onMeter listeners. In postMessage mode meters arrive via
  // node.port.onmessage instead, so this is a no-op then.
  /**
   * Drains meter snapshots published into the meter ring since the last poll and forwards each to
   * {@link onMeter} listeners. Returns an empty array outside `'sab'` mode, where meters arrive through
   * the port.
   */
  pollMeters(): WorkletMeterSnapshot[] {
    if (!this.meterRing) {
      return [];
    }
    const read = readMeterRingBuffer(this.meterRing, this.meterReadIndex);
    this.meterReadIndex = read.nextReadIndex;
    for (const meter of read.meters) {
      this.emitMeter(meter);
    }
    return read.meters;
  }

  // Drains scope telemetry (FFT spectrum + goniometer points) published into the
  // SAB scope ring and forwards each record to onScope listeners. A no-op unless
  // the node was created with scopeIntervalFrames > 0 (embind SAB mode).
  /**
   * Drains scope snapshots (spectrum and goniometer points) published since the last poll and forwards
   * each to {@link onScope} listeners. Returns an empty array unless the node was created in `'sab'`
   * mode with `scopeIntervalFrames` above 0.
   */
  pollScope(): WorkletScopeSnapshot[] {
    if (!this.scopeRing) {
      return [];
    }
    const read = readScopeRingBuffer(this.scopeRing, this.scopeReadIndex);
    this.scopeReadIndex = read.nextReadIndex;
    for (const scope of read.scopes) {
      this.emitScope(scope);
    }
    return read.scopes;
  }

  /** Drain lowered MIDI-1 records from the SAB ring on the main thread. */
  pollMidiOut(): ExternalMidiEvent[] {
    if (!this.externalMidiRing) {
      return [];
    }
    const read = readExternalMidiRingBuffer(this.externalMidiRing);
    const events = read.events.map((event) => {
      const bytes = new Uint8Array(event.byteCount);
      for (let index = 0; index < event.byteCount; index++) {
        bytes[index] = (event.byteWord >>> (8 * index)) & 0xff;
      }
      return externalMidiEvent(event.destinationId, event.renderFrame, bytes);
    });
    if (events.length > 0) {
      this.emitMidiOut(events);
    }
    return events;
  }

  /**
   * Drains bounded paged-clip misses from the SAB ring and forwards one batch
   * to subscribers. In postMessage mode the legacy handler remains available,
   * but that degraded path is not realtime-safe for OPFS streaming.
   */
  pollClipPageRequests(): EngineClipPageRequestMessage | undefined {
    if (!this.clipPageRequestRing) {
      return undefined;
    }
    const read = readClipPageRequestRingBuffer(this.clipPageRequestRing);
    const dropped = (read.dropped - this.clipPageRequestDroppedRead) >>> 0;
    this.clipPageRequestDroppedRead = read.dropped;
    if (read.requests.length === 0 && dropped === 0) {
      return undefined;
    }
    const message: EngineClipPageRequestMessage = {
      type: 'clipPageRequest',
      requests: read.requests,
      ...(dropped > 0 ? { dropped } : {}),
    };
    this.emitClipPageRequests(message);
    return message;
  }

  /**
   * Subscribes to telemetry records, polling the ring while any listener is attached.
   *
   * @returns A function that removes the subscription.
   */
  onTelemetry(callback: (telemetry: EngineTelemetry) => void): () => void {
    this.telemetryListeners.add(callback);
    this.startRingPolling();
    return () => {
      this.telemetryListeners.delete(callback);
      this.stopRingPollingIfUnused();
    };
  }

  /**
   * Subscribes to meter snapshots, from the meter ring in `'sab'` mode or the port otherwise.
   *
   * @returns A function that removes the subscription.
   */
  onMeter(callback: (meter: WorkletMeterSnapshot) => void): () => void {
    this.meterListeners.add(callback);
    this.startRingPolling();
    return () => {
      this.meterListeners.delete(callback);
      this.stopRingPollingIfUnused();
    };
  }

  /**
   * Subscribes to scope snapshots, polling the scope ring while any listener is attached.
   *
   * @returns A function that removes the subscription.
   */
  onScope(callback: (scope: WorkletScopeSnapshot) => void): () => void {
    this.scopeListeners.add(callback);
    this.startRingPolling();
    return () => {
      this.scopeListeners.delete(callback);
      this.stopRingPollingIfUnused();
    };
  }

  /**
   * Subscribe to batches of external-MIDI events drained from the engine,
   * already lowered to MIDI 1.0 bytes for a Web MIDI output port. Each event
   * carries its absolute `renderFrame`; a batch is not a render block. With
   * postMessage transport a batch holds one block's events; with the SAB ring
   * it holds whatever accumulated since the previous poll (every 16 ms), which
   * may span many blocks. While subscribed, the poll consumes the ring, so a
   * manual {@link pollMidiOut} sees only what arrived since. Returns an
   * unsubscribe function.
   */
  onMidiOut(callback: (events: ExternalMidiEvent[]) => void): () => void {
    this.midiOutListeners.add(callback);
    this.startRingPolling();
    return () => {
      this.midiOutListeners.delete(callback);
      this.stopRingPollingIfUnused();
    };
  }

  /**
   * Subscribe to bounded batches of paged-clip misses from the worklet. The
   * callback runs on the main thread and may initiate asynchronous OPFS I/O.
   */
  onClipPageRequests(callback: (message: EngineClipPageRequestMessage) => void): () => void {
    this.clipPageRequestListeners.add(callback);
    return () => {
      this.clipPageRequestListeners.delete(callback);
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
    if (this.epoch.closed) {
      return;
    }
    this.epoch.close();
    if (this.ringPollTimer !== undefined) {
      clearInterval(this.ringPollTimer);
      this.ringPollTimer = undefined;
    }
    try {
      this.node.port.postMessage({ type: 'destroy' });
    } catch {
      // The worklet notification is best effort. Local ownership still needs
      // to be released when the port has already closed or rejects dispatch.
    }
    this.node.disconnect();
    for (const pending of this.captureRequests.values()) {
      pending.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    this.captureRequests.clear();
    for (const pending of this.transportRequests.values()) {
      pending.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    this.transportRequests.clear();
    for (const pending of this.insertGainReductionRequests.values()) {
      pending.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    this.insertGainReductionRequests.clear();
    for (const pending of this.readRequests.values()) {
      pending.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    this.readRequests.clear();
    this.telemetryListeners.clear();
    this.meterListeners.clear();
    this.scopeListeners.clear();
    this.midiOutListeners.clear();
    this.clipPageRequestListeners.clear();
    this.syncErrorListeners.clear();
    this.errorListeners.clear();
    // The port outlives destroy(): the worklet tears itself down asynchronously
    // after the 'destroy' message above, so a syncError already in flight would
    // still be dispatched here. Detaching the handler is what makes the
    // listener sets stay empty rather than being refilled by a late message.
    this.node.port.onmessage = null;
  }

  private emitTelemetry(telemetry: EngineTelemetry): void {
    for (const listener of this.telemetryListeners) {
      listener(telemetry);
    }
  }

  private startRingPolling(): void {
    if (
      this.ringPollTimer !== undefined ||
      (!this.telemetryRing && !this.meterRing && !this.scopeRing && !this.externalMidiRing)
    ) {
      return;
    }
    const poll = () => {
      if (!this.epoch.closed) {
        this.pollTelemetry();
        this.pollMeters();
        this.pollScope();
        // The MIDI ring is a queue, not a telemetry snapshot: drain it only for
        // an onMidiOut subscriber so a manual pollMidiOut still gets every event.
        if (this.midiOutListeners.size > 0) {
          this.pollMidiOut();
        }
      }
    };
    poll();
    this.ringPollTimer = setInterval(poll, 16);
  }

  private stopRingPollingIfUnused(): void {
    if (
      this.ringPollTimer !== undefined &&
      this.telemetryListeners.size === 0 &&
      this.meterListeners.size === 0 &&
      this.scopeListeners.size === 0 &&
      this.midiOutListeners.size === 0
    ) {
      clearInterval(this.ringPollTimer);
      this.ringPollTimer = undefined;
    }
  }

  private emitMeter(meter: WorkletMeterSnapshot): void {
    for (const listener of this.meterListeners) {
      listener(meter);
    }
  }

  private emitMidiOut(events: ExternalMidiEvent[]): void {
    for (const listener of this.midiOutListeners) {
      listener(events);
    }
  }

  private emitClipPageRequests(message: EngineClipPageRequestMessage): void {
    for (const listener of this.clipPageRequestListeners) {
      listener(message);
    }
  }

  /**
   * Reads one target's per-insert gain reduction (dB, each <= 0, pre-to-post insert order)
   * from the worklet. Works in both ring and postMessage meter modes; rejects when the
   * target is invalid or mixing is not compiled in.
   */
  pollInsertGainReduction(targetId: number): Promise<number[]> {
    if (this.epoch.closed) {
      return Promise.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    const requestId = this.insertGainReductionRequestId++;
    let rejectRequest!: (reason?: unknown) => void;
    const promise = new Promise<number[]>((resolve, reject) => {
      rejectRequest = reject;
      this.insertGainReductionRequests.set(requestId, { resolve, reject });
    });
    try {
      this.postOrdered({ type: 'insertGainReductionRequest', requestId, targetId });
    } catch (error) {
      this.insertGainReductionRequests.delete(requestId);
      rejectRequest(error);
    }
    return promise;
  }

  /**
   * Reads one channel's articulation from the instrument the worklet's live engine has bound to
   * `destinationId`. Binding an instrument replaces the articulation with that instrument's own,
   * so this answers what the engine holds rather than what was last sent.
   *
   * @throws SonareError (as a rejection) on the same terms as `RealtimeEngine.articulation`, and
   *   `InvalidState` once the node is destroyed.
   */
  requestArticulation(destinationId: number, channel: number): Promise<Articulation> {
    return this.sendReadRequest('articulation', destinationId, channel).then((value) => {
      if (typeof value !== 'string') {
        throw codedError(ErrorCode.InvalidFormat, 'Articulation response is missing its value.');
      }
      return value;
    });
  }

  /**
   * Reads the legato fallback count of the instrument the worklet's live engine has bound to
   * `destinationId`. The count belongs to the instrument, so binding another one starts it from zero.
   *
   * @throws SonareError (as a rejection) on the same terms as `RealtimeEngine.legatoFallbackCount`,
   *   and `InvalidState` once the node is destroyed.
   */
  requestLegatoFallbackCount(destinationId: number): Promise<number> {
    return this.sendReadRequest('legatoFallbackCount', destinationId, 0).then((value) => {
      if (typeof value !== 'number') {
        throw codedError(
          ErrorCode.InvalidFormat,
          'Legato fallback count response is missing its value.',
        );
      }
      return value;
    });
  }

  /**
   * Reads how many timed MIDI events, live input and track-addressed, reached the worklet after the render frame
   * their context time named had been rendered. Cumulative since the processor was created.
   *
   * @throws SonareError (as a rejection) with `InvalidState` once the node is destroyed.
   */
  requestLateMidiEventCount(): Promise<number> {
    return this.sendReadRequest('lateMidiEventCount', 0, 0).then((value) => {
      if (typeof value !== 'number') {
        throw codedError(
          ErrorCode.InvalidFormat,
          'Late MIDI event count response is missing its value.',
        );
      }
      return value;
    });
  }

  private sendReadRequest(
    op: SonareEngineReadRequestMessage['op'],
    destinationId: number,
    channel: number,
  ): Promise<Articulation | number | undefined> {
    if (this.epoch.closed) {
      return Promise.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    const requestId = this.readRequestId++;
    let rejectRequest!: (reason?: unknown) => void;
    const promise = new Promise<Articulation | number | undefined>((resolve, reject) => {
      rejectRequest = reject;
      this.readRequests.set(requestId, { resolve, reject });
    });
    try {
      this.postOrdered({
        type: 'engineReadRequest',
        requestId,
        op,
        destinationId,
        channel,
      } satisfies SonareEngineReadRequestMessage);
    } catch (error) {
      this.readRequests.delete(requestId);
      rejectRequest(error);
    }
    return promise;
  }

  private emitScope(scope: WorkletScopeSnapshot): void {
    for (const listener of this.scopeListeners) {
      listener(scope);
    }
  }

  private sendCaptureRequest(
    op: SonareEngineCaptureRequestMessage['op'],
  ): Promise<SonareEngineCaptureResponseMessageInternal> {
    if (this.epoch.closed) {
      return Promise.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    const requestId = this.captureRequestId++;
    let rejectRequest!: (reason?: unknown) => void;
    const promise = new Promise<SonareEngineCaptureResponseMessageInternal>((resolve, reject) => {
      rejectRequest = reject;
      this.captureRequests.set(requestId, { op, resolve, reject });
    });
    try {
      this.postOrdered({ type: 'captureRequest', requestId, op });
    } catch (error) {
      this.captureRequests.delete(requestId);
      rejectRequest(error);
    }
    return promise;
  }

  private sendTransportRequest(): Promise<SonareEngineTransportResponseMessage> {
    if (this.epoch.closed) {
      return Promise.reject(releasedHandleError('SonareRealtimeEngineNode'));
    }
    const requestId = this.transportRequestId++;
    const promise = new Promise<SonareEngineTransportResponseMessage>((resolve, reject) => {
      this.transportRequests.set(requestId, { resolve, reject });
    });
    try {
      this.postOrdered({ type: 'transportRequest', requestId, op: 'state' });
    } catch (error) {
      // Keep a reentrant response as the winner if the port both delivers it
      // and reports a late dispatch failure. A failed initial send is exposed
      // through the returned Promise and is never retried.
      const pending = this.transportRequests.get(requestId);
      if (pending) {
        this.transportRequests.delete(requestId);
        pending.reject(error);
      }
    }
    return promise;
  }
}
