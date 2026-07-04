import type { EngineClip, EngineScopeTelemetry } from '../index';
import { RealtimeEngine } from '../index';
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
  type SonareRealtimeEngineWorkletProcessorOptions,
  type WorkletTransport,
} from './messages';
import {
  encodeFrameHi,
  encodeFrameLo,
  engineRingFromSharedBuffer,
  meterFromEngine,
  meterRingFromSharedBuffer,
  popSonareEngineCommandRingBuffer,
  type SharedMeterRingWriter,
  type SharedScopeRingWriter,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  type SonareEngineTelemetryRecord,
  type SonareEngineTelemetryRingBuffer,
  SonareEngineTelemetryType,
  type SonareWorkletMeterSnapshot,
  scopeRingFromSharedBuffer,
  telemetryFromEngine,
  writeSonareEngineTelemetryRingBuffer,
} from './protocol';

/**
 * AudioWorklet-style bridge for the DAW realtime engine facade.
 *
 * Backed by the `sonare.wasm` embind facade.
 */
export class SonareRealtimeEngineWorkletProcessor {
  private static warnedChannelScratchOverflow = false;
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly channelCount: number;
  private engine: RealtimeEngine;
  private closed = false;
  private commandRing?: SonareEngineCommandRingBuffer;
  private telemetryRing?: SonareEngineTelemetryRingBuffer;
  private meterRing?: SharedMeterRingWriter;
  private scopeRing?: SharedScopeRingWriter;
  private transport?: WorkletTransport;
  private meterIntervalFrames: number;
  private lastMeterFrame = Number.NEGATIVE_INFINITY;
  // Latest metronome gains/click length pushed via 'syncMetronome'. The
  // SetMetronome command only toggles enabled state; the config arrives here.
  private metronomeConfig: ResolvedMetronomeConfig = { ...DEFAULT_METRONOME_CONFIG };
  // Zero-copy prepared realtime path: persistent per-channel views onto the
  // engine's WASM-heap scratch (acquired once on the main thread via
  // getChannelBuffer). process() writes the AudioWorklet input straight into
  // these views, calls engine.processPrepared(frames) which runs the engine IN
  // PLACE, then reads the same views back — no std::vector or JS Float32Array is
  // allocated per render quantum (the old engine.process() round-tripped fresh
  // arrays on both heaps every block, an RT-safety hazard).
  private channelBuffers: Float32Array[];
  private readonly liveClips = new Map<number, EngineClip>();

  constructor(
    options: SonareRealtimeEngineWorkletProcessorOptions = {},
    transport?: WorkletTransport,
  ) {
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    this.transport = transport;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.commandRing = options.commandSharedBuffer
      ? this.commandRingFromSharedBuffer(options.commandSharedBuffer, options.commandRingCapacity)
      : undefined;
    this.telemetryRing = options.telemetrySharedBuffer
      ? this.telemetryRingFromSharedBuffer(
          options.telemetrySharedBuffer,
          options.telemetryRingCapacity,
        )
      : undefined;
    this.meterRing = options.meterSharedBuffer
      ? meterRingFromSharedBuffer(options.meterSharedBuffer, options.meterRingCapacity)
      : undefined;
    this.scopeRing = options.scopeSharedBuffer
      ? scopeRingFromSharedBuffer(
          options.scopeSharedBuffer,
          options.scopeRingCapacity,
          options.scopeBands,
        )
      : undefined;
    this.engine = new RealtimeEngine(this.sampleRate, this.blockSize);
    // Allocate persistent WASM-heap scratch (worst case: channelCount channels x
    // blockSize frames) and acquire the per-channel heap views once.
    this.engine.prepareChannels(this.channelCount, this.blockSize);
    this.channelBuffers = new Array(this.channelCount);
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.channelBuffers[ch] = this.engine.getChannelBuffer(ch, this.blockSize);
    }
    // Arm the engine's scope producer only when a scope ring was provided. The
    // band count follows the ring's record layout so writeScopeRing never
    // overruns its slot.
    if (this.scopeRing) {
      const interval = Math.max(1, Math.floor(options.scopeIntervalFrames ?? this.blockSize));
      this.engine.configureScopeTelemetry(interval, this.scopeRing.bands);
    }
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
      for (const channel of output ?? []) {
        channel.fill(0);
      }
      this.publishTelemetry();
      return true;
    }

    this.drainCommands();

    // Clamp `frames` to the pre-allocated scratch capacity. The earlier
    // `frames > this.blockSize` branch already returns early, so this is
    // defensive — but we warn once if it ever fires so the contract violation
    // is visible.
    let usableFrames = frames;
    if (usableFrames > this.blockSize) {
      if (!SonareRealtimeEngineWorkletProcessor.warnedChannelScratchOverflow) {
        SonareRealtimeEngineWorkletProcessor.warnedChannelScratchOverflow = true;
        // biome-ignore lint/suspicious/noConsole: realtime-safety diagnostic.
        console.warn(
          `SonareRealtimeEngineWorkletProcessor: requested ${usableFrames} frames ` +
            `exceeds pre-allocated capacity ${this.blockSize}; clamping.`,
        );
      }
      usableFrames = this.blockSize;
    }

    // Defend against WASM linear-memory growth detaching the cached heap views:
    // if any view's backing ArrayBuffer has been detached (byteLength === 0),
    // re-acquire all of them. This is a control-flow check (no allocation in the
    // common case where memory did not grow).
    if ((this.channelBuffers[0]?.byteLength ?? 0) === 0) {
      this.reacquireChannelBuffers();
    }

    const input = inputs[0];
    // Write the AudioWorklet input straight into the engine's WASM-heap views;
    // no per-block heap allocation.
    for (let ch = 0; ch < this.channelCount; ch++) {
      const dst = this.channelBuffers[ch];
      const source = input?.[ch];
      if (source && source.length === usableFrames) {
        dst.set(source.subarray(0, usableFrames));
      } else {
        dst.fill(0, 0, usableFrames);
      }
    }

    // Run the engine in place over the prepared scratch (allocation-free).
    this.engine.processPrepared(usableFrames);

    for (let ch = 0; ch < output.length; ch++) {
      const target = output[ch];
      const source = this.channelBuffers[ch] ?? this.channelBuffers[0];
      if (source) {
        target.set(source.subarray(0, Math.min(target.length, usableFrames)));
        if (target.length > usableFrames) {
          target.fill(0, usableFrames);
        }
      } else {
        target.fill(0);
      }
    }
    this.publishTelemetry();
    this.publishMeters();
    this.publishScope();
    this.publishExternalMidi();
    return true;
  }

  private reacquireChannelBuffers(): void {
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.channelBuffers[ch] = this.engine.getChannelBuffer(ch, this.blockSize);
    }
  }

  receiveCommand(command: SonareEngineCommandRecord): void {
    if (!this.closed) {
      this.applyCommand(command);
    }
  }

  // Applies an out-of-band control-plane sync message. Runs on the AudioWorklet
  // global scope but OUTSIDE process() (the message-port callback), so the
  // bulk/allocating engine setters (setClips/setMarkers) are safe here — they
  // never run on the realtime render path. This is the audio-thread equivalent
  // of the engine's control-thread RtPublisher setters.
  receiveSync(message: SonareEngineSyncMessage): void {
    if (this.closed) {
      return;
    }
    switch (message.type) {
      case 'syncClips':
        this.liveClips.clear();
        for (const clip of message.clips) {
          if (clip.id !== undefined) {
            this.liveClips.set(clip.id, clip);
          }
        }
        this.engine.setClips(message.clips);
        break;
      case 'syncClipsDelta':
        for (const clipId of message.removeIds) {
          this.liveClips.delete(clipId);
        }
        for (const clip of message.upserts) {
          if (clip.id !== undefined) {
            this.liveClips.set(clip.id, clip);
          }
        }
        this.engine.setClips(Array.from(this.liveClips.values()));
        break;
      case 'syncMidiClips':
        this.engine.setMidiClips(message.clips);
        break;
      case 'syncMarkers':
        this.engine.setMarkers(message.markers);
        break;
      case 'syncMetronome':
        this.metronomeConfig = resolveMetronomeConfig(message.config);
        this.engine.setMetronome(message.config);
        break;
      case 'syncAutomation':
        this.engine.setAutomationLane(message.paramId, message.points);
        break;
      case 'syncTempo':
        if (message.tempoSegments) {
          this.engine.setTempoSegments(message.tempoSegments);
        } else {
          this.engine.setTempo(message.bpm);
        }
        if (message.timeSignatureSegments) {
          this.engine.setTimeSignatureSegments(message.timeSignatureSegments);
        } else {
          this.engine.setTimeSignature(
            message.timeSignature.numerator,
            message.timeSignature.denominator,
          );
        }
        break;
      case 'syncMixer':
        if (message.buses) {
          this.engine.setTrackBuses(message.buses);
        }
        this.engine.setTrackLanes(message.lanes);
        for (const strip of message.trackStrips ?? []) {
          this.engine.setTrackStripJson(strip.trackId, strip.sceneJson);
        }
        for (const strip of message.busStrips ?? []) {
          this.engine.setBusStripJson(strip.busId, strip.sceneJson);
        }
        if (message.masterStripJson) {
          this.engine.setMasterStripJson(message.masterStripJson);
        }
        for (const binding of message.laneSidechains ?? []) {
          this.engine.setLaneSidechain(binding.trackId, binding.insertIndex, binding.sourceTrackId);
        }
        break;
      case 'syncCapture':
        this.engine.setCaptureBuffer(message.channels, message.bufferFrames);
        this.engine.setCaptureSource(message.source);
        this.engine.setRecordOffsetSamples(message.recordOffsetSamples);
        this.engine.setInputMonitor(message.inputMonitor.enabled, message.inputMonitor.gain);
        break;
      case 'syncTrackStripEqBand':
        this.engine.setTrackStripEqBandJson(message.trackId, message.bandIndex, message.bandJson);
        break;
      case 'syncMasterStripEqBand':
        this.engine.setMasterStripEqBandJson(message.bandIndex, message.bandJson);
        break;
      case 'syncTrackStripInsertBypassed':
        this.engine.setTrackStripInsertBypassed(
          message.trackId,
          message.insertIndex,
          message.bypassed,
          message.resetOnBypass,
        );
        break;
      case 'syncMasterStripInsertBypassed':
        this.engine.setMasterStripInsertBypassed(
          message.insertIndex,
          message.bypassed,
          message.resetOnBypass,
        );
        break;
      case 'syncTrackStripInsertParamByName':
        this.engine.setTrackStripInsertParamByName(
          message.trackId,
          message.insertIndex,
          message.paramName,
          message.value,
        );
        break;
      case 'syncMasterStripInsertParamByName':
        this.engine.setMasterStripInsertParamByName(
          message.insertIndex,
          message.paramName,
          message.value,
        );
        break;
      case 'syncBusStripInsertParamByName':
        this.engine.setBusStripInsertParamByName(
          message.busId,
          message.insertIndex,
          message.paramName,
          message.value,
        );
        break;
      case 'syncTrackStripPan':
        this.engine.setTrackStripPan(message.trackId, message.pan);
        break;
      case 'syncTrackStripPanLaw':
        this.engine.setTrackStripPanLaw(message.trackId, message.panLaw);
        break;
      case 'syncTrackStripPanMode':
        this.engine.setTrackStripPanMode(message.trackId, message.panMode);
        break;
      case 'syncTrackStripDualPan':
        this.engine.setTrackStripDualPan(message.trackId, message.leftPan, message.rightPan);
        break;
      case 'syncTrackStripChannelDelaySamples':
        this.engine.setTrackStripChannelDelaySamples(message.trackId, message.delaySamples);
        break;
      case 'syncBuiltinInstrument':
        this.engine.setBuiltinInstrument(message.config, message.destinationId);
        break;
      case 'syncSynthInstrument':
        this.engine.setSynthInstrument(message.patch, message.destinationId);
        break;
      case 'syncLoadSoundFont':
        this.engine.loadSoundFont(message.data);
        break;
      case 'syncSf2Instrument':
        this.engine.setSf2Instrument(message.config, message.destinationId);
        break;
      case 'syncMidiFx':
        this.engine.setMidiFx(message.destinationId, message.configJson ?? '');
        break;
      case 'syncClearMidiFx':
        this.engine.clearMidiFx(message.destinationId);
        break;
      case 'syncMidiNoteOn':
        this.engine.pushMidiNoteOn(
          message.destinationId,
          message.group,
          message.channel,
          message.note,
          message.velocity,
          message.renderFrame,
        );
        break;
      case 'syncMidiNoteOff':
        this.engine.pushMidiNoteOff(
          message.destinationId,
          message.group,
          message.channel,
          message.note,
          message.velocity,
          message.renderFrame,
        );
        break;
      case 'syncMidiCc':
        this.engine.pushMidiCc(
          message.destinationId,
          message.group,
          message.channel,
          message.controller,
          message.value,
          message.renderFrame,
        );
        break;
      case 'syncMidiSysex':
        this.engine.pushMidiSysex(message.destinationId, message.data, message.renderFrame);
        break;
      case 'syncMidiPanic':
        this.engine.pushMidiPanic(message.renderFrame);
        break;
      case 'syncMidiDestinationExternal':
        this.engine.setMidiDestinationExternal(message.destinationId, message.external);
        break;
      case 'syncExternalMidiClock':
        this.engine.setExternalMidiClockEnabled(message.enabled);
        break;
    }
  }

  receiveCaptureRequest(message: SonareEngineCaptureRequestMessage): void {
    if (this.closed) {
      return;
    }
    try {
      if (message.op === 'status') {
        const status = this.engine.captureStatus();
        this.transport?.postMessage?.({
          type: 'captureResponse',
          requestId: message.requestId,
          ok: true,
          status: {
            capturedFrames: status.capturedFrames,
            overflowCount: status.overflowCount,
            armed: status.armed,
            punchEnabled: status.punchEnabled,
            source: status.source,
            recordOffsetSamples: status.recordOffsetSamples,
          },
        } satisfies SonareEngineCaptureResponseMessage);
        return;
      }
      if (message.op === 'read') {
        const captured = this.engine.capturedAudio();
        const channels: number[][] = [];
        for (let ch = 0; ch < captured.length; ch++) {
          const source = captured[ch];
          const copy: number[] = [];
          for (let i = 0; i < source.length; i++) {
            copy.push(Number(source[i]));
          }
          channels.push(copy);
        }
        this.transport?.postMessage?.({
          type: 'captureResponse',
          requestId: message.requestId,
          ok: true,
          channels,
        } satisfies SonareEngineCaptureResponseMessage);
        return;
      }
      this.engine.resetCapture();
      this.transport?.postMessage?.({
        type: 'captureResponse',
        requestId: message.requestId,
        ok: true,
      } satisfies SonareEngineCaptureResponseMessage);
    } catch (error) {
      this.transport?.postMessage?.({
        type: 'captureResponse',
        requestId: message.requestId,
        ok: false,
        error: error instanceof Error ? error.message : String(error),
      } satisfies SonareEngineCaptureResponseMessage);
    }
  }

  receiveTransportRequest(message: SonareEngineTransportRequestMessage): void {
    if (this.closed) {
      return;
    }
    try {
      this.transport?.postMessage?.({
        type: 'transportResponse',
        requestId: message.requestId,
        ok: true,
        state: this.engine.getTransportState(),
      } satisfies SonareEngineTransportResponseMessage);
    } catch (error) {
      this.transport?.postMessage?.({
        type: 'transportResponse',
        requestId: message.requestId,
        ok: false,
        error: error instanceof Error ? error.message : String(error),
      } satisfies SonareEngineTransportResponseMessage);
    }
  }

  destroy(): void {
    if (!this.closed) {
      this.engine.destroy();
      this.closed = true;
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
    const sampleTime = Number(command.sampleTime ?? -1);
    switch (command.type) {
      case SonareEngineCommandType.SetParam:
        // paramId is carried in targetId, the new value in argFloat (matches the
        // SonareEngine.setParam producer). sampleTime is the render frame.
        this.engine.setParameter(
          Math.trunc(Number(command.targetId ?? 0)),
          Number(command.argFloat ?? 0),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.SetParamSmoothed:
        this.engine.setParameterSmoothed(
          Math.trunc(Number(command.targetId ?? 0)),
          Number(command.argFloat ?? 0),
          sampleTime,
        );
        break;
      case SonareEngineCommandType.TransportPlay:
        this.engine.play(sampleTime);
        break;
      case SonareEngineCommandType.TransportStop:
        this.engine.stop(sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekSample:
        this.engine.seekSample(Number(command.argInt ?? 0), sampleTime);
        break;
      case SonareEngineCommandType.TransportSeekPpq:
        this.engine.seekPpq(Number(command.argFloat ?? 0), sampleTime);
        break;
      case SonareEngineCommandType.SetTempoMap:
        this.engine.setTempo(Number(command.argFloat ?? 120));
        break;
      case SonareEngineCommandType.SetLoop:
        this.engine.setLoop(
          Number(command.argFloat ?? 0),
          Number(command.argInt ?? 0) / 1_000_000,
          command.targetId !== 0,
        );
        break;
      case SonareEngineCommandType.ArmRecord:
        this.engine.armCapture(Boolean(command.argInt));
        break;
      case SonareEngineCommandType.Punch:
        // Both endpoints already arrive as samples (see SonareEngine.punch);
        // do NOT re-scale by sampleRate.
        this.engine.setCapturePunch(
          Number(command.argInt ?? 0),
          Math.max(0, Math.round(Number(command.argFloat ?? 0))),
          true,
        );
        break;
      case SonareEngineCommandType.SetMetronome:
        // Metronome config (beatGain/accentGain/clickSamples/clickSeconds) is
        // delivered out-of-band via the 'syncMetronome' message so it carries
        // the caller's full config; the command only toggles enabled state as a
        // sample-aligned fallback.
        this.engine.setMetronome({
          enabled: Boolean(command.argInt),
          beatGain: this.metronomeConfig.beatGain,
          accentGain: this.metronomeConfig.accentGain,
          clickSamples: this.metronomeConfig.clickSamples,
        });
        break;
      case SonareEngineCommandType.SeekMarker:
        // The realtime engine's markers are kept in sync via 'syncMarkers'
        // (RtPublisher-style swap), so a queued kSeekMarker resolves correctly.
        this.engine.seekMarker(Math.trunc(Number(command.targetId ?? 0)), sampleTime);
        break;
      case SonareEngineCommandType.SetSoloMute:
        this.engine.setSoloMute(
          Math.trunc(Number(command.targetId ?? 0)),
          Boolean((Number(command.argInt ?? 0) & 0x2) !== 0),
          Boolean((Number(command.argInt ?? 0) & 0x1) !== 0),
          sampleTime,
        );
        break;
      default:
        this.publishTelemetryRecord({
          type: SonareEngineTelemetryType.Error,
          error: SonareEngineTelemetryError.UnknownTarget,
          renderFrame: 0,
          timelineSample: 0,
          audibleTimelineSample: 0,
          graphLatencySamplesQ8: 0,
          value: Number(command.type),
        });
        break;
    }
  }

  private publishTelemetry(): void {
    for (const item of this.engine.drainTelemetry(64)) {
      this.publishTelemetryRecord(telemetryFromEngine(item));
    }
  }

  private publishTelemetryRecord(record: SonareEngineTelemetryRecord): void {
    if (this.telemetryRing) {
      writeSonareEngineTelemetryRingBuffer(this.telemetryRing, record);
      return;
    }
    this.transport?.postMessage?.(record);
  }

  // Drains the engine meter telemetry queue into the stereo meter ring / transport.
  //
  // Shared-queue contract: `drainMeterTelemetry` and `drainMeterTelemetryWide`
  // pop the SAME single-consumer telemetry queue, so exactly ONE of them may run
  // per engine. The live worklet path owns the queue via the stereo drain below;
  // the worklet meter ring (SONARE_METER_RING_RECORD_FLOATS) is a fixed stereo
  // layout carrying planes 0/1 plus the correlation/LUFS summary. Per-plane
  // surround meters are NOT delivered over the live worklet ring — a host that
  // needs them must use the offline `drainMeterTelemetryWide()` API on a
  // non-worklet engine instance (do not also call it on a worklet-driven engine,
  // or the two drains will starve each other).
  private publishMeters(): void {
    if (this.meterIntervalFrames <= 0 || (!this.transport && !this.meterRing)) {
      return;
    }
    for (const item of this.engine.drainMeterTelemetry(64)) {
      const meter = meterFromEngine(item);
      if (
        meter.frame !== this.lastMeterFrame &&
        meter.frame - this.lastMeterFrame < this.meterIntervalFrames
      ) {
        continue;
      }
      if (meter.frame !== this.lastMeterFrame) {
        this.lastMeterFrame = meter.frame;
      }
      // Prefer the lock-free SAB meter ring (matching the telemetry path and
      // SonareWorkletProcessor); only fall back to structured-clone postMessage
      // when no ring was provided, so we do not allocate/post from the audio
      // render callback in SAB mode.
      if (this.meterRing) {
        this.writeMeterRing(meter);
      } else {
        this.transport?.onMeter?.(meter);
        this.transport?.postMessage?.(meter);
      }
    }
  }

  private writeMeterRing(meter: SonareWorkletMeterSnapshot): void {
    const ring = this.meterRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    ring.records[offset] = encodeFrameLo(meter.frame);
    ring.records[offset + 1] = encodeFrameHi(meter.frame);
    ring.records[offset + 2] = meter.targetId;
    ring.records[offset + 3] = meter.peakDbL;
    ring.records[offset + 4] = meter.peakDbR;
    ring.records[offset + 5] = meter.rmsDbL;
    ring.records[offset + 6] = meter.rmsDbR;
    ring.records[offset + 7] = meter.correlation;
    ring.records[offset + 8] = meter.truePeakDbL;
    ring.records[offset + 9] = meter.truePeakDbR;
    ring.records[offset + 10] = meter.momentaryLufs;
    ring.records[offset + 11] = meter.shortTermLufs;
    ring.records[offset + 12] = meter.integratedLufs;
    ring.records[offset + 13] = meter.gainReductionDb;
    Atomics.store(ring.header, 0, writeIndex + 1);
    // writeIndex is a free-running monotonic counter, so an overflow guard here
    // would fire on essentially every write past the first `capacity` records
    // and store an ever-growing value, not a dropped-record count. Readers
    // already detect silent overrun via firstReadable = max(readIndex,
    // writeIndex - capacity), so header slot 3 is left at its initial 0.
  }

  // Drains the engine's scope producer (FFT spectrum + goniometer points) into
  // the lock-free SAB scope ring. No allocation on the render path: records are
  // written field-by-field into the ring.
  private publishScope(): void {
    const ring = this.scopeRing;
    if (!ring) {
      return;
    }
    for (const item of this.engine.drainScopeTelemetry(64)) {
      this.writeScopeRing(ring, item);
    }
  }

  // Drains queued external-MIDI events (already lowered to MIDI 1.0 bytes) and
  // forwards them to the main thread for delivery to Web MIDI output ports.
  // One batch per render block; skipped entirely when nothing is queued, so an
  // all-internal project never allocates or posts here.
  private publishExternalMidi(): void {
    if (!this.transport?.postMessage) {
      return;
    }
    const events = this.engine.drainExternalMidi(256);
    if (events.length === 0) {
      return;
    }
    this.transport.postMessage({ type: 'externalMidi', events });
  }

  private writeScopeRing(ring: SharedScopeRingWriter, record: EngineScopeTelemetry): void {
    const writeIndex = Atomics.load(ring.header, 0);
    const base = (writeIndex % ring.capacity) * ring.recordFloats;
    ring.records[base] = encodeFrameLo(record.renderFrame);
    ring.records[base + 1] = encodeFrameHi(record.renderFrame);
    ring.records[base + 2] = record.targetId;
    const bandCount = Math.min(ring.bands, record.bands.length);
    ring.records[base + 3] = bandCount;
    const pointCount = Math.min(ring.maxPoints, record.points.length);
    ring.records[base + 4] = pointCount;
    const bandsBase = base + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS;
    for (let i = 0; i < bandCount; i++) {
      ring.records[bandsBase + i] = record.bands[i];
    }
    const pointsBase = bandsBase + ring.bands;
    for (let i = 0; i < pointCount; i++) {
      const point = record.points[i];
      ring.records[pointsBase + 2 * i] = point.left;
      ring.records[pointsBase + 2 * i + 1] = point.right;
    }
    Atomics.store(ring.header, 0, writeIndex + 1);
    // Like writeMeterRing, writeIndex is a free-running monotonic counter; the
    // reader detects silent overrun via firstReadable, so the overflow slot
    // (header[5]) stays at its initial 0.
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
