import type { EngineClip } from '../index';
import { RealtimeEngine } from '../index';
import type { WorkletInput, WorkletOutput } from './audio_types';
import {
  DEFAULT_METRONOME_CONFIG,
  type ResolvedMetronomeConfig,
  resolveMetronomeConfig,
  type SonareEngineCaptureRequestMessage,
  type SonareEngineCaptureResponseMessageInternal,
  type SonareEngineSyncMessage,
  type SonareEngineTransportRequestMessage,
  type SonareEngineTransportResponseMessage,
  type SonareRealtimeEngineWorkletProcessorOptions,
  type WorkletTransport,
} from './messages';
import {
  clipPageRequestRingFromSharedBuffer,
  encodeFrameHi,
  encodeFrameLo,
  engineRingFromSharedBuffer,
  externalMidiRingFromSharedBuffer,
  meterFromEngine,
  meterRingFromSharedBuffer,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareExternalMidiRingBuffer,
  type SharedClipPageRequestRingWriter,
  type SharedExternalMidiRingWriter,
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
  writeInt64Words,
  writeSonareEngineTelemetryRingBuffer,
} from './protocol';

/**
 * Copies one plane per output channel, zero-filling the tail past `frames` and
 * any channel the source does not cover. Shared by the program and cue outputs
 * so the two cannot drift in their padding behaviour. Allocation-free.
 */
function copyPlanesToOutput(
  output: Float32Array[],
  planes: readonly Float32Array[],
  frames: number,
): void {
  for (let ch = 0; ch < output.length; ch++) {
    const target = output[ch];
    const source = planes[ch] ?? planes[0];
    if (source) {
      target.set(source.subarray(0, Math.min(target.length, frames)));
      if (target.length > frames) {
        target.fill(0, frames);
      }
    } else {
      target.fill(0);
    }
  }
}

function captureTransferList(channels: readonly Float32Array[]): Transferable[] {
  const transfers: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const channel of channels) {
    const buffer = channel.buffer;
    if (!(buffer instanceof ArrayBuffer)) {
      throw new TypeError('capture response channels must use plain ArrayBuffers');
    }
    if (!seen.has(buffer)) {
      seen.add(buffer);
      transfers.push(buffer);
    }
  }
  return transfers;
}

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
  private clipPageRequestRing?: SharedClipPageRequestRingWriter;
  private externalMidiRing?: SharedExternalMidiRingWriter;
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
  // Cue-bus plane, allocated only when the host asked for a separate PFL/AFL
  // output. Empty otherwise, so a single-output host pays no heap and keeps the
  // historical behaviour where process() folds the cue into the program mix.
  private monitorBuffers: Float32Array[] = [];
  private readonly cueOutput: boolean;
  private readonly liveClips = new Map<number, EngineClip>();
  private readonly pagedClipProviders = new Map<number, number>();
  private readonly pagedClipPageFrames = new Map<number, number>();
  private readonly pendingPagedClips = new Map<number, EngineClip>();
  // The worklet drains at most this many distinct page misses per render
  // quantum. The fixed scalar buffers avoid retaining an unbounded queue on
  // the audio thread; any remaining native requests stay queued for the next
  // quantum and render silence meanwhile.
  private readonly clipPageRequestClipIds = new Float64Array(64);
  private readonly clipPageRequestPageIndices = new Float64Array(64);
  private clipPageRequestOverflowReported = 0;

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
    this.clipPageRequestRing = options.clipPageRequestSharedBuffer
      ? clipPageRequestRingFromSharedBuffer(
          options.clipPageRequestSharedBuffer,
          options.clipPageRequestRingCapacity,
        )
      : undefined;
    this.externalMidiRing = options.externalMidiSharedBuffer
      ? externalMidiRingFromSharedBuffer(
          options.externalMidiSharedBuffer,
          options.externalMidiRingCapacity,
        )
      : undefined;
    this.engine = new RealtimeEngine(
      this.sampleRate,
      this.blockSize,
      1024,
      1024,
      this.channelCount,
    );
    // Allocate persistent WASM-heap scratch (worst case: channelCount channels x
    // blockSize frames) and acquire the per-channel heap views once.
    this.engine.prepareChannels(this.channelCount, this.blockSize);
    this.channelBuffers = new Array(this.channelCount);
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.channelBuffers[ch] = this.engine.getChannelBuffer(ch, this.blockSize);
    }
    this.cueOutput = options.cueOutput === true;
    if (this.cueOutput) {
      this.engine.prepareMonitorChannels(this.channelCount, this.blockSize);
      this.monitorBuffers = new Array(this.channelCount);
      for (let ch = 0; ch < this.channelCount; ch++) {
        this.monitorBuffers[ch] = this.engine.getMonitorChannelBuffer(ch, this.blockSize);
      }
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
    if (this.cueOutput && (this.monitorBuffers[0]?.byteLength ?? 0) === 0) {
      this.reacquireMonitorBuffers();
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

    // Run the engine in place over the prepared scratch (allocation-free). The
    // monitor variant keeps the cue bus out of the program planes so it can go
    // to its own output; the plain call folds it in, as it always has.
    if (this.cueOutput) {
      this.engine.processPreparedWithMonitor(usableFrames);
    } else {
      this.engine.processPrepared(usableFrames);
    }

    copyPlanesToOutput(output, this.channelBuffers, usableFrames);
    if (this.cueOutput) {
      const cue = outputs[1];
      if (cue) {
        copyPlanesToOutput(cue, this.monitorBuffers, usableFrames);
      }
    }
    this.publishClipPageRequests();
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

  private reacquireMonitorBuffers(): void {
    for (let ch = 0; ch < this.channelCount; ch++) {
      this.monitorBuffers[ch] = this.engine.getMonitorChannelBuffer(ch, this.blockSize);
    }
  }

  receiveCommand(command: SonareEngineCommandRecord): void {
    if (!this.closed) {
      this.safeApplyCommand(command);
    }
  }

  // Applies an out-of-band control-plane sync message on the AudioWorklet
  // thread. These handlers must remain bounded: expensive clip transforms are
  // performed on the main-thread mirror, and long pre-baked PCM arrives in
  // small pages before the final lightweight clip schedule is committed.
  receiveSync(message: SonareEngineSyncMessage): void {
    if (this.closed) {
      return;
    }
    try {
      this.applySync(message);
    } catch (error) {
      this.transport?.postMessage?.({
        type: 'syncError',
        syncType: message.type,
        message: error instanceof Error ? error.message : String(error),
      });
    }
  }

  private applySync(message: SonareEngineSyncMessage): void {
    switch (message.type) {
      case 'destroy':
        this.destroy();
        return;
      case 'syncClips': {
        // Commit native state before touching the worklet-side mirrors. A
        // rejected embind call must leave a later delta based on the previous
        // clip set, not on a half-applied replacement.
        this.engine.setClips(message.clips);
        this.clearPagedClipProviders();
        this.liveClips.clear();
        for (const clip of message.clips) {
          if (clip.id !== undefined) {
            this.liveClips.set(clip.id, clip);
          }
        }
        break;
      }
      case 'syncClipsDelta': {
        const nextClips = new Map(this.liveClips);
        for (const clipId of message.removeIds) {
          nextClips.delete(clipId);
        }
        for (const clip of message.upserts) {
          if (clip.id !== undefined) {
            nextClips.set(clip.id, clip);
          }
        }
        this.engine.setClips(Array.from(nextClips.values()));
        for (const clipId of message.removeIds) {
          this.removePagedClipProvider(clipId);
        }
        this.liveClips.clear();
        for (const [clipId, clip] of nextClips) {
          this.liveClips.set(clipId, clip);
        }
        break;
      }
      case 'syncClipPageProvider': {
        const provider = this.engine.createClipPageProvider(
          message.numChannels,
          message.numSamples,
          message.pageFrames,
        );
        this.removePagedClipProvider(message.clipId);
        this.pagedClipProviders.set(message.clipId, provider.id);
        this.pagedClipPageFrames.set(message.clipId, message.pageFrames);
        if (message.clip) {
          this.pendingPagedClips.set(message.clipId, message.clip);
        }
        break;
      }
      case 'syncClipPage': {
        const providerId = this.pagedClipProviders.get(message.clipId);
        if (providerId !== undefined) {
          this.engine.supplyClipPage(providerId, message.pageIndex, message.channels);
        }
        break;
      }
      case 'syncClipPageClear': {
        const providerId = this.pagedClipProviders.get(message.clipId);
        if (providerId !== undefined) {
          this.engine.clearClipPage(providerId, message.pageIndex);
        }
        break;
      }
      case 'syncClipPageCommit': {
        const providerId = this.pagedClipProviders.get(message.clipId);
        const clip = message.clip ?? this.pendingPagedClips.get(message.clipId);
        if (providerId !== undefined && clip) {
          const nextClip = { ...clip, pageProvider: providerId };
          const nextClips = new Map(this.liveClips);
          nextClips.set(message.clipId, nextClip);
          this.engine.setClips(Array.from(nextClips.values()));
          this.liveClips.set(message.clipId, nextClip);
          this.pendingPagedClips.delete(message.clipId);
        }
        break;
      }
      case 'syncClipPageDestroy': {
        const nextClips = new Map(this.liveClips);
        nextClips.delete(message.clipId);
        this.engine.setClips(Array.from(nextClips.values()));
        this.liveClips.delete(message.clipId);
        this.removePagedClipProvider(message.clipId);
        break;
      }
      case 'syncMidiClips':
        this.engine.setMidiClips(message.clips);
        break;
      case 'syncMarkers':
        this.engine.setMarkers(message.markers);
        break;
      case 'syncMetronome':
        // Do not publish the cached config until the native engine accepted it.
        // Otherwise a rejected sync would corrupt the command-side fallback.
        {
          const config = resolveMetronomeConfig(message.config);
          this.engine.setMetronome(message.config);
          this.metronomeConfig = config;
        }
        break;
      case 'syncAutomation':
        this.engine.setAutomationLane(message.paramId, message.points);
        break;
      case 'syncParameters':
        this.engine.clearParameters();
        for (const info of message.parameters) {
          this.engine.addParameter(info);
        }
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
      case 'syncMidiUmp':
        this.engine.pushMidiUmp(message.destinationId, message.word0, message.renderFrame);
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
      case 'syncMidiInputSource':
        this.engine.setMidiInputSource(message.destinationId ?? 0);
        break;
      case 'syncClearMidiInputSource':
        this.engine.clearMidiInputSource();
        break;
      case 'syncMidiCcBinding':
        this.engine.bindMidiCc(message.channel, message.controller, message.paramId, {
          minValue: message.minValue,
          maxValue: message.maxValue,
        });
        break;
      case 'syncMidiInputNoteOn':
        this.engine.pushMidiInputNoteOn(
          message.group,
          message.channel,
          message.data0,
          message.data1,
          message.portTimeSamples,
        );
        break;
      case 'syncMidiInputNoteOff':
        this.engine.pushMidiInputNoteOff(
          message.group,
          message.channel,
          message.data0,
          message.data1,
          message.portTimeSamples,
        );
        break;
      case 'syncMidiInputCc':
        this.engine.pushMidiInputCc(
          message.group,
          message.channel,
          message.data0,
          message.data1,
          message.portTimeSamples,
        );
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
        } satisfies SonareEngineCaptureResponseMessageInternal);
        return;
      }
      if (message.op === 'read') {
        // Embind's outer val::array is array-like but not structured-cloneable
        // in every host. Copy only that tiny container; keep each native
        // Float32Array intact so the channel payload remains transferable.
        const channels = Array.from(this.engine.capturedAudio());
        const transfer = captureTransferList(channels);
        this.transport?.postMessage?.(
          {
            type: 'captureResponse',
            requestId: message.requestId,
            ok: true,
            channels,
          } satisfies SonareEngineCaptureResponseMessageInternal,
          transfer,
        );
        return;
      }
      this.engine.resetCapture();
      this.transport?.postMessage?.({
        type: 'captureResponse',
        requestId: message.requestId,
        ok: true,
      } satisfies SonareEngineCaptureResponseMessageInternal);
    } catch (error) {
      this.transport?.postMessage?.({
        type: 'captureResponse',
        requestId: message.requestId,
        ok: false,
        error: error instanceof Error ? error.message : String(error),
      } satisfies SonareEngineCaptureResponseMessageInternal);
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
      this.clearPagedClipProviders();
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
      this.safeApplyCommand(command);
    }
  }

  private safeApplyCommand(command: SonareEngineCommandRecord): void {
    try {
      this.applyCommand(command);
    } catch {
      // A malformed control command must never escape AudioWorklet.process():
      // Web Audio permanently stops invoking a processor that throws there.
      this.publishTelemetryRecord({
        type: SonareEngineTelemetryType.Error,
        error: SonareEngineTelemetryError.InvalidCommand,
        renderFrame: 0,
        timelineSample: 0,
        audibleTimelineSample: 0,
        graphLatencySamplesQ8: 0,
        value: Number(command.type),
      });
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
          clickSeconds: this.metronomeConfig.clickSeconds,
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
      case SonareEngineCommandType.SetTrackMonitorMode: {
        const rawMode = command.argInt;
        const mode = typeof rawMode === 'bigint' ? Number(rawMode) : rawMode;
        if (typeof mode !== 'number' || !Number.isSafeInteger(mode) || mode < 0 || mode > 2) {
          throw new RangeError(`Invalid track monitor mode: ${String(rawMode)}`);
        }
        const laneIndex = command.targetId;
        if (
          typeof laneIndex !== 'number' ||
          !Number.isSafeInteger(laneIndex) ||
          laneIndex < 0 ||
          laneIndex > 0xffff_ffff
        ) {
          throw new RangeError(`Invalid track monitor lane index: ${String(laneIndex)}`);
        }
        this.engine.setTrackMonitorMode(laneIndex, mode as 0 | 1 | 2, sampleTime);
        break;
      }
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
    const ring = this.telemetryRing;
    if (ring) {
      for (let count = 0; count < 64 && this.engine.popTelemetryToScratch(); count++) {
        this.writeTelemetryScratch(ring);
      }
      return;
    }
    for (const item of this.engine.drainTelemetry(64)) {
      this.publishTelemetryRecord(telemetryFromEngine(item));
    }
  }

  private writeTelemetryScratch(ring: SonareEngineTelemetryRingBuffer): void {
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * SONARE_ENGINE_TELEMETRY_RECORD_BYTES;
    ring.view.setUint32(offset, this.engine.telemetryScratchType(), true);
    ring.view.setUint32(offset + 4, this.engine.telemetryScratchError(), true);
    writeInt64Words(ring.view, offset + 8, this.engine.telemetryScratchRenderFrame());
    writeInt64Words(ring.view, offset + 16, this.engine.telemetryScratchTimelineSample());
    writeInt64Words(ring.view, offset + 24, this.engine.telemetryScratchAudibleTimelineSample());
    ring.view.setInt32(offset + 32, this.engine.telemetryScratchGraphLatencySamplesQ8(), true);
    ring.view.setUint32(offset + 36, this.engine.telemetryScratchValue(), true);
    ring.view.setUint32(offset + 40, 0, true);
    ring.view.setUint32(offset + 44, 0, true);
    Atomics.store(ring.header, 0, writeIndex + 1);
    if (writeIndex + 1 > ring.capacity) {
      Atomics.store(ring.header, 4, writeIndex + 1 - ring.capacity);
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
    if (this.meterRing) {
      for (let count = 0; count < 64 && this.engine.popMeterTelemetryToScratch(); count++) {
        const frame = this.engine.meterScratchRenderFrame();
        if (
          frame !== this.lastMeterFrame &&
          frame - this.lastMeterFrame < this.meterIntervalFrames
        ) {
          continue;
        }
        if (frame !== this.lastMeterFrame) {
          this.lastMeterFrame = frame;
        }
        this.writeMeterScratch(this.meterRing);
      }
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

  private writeMeterScratch(ring: SharedMeterRingWriter): void {
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * SONARE_METER_RING_RECORD_FLOATS;
    const frame = Number(this.engine.meterScratchRenderFrame());
    ring.records[offset] = encodeFrameLo(frame);
    ring.records[offset + 1] = encodeFrameHi(frame);
    ring.records[offset + 2] = this.engine.meterScratchTargetId();
    for (let field = 0; field < 11; field++) {
      ring.records[offset + 3 + field] = this.engine.meterScratchValue(field);
    }
    Atomics.store(ring.header, 0, writeIndex + 1);
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
    for (let count = 0; count < 64 && this.engine.popScopeTelemetryToScratch(); count++) {
      this.writeScopeScratch(ring);
    }
  }

  private writeScopeScratch(ring: SharedScopeRingWriter): void {
    const writeIndex = Atomics.load(ring.header, 0);
    const base = (writeIndex % ring.capacity) * ring.recordFloats;
    const frame = Number(this.engine.scopeScratchRenderFrame());
    ring.records[base] = encodeFrameLo(frame);
    ring.records[base + 1] = encodeFrameHi(frame);
    ring.records[base + 2] = this.engine.scopeScratchTargetId();
    const bandCount = Math.min(ring.bands, this.engine.scopeScratchBandCount());
    ring.records[base + 3] = bandCount;
    const pointCount = Math.min(ring.maxPoints, this.engine.scopeScratchPointCount());
    ring.records[base + 4] = pointCount;
    const bandsBase = base + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS;
    for (let i = 0; i < bandCount; i++) {
      ring.records[bandsBase + i] = this.engine.scopeScratchBand(i);
    }
    const pointsBase = bandsBase + ring.bands;
    for (let i = 0; i < pointCount; i++) {
      ring.records[pointsBase + 2 * i] = this.engine.scopeScratchPointLeft(i);
      ring.records[pointsBase + 2 * i + 1] = this.engine.scopeScratchPointRight(i);
    }
    Atomics.store(ring.header, 0, writeIndex + 1);
  }

  // Drains queued external-MIDI events (already lowered to MIDI 1.0 bytes) and
  // forwards them to the main thread for delivery to Web MIDI output ports.
  // One batch per render block; skipped entirely when nothing is queued, so an
  // all-internal project never allocates or posts here.
  private publishExternalMidi(): void {
    const ring = this.externalMidiRing;
    if (ring) {
      for (let count = 0; count < 256 && this.engine.popExternalMidiToScratch(); count++) {
        pushSonareExternalMidiRingBuffer(
          ring,
          this.engine.externalMidiScratchDestinationId(),
          this.engine.externalMidiScratchRenderFrame(),
          this.engine.externalMidiScratchByteWord(),
          this.engine.externalMidiScratchByteCount(),
        );
        this.engine.consumeExternalMidiScratch();
      }
      return;
    }
    if (!this.transport?.postMessage) {
      return;
    }
    if (this.engine.externalMidiPendingCount() === 0) {
      return;
    }
    const events = this.engine.drainExternalMidi(256);
    if (events.length === 0) {
      return;
    }
    this.transport.postMessage({ type: 'externalMidi', events });
  }

  /**
   * Forward a bounded batch of native page misses to the main thread. This
   * deliberately runs after audio rendering: a cache miss is silence for this
   * block, and OPFS I/O must never delay `process()`.
   */
  private publishClipPageRequests(): void {
    const ring = this.clipPageRequestRing;
    const transport = this.transport;
    if (!ring && !transport?.postMessage) {
      return;
    }
    let count = 0;
    let drained = 0;
    while (drained < this.clipPageRequestClipIds.length) {
      drained += 1;
      let clipId: number;
      let sample: number;
      if (ring) {
        // This scalar scratch API is intentionally used only on the SAB path:
        // it avoids embind materialising one JS object for every page miss in
        // AudioWorklet process().
        if (!this.engine.popClipPageRequestToScratch()) {
          break;
        }
        clipId = this.engine.clipPageRequestScratchClipId();
        sample = this.engine.clipPageRequestScratchSample();
      } else {
        // postMessage fallback is explicitly non-RT-safe; retain the public
        // object-returning API only for that degraded control-plane path.
        const request = this.engine.popClipPageRequest();
        if (!request) {
          break;
        }
        clipId = request.clipId;
        sample = request.sample;
      }
      const pageFrames = this.pagedClipPageFrames.get(clipId);
      if (!pageFrames) {
        continue;
      }
      const pageIndex = Math.floor(sample / pageFrames);
      let duplicate = false;
      for (let i = 0; i < count; ++i) {
        if (
          this.clipPageRequestClipIds[i] === clipId &&
          this.clipPageRequestPageIndices[i] === pageIndex
        ) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        this.clipPageRequestClipIds[count] = clipId;
        this.clipPageRequestPageIndices[count] = pageIndex;
        count += 1;
      }
    }
    const overflowCount = this.engine.clipPageRequestOverflowCount();
    // The native counter is uint32_t; preserve a correct delta across its
    // defined unsigned wraparound instead of emitting a negative count.
    const dropped = (overflowCount - this.clipPageRequestOverflowReported) >>> 0;
    this.clipPageRequestOverflowReported = overflowCount;
    if (ring) {
      // Preserve native-queue loss in the same cumulative counter used for a
      // full SAB request ring. No object, structured clone, or postMessage is
      // created on this path.
      if (dropped > 0) {
        Atomics.add(ring.header, 4, dropped);
      }
      for (let i = 0; i < count; ++i) {
        pushSonareClipPageRequestRingBuffer(
          ring,
          this.clipPageRequestClipIds[i],
          this.clipPageRequestPageIndices[i],
        );
      }
      return;
    }
    if (count === 0 && dropped === 0) {
      return;
    }
    if (!transport?.postMessage) {
      return;
    }
    // A message is necessarily allocated by postMessage; the hot path's
    // resident queue remains fixed-size and no message is built without a miss.
    const requests = new Array<{ clipId: number; pageIndex: number }>(count);
    for (let i = 0; i < count; ++i) {
      requests[i] = {
        clipId: this.clipPageRequestClipIds[i],
        pageIndex: this.clipPageRequestPageIndices[i],
      };
    }
    transport.postMessage({
      type: 'clipPageRequest',
      requests,
      ...(dropped > 0 ? { dropped } : {}),
    });
  }

  private removePagedClipProvider(clipId: number): void {
    const providerId = this.pagedClipProviders.get(clipId);
    if (providerId !== undefined) {
      this.engine.destroyClipPageProvider(providerId);
    }
    this.pagedClipProviders.delete(clipId);
    this.pagedClipPageFrames.delete(clipId);
    this.pendingPagedClips.delete(clipId);
  }

  private clearPagedClipProviders(): void {
    for (const clipId of this.pagedClipProviders.keys()) {
      this.removePagedClipProvider(clipId);
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
