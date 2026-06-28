import type { SonareRtModule } from '../sonare-rt';
import type { WorkletInput, WorkletOutput } from './audio_types';
import {
  DEFAULT_METRONOME_CONFIG,
  type ResolvedMetronomeConfig,
  resolveMetronomeConfig,
  type SonareEngineCaptureRequestMessage,
  type SonareEngineCaptureResponseMessage,
  type SonareEngineSyncMessage,
  type SonareEngineTransportRequestMessage,
  type SonareEngineTransportResponseMessage,
  type SonareRtRealtimeEngineRuntimeOptions,
  type WorkletPort,
} from './messages';
import {
  engineRingFromSharedBuffer,
  popSonareEngineCommandRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  type SonareEngineTelemetryRingBuffer,
  SonareEngineTelemetryType,
  toBigInt64,
  writeSonareEngineTelemetryRingBuffer,
} from './protocol';

export class SonareRtRealtimeEngineRuntime {
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly channelCount: number;
  private readonly module: SonareRtModule;
  private readonly memory: WebAssembly.Memory;
  private readonly engine: number;
  private readonly channelPointerTable: number;
  private readonly channelBuffers: number[];
  private readonly telemetryIntsPtr: number;
  private readonly telemetryFramesPtr: number;
  private readonly commandRing?: SonareEngineCommandRingBuffer;
  private readonly telemetryRing?: SonareEngineTelemetryRingBuffer;
  private metronomeConfig: ResolvedMetronomeConfig = { ...DEFAULT_METRONOME_CONFIG };
  private closed = false;

  constructor(options: SonareRtRealtimeEngineRuntimeOptions) {
    this.module = options.module;
    this.memory = options.memory;
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    this.commandRing = options.commandSharedBuffer
      ? this.commandRingFromSharedBuffer(options.commandSharedBuffer, options.commandRingCapacity)
      : undefined;
    this.telemetryRing = options.telemetrySharedBuffer
      ? this.telemetryRingFromSharedBuffer(
          options.telemetrySharedBuffer,
          options.telemetryRingCapacity,
        )
      : undefined;

    this.engine = this.module._sonare_rt_engine_create();
    if (this.engine <= 0) {
      throw new Error('failed to create sonare-rt engine');
    }
    if (
      this.module._sonare_rt_engine_prepare(
        this.engine,
        this.sampleRate,
        this.blockSize,
        1024,
        1024,
      ) !== 1
    ) {
      this.module._sonare_rt_engine_destroy(this.engine);
      throw new Error('failed to prepare sonare-rt engine');
    }
    this.channelPointerTable = this.module._malloc(
      this.channelCount * Uint32Array.BYTES_PER_ELEMENT,
    );
    this.channelBuffers = [];
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.channelBuffers.push(
        this.module._malloc(this.blockSize * Float32Array.BYTES_PER_ELEMENT),
      );
    }
    this.telemetryIntsPtr = this.module._malloc(64 * 4 * Int32Array.BYTES_PER_ELEMENT);
    this.telemetryFramesPtr = this.module._malloc(64 * 3 * Float64Array.BYTES_PER_ELEMENT);
    this.writeChannelPointers();
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const firstOutput = output?.[0];
    if (!firstOutput) {
      return true;
    }
    const frames = firstOutput.length;
    if (frames > this.blockSize) {
      for (const channel of output) {
        channel.fill(0);
      }
      return true;
    }

    this.drainCommands();
    const heap = new Float32Array(this.memory.buffer);
    const input = inputs[0];
    for (let ch = 0; ch < this.channelCount; ch++) {
      const ptr = this.channelBuffers[ch] ?? this.channelBuffers[0];
      const offset = ptr >> 2;
      const source = input?.[ch];
      if (source && source.length === frames) {
        heap.set(source, offset);
      } else {
        heap.fill(0, offset, offset + frames);
      }
    }

    this.module._sonare_rt_engine_process(
      this.engine,
      this.channelPointerTable,
      this.channelCount,
      frames,
    );

    for (let ch = 0; ch < output.length; ch++) {
      const target = output[ch];
      const ptr = this.channelBuffers[ch] ?? this.channelBuffers[0];
      target.set(heap.subarray(ptr >> 2, (ptr >> 2) + target.length));
    }
    this.publishTelemetry();
    return true;
  }

  receiveCommand(command: SonareEngineCommandRecord): void {
    if (!this.closed) {
      this.applyCommand(command);
    }
  }

  // Out-of-band control sync for the sonare-rt runtime. The sonare-rt C ABI
  // (src/wasm/rt_bindings.cpp) exposes set_metronome_enabled and seek_marker but
  // NOT set_clips / set_markers, so clip/marker mutations cannot be applied to a
  // live sonare-rt engine. We honor the metronome config and surface a clear
  // telemetry error for the unsupported clip/marker paths instead of silently
  // dropping them. The default 'embind' runtime wires all three fully.
  receiveSync(message: SonareEngineSyncMessage): void {
    if (this.closed) {
      return;
    }
    switch (message.type) {
      case 'syncMetronome':
        this.metronomeConfig = resolveMetronomeConfig(message.config);
        this.module._sonare_rt_engine_set_metronome_enabled(
          this.engine,
          message.config.enabled ? 1 : 0,
          this.metronomeConfig.beatGain,
          this.metronomeConfig.accentGain,
          this.metronomeConfig.clickSamples,
        );
        break;
      case 'syncTempo':
        this.module._sonare_rt_engine_set_tempo(this.engine, message.bpm);
        break;
      case 'syncClips':
      case 'syncClipsDelta':
      case 'syncMidiClips':
      case 'syncMarkers':
      case 'syncAutomation':
      case 'syncMixer':
      case 'syncCapture':
      case 'syncTrackStripEqBand':
      case 'syncMasterStripEqBand':
      case 'syncTrackStripInsertBypassed':
      case 'syncMasterStripInsertBypassed':
      case 'syncBuiltinInstrument':
      case 'syncSynthInstrument':
      case 'syncSf2Instrument':
      case 'syncLoadSoundFont':
      case 'syncMidiFx':
      case 'syncClearMidiFx':
      case 'syncMidiNoteOn':
      case 'syncMidiNoteOff':
      case 'syncMidiCc':
      case 'syncMidiPanic':
        // The sonare-rt C ABI exposes no set_clips / set_markers /
        // set_automation_lane / set_track_lanes, so these mutations cannot
        // reach a live sonare-rt engine. Surface a clear telemetry error rather
        // than silently dropping.
        if (this.telemetryRing) {
          writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
            type: SonareEngineTelemetryType.Error,
            error: SonareEngineTelemetryError.UnknownTarget,
            renderFrame: 0,
            timelineSample: 0,
            audibleTimelineSample: 0,
            graphLatencySamplesQ8: 0,
            value: 0,
          });
        }
        break;
    }
  }

  receiveCaptureRequest(message: SonareEngineCaptureRequestMessage, port?: WorkletPort): void {
    if (this.closed) {
      return;
    }
    port?.postMessage?.({
      type: 'captureResponse',
      requestId: message.requestId,
      ok: false,
      error: 'Capture read-back is not supported by the sonare-rt runtime.',
    } satisfies SonareEngineCaptureResponseMessage);
  }

  receiveTransportRequest(message: SonareEngineTransportRequestMessage, port?: WorkletPort): void {
    if (this.closed) {
      return;
    }
    port?.postMessage?.({
      type: 'transportResponse',
      requestId: message.requestId,
      ok: false,
      error: 'Transport state read-back is not supported by the sonare-rt runtime.',
    } satisfies SonareEngineTransportResponseMessage);
  }

  destroy(): void {
    if (this.closed) {
      return;
    }
    this.module._free(this.telemetryFramesPtr);
    this.module._free(this.telemetryIntsPtr);
    for (const ptr of this.channelBuffers) {
      this.module._free(ptr);
    }
    this.module._free(this.channelPointerTable);
    this.module._sonare_rt_engine_destroy(this.engine);
    this.closed = true;
  }

  private writeChannelPointers(): void {
    const pointers = new Uint32Array(this.memory.buffer);
    const offset = this.channelPointerTable >> 2;
    for (let ch = 0; ch < this.channelBuffers.length; ch++) {
      pointers[offset + ch] = this.channelBuffers[ch];
    }
  }

  private drainCommands(): void {
    if (!this.commandRing) {
      return;
    }
    for (let i = 0; i < 64; i++) {
      const command = popSonareEngineCommandRingBuffer(this.commandRing);
      if (!command) {
        return;
      }
      this.applyCommand(command);
    }
  }

  private applyCommand(command: SonareEngineCommandRecord): void {
    const sampleTime = toBigInt64(command.sampleTime, -1n);
    switch (command.type) {
      case SonareEngineCommandType.SetParam:
      case SonareEngineCommandType.SetParamSmoothed:
        // The sonare-rt C ABI (src/wasm/rt_bindings.cpp) does not export a
        // sonare_rt_engine_set_param entry point, so parameter automation has no
        // realtime transport on this runtime target. Surface a clear error
        // telemetry record (rather than silently dropping the command) so hosts
        // can detect the unsupported path; the embind runtime fully wires this.
        if (this.telemetryRing) {
          writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
            type: SonareEngineTelemetryType.Error,
            error: SonareEngineTelemetryError.UnknownTarget,
            renderFrame: 0,
            timelineSample: 0,
            audibleTimelineSample: 0,
            graphLatencySamplesQ8: 0,
            value: Number(command.type),
          });
        }
        break;
      case SonareEngineCommandType.TransportPlay:
        this.module._sonare_rt_engine_play(this.engine, sampleTime);
        break;
      case SonareEngineCommandType.TransportStop:
        this.module._sonare_rt_engine_stop(this.engine, sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekSample:
        this.module._sonare_rt_engine_seek_sample(
          this.engine,
          toBigInt64(command.argInt, 0n),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.TransportSeekPpq:
        this.module._sonare_rt_engine_seek_ppq(
          this.engine,
          Number(command.argFloat ?? 0),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.SetTempoMap:
        this.module._sonare_rt_engine_set_tempo(this.engine, Number(command.argFloat ?? 120));
        break;
      case SonareEngineCommandType.SetLoop:
        this.module._sonare_rt_engine_set_loop(
          this.engine,
          Number(command.argFloat ?? 0),
          Number(command.argInt ?? 0) / 1_000_000,
          command.targetId ? 1 : 0,
        );
        break;
      case SonareEngineCommandType.ArmRecord:
        this.module._sonare_rt_engine_set_capture_armed(this.engine, command.argInt ? 1 : 0);
        break;
      case SonareEngineCommandType.Punch:
        // Both endpoints already arrive as samples (see SonareEngine.punch);
        // do NOT re-scale by sampleRate.
        this.module._sonare_rt_engine_set_capture_punch(
          this.engine,
          toBigInt64(command.argInt, 0n),
          BigInt(Math.max(0, Math.round(Number(command.argFloat ?? 0)))),
          1,
        );
        break;
      case SonareEngineCommandType.SetMetronome:
        this.module._sonare_rt_engine_set_metronome_enabled(
          this.engine,
          command.argInt ? 1 : 0,
          this.metronomeConfig.beatGain,
          this.metronomeConfig.accentGain,
          this.metronomeConfig.clickSamples,
        );
        break;
      case SonareEngineCommandType.SeekMarker:
        this.module._sonare_rt_engine_seek_marker(
          this.engine,
          Math.trunc(command.targetId ?? 0),
          sampleTime,
        );
        break;
      default:
        if (this.telemetryRing) {
          writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
            type: SonareEngineTelemetryType.Error,
            error: SonareEngineTelemetryError.UnknownTarget,
            renderFrame: 0,
            timelineSample: 0,
            audibleTimelineSample: 0,
            graphLatencySamplesQ8: 0,
            value: Number(command.type),
          });
        }
        break;
    }
  }

  private publishTelemetry(): void {
    if (!this.telemetryRing) {
      this.module._sonare_rt_engine_drain_telemetry(
        this.engine,
        this.telemetryIntsPtr,
        this.telemetryFramesPtr,
        64,
      );
      return;
    }
    const count = this.module._sonare_rt_engine_drain_telemetry(
      this.engine,
      this.telemetryIntsPtr,
      this.telemetryFramesPtr,
      64,
    );
    const ints = new Int32Array(this.memory.buffer);
    const frames = new Float64Array(this.memory.buffer);
    const intBase = this.telemetryIntsPtr >> 2;
    const frameBase = this.telemetryFramesPtr >> 3;
    for (let i = 0; i < count; i++) {
      writeSonareEngineTelemetryRingBuffer(this.telemetryRing, {
        type: ints[intBase + i * 4],
        error: ints[intBase + i * 4 + 1],
        renderFrame: frames[frameBase + i * 3],
        timelineSample: frames[frameBase + i * 3 + 1],
        audibleTimelineSample: frames[frameBase + i * 3 + 2],
        graphLatencySamplesQ8: ints[intBase + i * 4 + 2],
        value: ints[intBase + i * 4 + 3],
      });
    }
  }

  private commandRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineCommandRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_COMMAND_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }

  private telemetryRingFromSharedBuffer(
    sharedBuffer: SharedArrayBuffer,
    fallbackCapacity?: number,
  ): SonareEngineTelemetryRingBuffer {
    const ring = engineRingFromSharedBuffer(
      sharedBuffer,
      SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
      fallbackCapacity,
    );
    return { sharedBuffer, header: ring.header, view: ring.view, capacity: ring.capacity };
  }
}
