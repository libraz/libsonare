import type { EngineCaptureStatus, EngineTransportState } from '../index';
import { engineCapabilities } from '../index';
import {
  isEngineCaptureResponseMessage,
  isEngineTelemetryRecord,
  isEngineTransportResponseMessage,
  isExternalMidiBatchMessage,
  isMeterSnapshot,
} from './guards';
import type {
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessage,
  SonareEngineTransportResponseMessage,
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareWorkletExternalMidiEvent,
} from './messages';
import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  isRecord,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  type SonareEngineTelemetryRecord,
  type SonareEngineTelemetryRingBuffer,
  type SonareMeterRingBuffer,
  type SonareScopeRingBuffer,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletScopeSnapshot,
} from './protocol';

export class SonareRealtimeEngineNode {
  readonly node: AudioWorkletNode;
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  readonly commandRing?: SonareEngineCommandRingBuffer;
  readonly telemetryRing?: SonareEngineTelemetryRingBuffer;
  readonly meterRing?: SonareMeterRingBuffer;
  readonly scopeRing?: SonareScopeRingBuffer;
  readonly ready: Promise<void>;
  private telemetryReadIndex = 0;
  private meterReadIndex = 0;
  private scopeReadIndex = 0;
  private telemetryListeners = new Set<(telemetry: SonareEngineTelemetryRecord) => void>();
  private meterListeners = new Set<(meter: SonareWorkletMeterSnapshot) => void>();
  private scopeListeners = new Set<(scope: SonareWorkletScopeSnapshot) => void>();
  private midiOutListeners = new Set<(events: SonareWorkletExternalMidiEvent[]) => void>();
  private captureRequestId = 1;
  private readonly captureRequests = new Map<
    number,
    {
      resolve: (response: SonareEngineCaptureResponseMessage) => void;
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
  private resolveReady!: () => void;
  private rejectReady!: (reason?: unknown) => void;
  private destroyed = false;

  private constructor(
    node: AudioWorkletNode,
    capabilities: SonareRealtimeEngineNodeCapabilities,
    commandRing?: SonareEngineCommandRingBuffer,
    telemetryRing?: SonareEngineTelemetryRingBuffer,
    meterRing?: SonareMeterRingBuffer,
    scopeRing?: SonareScopeRingBuffer,
  ) {
    this.node = node;
    this.capabilities = capabilities;
    this.commandRing = commandRing;
    this.telemetryRing = telemetryRing;
    this.meterRing = meterRing;
    this.scopeRing = scopeRing;
    this.ready = new Promise((resolve, reject) => {
      this.resolveReady = resolve;
      this.rejectReady = reject;
    });
    if (!capabilities.readyMessage) {
      this.resolveReady();
    }
    this.node.port.onmessage = (event: MessageEvent<unknown>) => {
      if (isEngineCaptureResponseMessage(event.data)) {
        const pending = this.captureRequests.get(event.data.requestId);
        if (pending) {
          this.captureRequests.delete(event.data.requestId);
          if (event.data.ok) {
            pending.resolve(event.data);
          } else {
            pending.reject(new Error(event.data.error ?? 'Capture request failed'));
          }
        }
      } else if (isEngineTransportResponseMessage(event.data)) {
        const pending = this.transportRequests.get(event.data.requestId);
        if (pending) {
          this.transportRequests.delete(event.data.requestId);
          if (event.data.ok) {
            pending.resolve(event.data);
          } else {
            pending.reject(new Error(event.data.error ?? 'Transport request failed'));
          }
        }
      } else if (isEngineTelemetryRecord(event.data)) {
        this.emitTelemetry(event.data);
      } else if (isMeterSnapshot(event.data)) {
        this.emitMeter(event.data);
      } else if (isExternalMidiBatchMessage(event.data)) {
        this.emitMidiOut(event.data.events);
      } else if (isRecord(event.data) && event.data.type === 'ready') {
        this.resolveReady();
      } else if (isRecord(event.data) && event.data.type === 'error') {
        this.rejectReady(new Error(String(event.data.message ?? 'AudioWorklet error')));
      }
    };
  }

  static async create(
    context: BaseAudioContext,
    options: SonareRealtimeEngineNodeOptions = {},
  ): Promise<SonareRealtimeEngineNode> {
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
      throw new Error(
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
      throw new Error(
        'SharedArrayBuffer mode requested but SharedArrayBuffer/Atomics are unavailable.',
      );
    }

    const commandRing =
      mode === 'sab'
        ? createSonareEngineCommandRingBuffer(options.commandRingCapacity ?? 128)
        : undefined;
    const telemetryRing =
      mode === 'sab'
        ? createSonareEngineTelemetryRingBuffer(options.telemetryRingCapacity ?? 128)
        : undefined;
    // Meter ring: the engine publishes meters into a SAB ring. Lock-free meter
    // delivery matches the telemetry path and keeps the audio render callback
    // allocation-free in SAB mode.
    const meterRing =
      mode === 'sab' ? createSonareMeterRingBuffer(options.meterRingCapacity ?? 128) : undefined;
    // Scope ring (FFT spectrum + goniometer): opt-in. The per-block FFT is
    // heavier than the meter path, so it is created only when the caller
    // requests scope telemetry via scopeIntervalFrames > 0.
    const scopeIntervalFrames = Math.max(0, Math.floor(options.scopeIntervalFrames ?? 0));
    const scopeRing =
      mode === 'sab' && scopeIntervalFrames > 0
        ? createSonareScopeRingBuffer(options.scopeRingCapacity ?? 64, options.scopeBands ?? 48)
        : undefined;
    const channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    const processorOptions: SonareRealtimeEngineWorkletProcessorOptions = {
      sampleRate: options.sampleRate ?? context.sampleRate,
      blockSize: options.blockSize,
      channelCount,
      commandSharedBuffer: commandRing?.sharedBuffer,
      commandRingCapacity: commandRing?.capacity,
      telemetrySharedBuffer: telemetryRing?.sharedBuffer,
      telemetryRingCapacity: telemetryRing?.capacity,
      meterSharedBuffer: meterRing?.sharedBuffer,
      meterRingCapacity: meterRing?.capacity,
      scopeSharedBuffer: scopeRing?.sharedBuffer,
      scopeRingCapacity: scopeRing?.capacity,
      scopeBands: scopeRing?.bands,
      scopeIntervalFrames: scopeRing ? scopeIntervalFrames : undefined,
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
      numberOfOutputs: 1,
      outputChannelCount: [channelCount],
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
        engineAbiVersion: detectedCapabilities?.engineAbiVersion,
        expectedEngineAbiVersion: detectedCapabilities?.expectedEngineAbiVersion,
        abiCompatible: detectedCapabilities?.abiCompatible,
        degradedReason,
        readyMessage: moduleUrl !== undefined && !options.nodeFactory,
      },
      commandRing,
      telemetryRing,
      meterRing,
      scopeRing,
    );
  }

  play(sampleTime = -1): boolean {
    return this.sendCommand({ type: SonareEngineCommandType.TransportPlay, sampleTime });
  }

  stop(sampleTime = -1): boolean {
    return this.sendCommand({ type: SonareEngineCommandType.TransportStop, sampleTime });
  }

  seekSample(timelineSample: number, sampleTime = -1): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekSample,
      sampleTime,
      argInt: timelineSample,
    });
  }

  seekPpq(ppq: number, sampleTime = -1): boolean {
    return this.sendCommand({
      type: SonareEngineCommandType.TransportSeekPpq,
      sampleTime,
      argFloat: ppq,
    });
  }

  sendCommand(command: SonareEngineCommandRecord): boolean {
    if (this.destroyed) {
      return false;
    }
    if (this.commandRing) {
      return pushSonareEngineCommandRingBuffer(this.commandRing, command);
    }
    this.node.port.postMessage(command);
    return true;
  }

  requestCaptureStatus(): Promise<EngineCaptureStatus> {
    return this.sendCaptureRequest('status').then((response) => {
      if (!response.status) {
        throw new Error('Capture status response is missing status.');
      }
      return response.status;
    });
  }

  requestCapturedAudio(): Promise<Float32Array[]> {
    return this.sendCaptureRequest('read').then((response) =>
      (response.channels ?? []).map((channel) =>
        channel instanceof Float32Array ? channel : new Float32Array(channel),
      ),
    );
  }

  requestCaptureReset(): Promise<void> {
    return this.sendCaptureRequest('reset').then(() => undefined);
  }

  requestTransportState(): Promise<EngineTransportState> {
    return this.sendTransportRequest().then((response) => {
      if (!response.state) {
        throw new Error('Transport state response is missing state.');
      }
      return response.state;
    });
  }

  pollTelemetry(): SonareEngineTelemetryRecord[] {
    if (!this.telemetryRing) {
      return [];
    }
    const read = readSonareEngineTelemetryRingBuffer(this.telemetryRing, this.telemetryReadIndex);
    this.telemetryReadIndex = read.nextReadIndex;
    for (const telemetry of read.telemetry) {
      this.emitTelemetry(telemetry);
    }
    return read.telemetry;
  }

  // Drains any meters published into the SAB meter ring (embind SAB mode) and
  // forwards them to onMeter listeners. In postMessage mode meters arrive via
  // node.port.onmessage instead, so this is a no-op then.
  pollMeters(): SonareWorkletMeterSnapshot[] {
    if (!this.meterRing) {
      return [];
    }
    const read = readSonareMeterRingBuffer(this.meterRing, this.meterReadIndex);
    this.meterReadIndex = read.nextReadIndex;
    for (const meter of read.meters) {
      this.emitMeter(meter);
    }
    return read.meters;
  }

  // Drains scope telemetry (FFT spectrum + goniometer points) published into the
  // SAB scope ring and forwards each record to onScope listeners. A no-op unless
  // the node was created with scopeIntervalFrames > 0 (embind SAB mode).
  pollScope(): SonareWorkletScopeSnapshot[] {
    if (!this.scopeRing) {
      return [];
    }
    const read = readSonareScopeRingBuffer(this.scopeRing, this.scopeReadIndex);
    this.scopeReadIndex = read.nextReadIndex;
    for (const scope of read.scopes) {
      this.emitScope(scope);
    }
    return read.scopes;
  }

  onTelemetry(callback: (telemetry: SonareEngineTelemetryRecord) => void): () => void {
    this.telemetryListeners.add(callback);
    return () => {
      this.telemetryListeners.delete(callback);
    };
  }

  onMeter(callback: (meter: SonareWorkletMeterSnapshot) => void): () => void {
    this.meterListeners.add(callback);
    return () => {
      this.meterListeners.delete(callback);
    };
  }

  onScope(callback: (scope: SonareWorkletScopeSnapshot) => void): () => void {
    this.scopeListeners.add(callback);
    return () => {
      this.scopeListeners.delete(callback);
    };
  }

  /**
   * Subscribe to external-MIDI batches drained from the engine (one call per
   * render block that produced events), already lowered to MIDI 1.0 bytes for a
   * Web MIDI output port. Returns an unsubscribe function.
   */
  onMidiOut(callback: (events: SonareWorkletExternalMidiEvent[]) => void): () => void {
    this.midiOutListeners.add(callback);
    return () => {
      this.midiOutListeners.delete(callback);
    };
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.node.port.postMessage({ type: SonareEngineCommandType.TransportStop, sampleTime: -1 });
    this.node.disconnect();
    for (const pending of this.captureRequests.values()) {
      pending.reject(new Error('Realtime engine node is destroyed.'));
    }
    this.captureRequests.clear();
    for (const pending of this.transportRequests.values()) {
      pending.reject(new Error('Realtime engine node is destroyed.'));
    }
    this.transportRequests.clear();
    this.telemetryListeners.clear();
    this.meterListeners.clear();
    this.scopeListeners.clear();
    this.midiOutListeners.clear();
  }

  private emitTelemetry(telemetry: SonareEngineTelemetryRecord): void {
    for (const listener of this.telemetryListeners) {
      listener(telemetry);
    }
  }

  private emitMeter(meter: SonareWorkletMeterSnapshot): void {
    for (const listener of this.meterListeners) {
      listener(meter);
    }
  }

  private emitMidiOut(events: SonareWorkletExternalMidiEvent[]): void {
    for (const listener of this.midiOutListeners) {
      listener(events);
    }
  }

  private emitScope(scope: SonareWorkletScopeSnapshot): void {
    for (const listener of this.scopeListeners) {
      listener(scope);
    }
  }

  private sendCaptureRequest(
    op: SonareEngineCaptureRequestMessage['op'],
  ): Promise<SonareEngineCaptureResponseMessage> {
    if (this.destroyed) {
      return Promise.reject(new Error('Realtime engine node is destroyed.'));
    }
    const requestId = this.captureRequestId++;
    const promise = new Promise<SonareEngineCaptureResponseMessage>((resolve, reject) => {
      this.captureRequests.set(requestId, { resolve, reject });
    });
    this.node.port.postMessage({ type: 'captureRequest', requestId, op });
    return promise;
  }

  private sendTransportRequest(): Promise<SonareEngineTransportResponseMessage> {
    if (this.destroyed) {
      return Promise.reject(new Error('Realtime engine node is destroyed.'));
    }
    const requestId = this.transportRequestId++;
    const promise = new Promise<SonareEngineTransportResponseMessage>((resolve, reject) => {
      this.transportRequests.set(requestId, { resolve, reject });
    });
    this.node.port.postMessage({ type: 'transportRequest', requestId, op: 'state' });
    return promise;
  }
}
