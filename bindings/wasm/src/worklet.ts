import { panLawCode, panModeCode } from './codes';
import type {
  EngineAutomationPoint,
  EngineBus,
  EngineCaptureStatus,
  EngineClip,
  EngineMarker,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineScopeTelemetry,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackSend,
  EngineTransportState,
  EqBand,
  MixerRealtimeBuffer,
  PanLaw,
  PanMode,
} from './index';
import {
  engineCapabilities,
  init as initSonareModule,
  isInitialized,
  Mixer,
  RealtimeEngine,
  RealtimeVoiceChanger,
} from './index';
import type { SonareModule } from './sonare.js';
import type { SonareRtModule } from './sonare-rt';

// With code-splitting disabled, the worklet bundle carries its own copy of the
// module singleton (a real AudioWorkletGlobalScope cannot resolve sibling
// chunks, so the bundle must be self-contained). Re-export the lifecycle so that
// realm can initialize its own wasm instance, independent of the main-thread
// `index` module.
export { init, isInitialized } from './index';

import type { WorkletInput, WorkletOutput } from './worklet/audio_types';
import {
  isEngineCaptureRequestMessage,
  isEngineCaptureResponseMessage,
  isEngineCommandRecord,
  isEngineSyncMessage,
  isEngineTelemetryRecord,
  isEngineTransportRequestMessage,
  isEngineTransportResponseMessage,
  isMeterSnapshot,
  isRealtimeVoiceChangerMessage,
  isWorkletMessage,
} from './worklet/guards';
import {
  DEFAULT_METRONOME_CONFIG,
  type ResolvedMetronomeConfig,
  resolveMetronomeConfig,
  type SonareEngineCaptureRequestMessage,
  type SonareEngineCaptureResponseMessage,
  type SonareEngineInstrumentSyncMessage,
  type SonareEngineSyncCaptureMessage,
  type SonareEngineSyncMessage,
  type SonareEngineTransportFacade,
  type SonareEngineTransportRequestMessage,
  type SonareEngineTransportResponseMessage,
  type SonareRealtimeEngineNodeCapabilities,
  type SonareRealtimeEngineNodeOptions,
  type SonareRealtimeEngineWorkletProcessorOptions,
  type SonareRealtimeVoiceChangerMessage,
  type SonareRealtimeVoiceChangerWorkletProcessorOptions,
  type SonareRtRealtimeEngineRuntimeOptions,
  type SonareWorkletMessage,
  type SonareWorkletProcessorOptions,
  type WorkletPort,
  type WorkletTransport,
} from './worklet/messages';
// --- internal modules (split out of this file; bundled back into a single
// dist/worklet.js by tsup, so the public surface is unchanged) ---
import {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  ENGINE_MIXER_PARAM_FADER_DB,
  ENGINE_MIXER_PARAM_PAN,
  encodeFrameHi,
  encodeFrameLo,
  engineMixerBusTarget,
  engineMixerLaneTarget,
  engineMixerMasterTarget,
  engineRingFromSharedBuffer,
  isRecord,
  magnitudeToDb,
  meterFromEngine,
  meterRingFromSharedBuffer,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  type SharedMeterRingWriter,
  type SharedScopeRingWriter,
  type SharedSpectrumRingWriter,
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
  type SonareMeterRingBuffer,
  type SonareScopeRingBuffer,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletScopeSnapshot,
  type SonareWorkletSpectrumSnapshot,
  scopeRingFromSharedBuffer,
  spectrumRingFromSharedBuffer,
  telemetryFromEngine,
  toBigInt64,
  toDb,
  writeSonareEngineTelemetryRingBuffer,
} from './worklet/protocol';

export type {
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessage,
  SonareEngineSyncAutomationMessage,
  SonareEngineSyncBuiltinInstrumentMessage,
  SonareEngineSyncCaptureMessage,
  SonareEngineSyncClipsDeltaMessage,
  SonareEngineSyncClipsMessage,
  SonareEngineSyncLoadSoundFontMessage,
  SonareEngineSyncMarkersMessage,
  SonareEngineSyncMasterStripEqBandMessage,
  SonareEngineSyncMasterStripInsertBypassedMessage,
  SonareEngineSyncMessage,
  SonareEngineSyncMetronomeMessage,
  SonareEngineSyncMidiCcMessage,
  SonareEngineSyncMidiClipsMessage,
  SonareEngineSyncMidiNoteMessage,
  SonareEngineSyncMidiPanicMessage,
  SonareEngineSyncMixerMessage,
  SonareEngineSyncSf2InstrumentMessage,
  SonareEngineSyncSynthInstrumentMessage,
  SonareEngineSyncTempoMessage,
  SonareEngineSyncTrackStripEqBandMessage,
  SonareEngineSyncTrackStripInsertBypassedMessage,
  SonareEngineTransportFacade,
  SonareEngineTransportRequestMessage,
  SonareEngineTransportResponseMessage,
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareRealtimeVoiceChangerDestroyMessage,
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerResetMessage,
  SonareRealtimeVoiceChangerSetConfigMessage,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  SonareRtRealtimeEngineRuntimeOptions,
  SonareWorkletDestroyMessage,
  SonareWorkletMessage,
  SonareWorkletProcessorOptions,
  SonareWorkletScheduleInsertAutomationMessage,
  SonareWorkletSetMeterIntervalMessage,
  SonareWorkletTransportMessage,
} from './worklet/messages';
export {
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  createSonareSpectrumRingBuffer,
  decodeFrame,
  encodeFrameHi,
  encodeFrameLo,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  readSonareSpectrumRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  type SonareEngineTelemetryRecord,
  type SonareEngineTelemetryRingBuffer,
  type SonareEngineTelemetryRingReadResult,
  SonareEngineTelemetryType,
  type SonareMeterRingBuffer,
  type SonareMeterRingReadResult,
  type SonareScopeRingBuffer,
  type SonareScopeRingReadResult,
  type SonareSpectrumRingBuffer,
  type SonareSpectrumRingReadResult,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletScopeSnapshot,
  type SonareWorkletSpectrumSnapshot,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  sonareScopeRingBufferByteLength,
  sonareSpectrumRingBufferByteLength,
  writeSonareEngineTelemetryRingBuffer,
} from './worklet/protocol';

export interface SonareEngineOptions extends SonareRealtimeEngineNodeOptions {
  offlineEngine?: RealtimeEngine;
  offlineBlockSize?: number;
  offlineChannelCount?: number;
}

type SuspendableAudioContext = BaseAudioContext & {
  suspend?: () => Promise<void>;
  resume?: () => Promise<void>;
};

/**
 * AudioWorklet-style mixer bridge backed by the package's single `sonare.wasm`.
 *
 * The WASM module must already be initialized via `init()` before constructing
 * this bridge. Each AudioWorklet input is treated as one stereo strip:
 * `inputs[strip][0]` is left and `inputs[strip][1]` is right. Missing channels
 * are replaced with preallocated silence.
 */
export class SonareWorkletProcessor {
  readonly sampleRate: number;
  readonly blockSize: number;
  private mixer: Mixer;
  private realtime: MixerRealtimeBuffer;
  private closed = false;
  private processedFrames = 0;
  private lastMeterFrame = 0;
  private meterIntervalFrames: number;
  private spectrumIntervalFrames: number;
  private lastSpectrumFrame = 0;
  private transport?: WorkletTransport;
  private meterRing?: SharedMeterRingWriter;
  private spectrumRing?: SharedSpectrumRingWriter;
  private spectrumBands: Float32Array;

  constructor(options: SonareWorkletProcessorOptions, transport?: WorkletTransport) {
    if (!options.sceneJson) {
      throw new Error('sceneJson is required.');
    }
    this.sampleRate = options.sampleRate ?? 48000;
    this.blockSize = options.blockSize ?? 128;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.spectrumIntervalFrames = Math.max(0, Math.floor(options.spectrumIntervalFrames ?? 0));
    this.transport = transport;
    this.meterIntervalFrames = Math.max(0, Math.floor(options.meterIntervalFrames ?? 2048));
    this.meterRing = options.meterSharedBuffer
      ? meterRingFromSharedBuffer(options.meterSharedBuffer, options.meterRingCapacity)
      : undefined;
    this.spectrumRing = options.spectrumSharedBuffer
      ? spectrumRingFromSharedBuffer(
          options.spectrumSharedBuffer,
          options.spectrumRingCapacity,
          options.spectrumBands,
        )
      : undefined;
    const spectrumBandCount = this.spectrumRing?.bands ?? Math.max(1, options.spectrumBands ?? 16);
    this.spectrumBands = new Float32Array(spectrumBandCount);
    this.mixer = Mixer.fromSceneJson(options.sceneJson, this.sampleRate, this.blockSize);
    this.mixer.compile();
    const sceneStripCount = this.mixer.stripCount();
    const stripCount = options.stripCount ?? sceneStripCount;
    if (stripCount !== sceneStripCount) {
      throw new Error('stripCount must match the scene strip count.');
    }
    this.realtime = this.mixer.createRealtimeBuffer();
  }

  process(inputs: WorkletInput, outputs: WorkletOutput): boolean {
    if (this.closed) {
      return false;
    }
    const output = outputs[0];
    const leftOut = output?.[0];
    const rightOut = output?.[1];
    if (!leftOut) {
      return true;
    }
    const frames = leftOut.length;
    // The mixer's realtime heap buffers are sized to blockSize. A render quantum
    // that differs from blockSize (e.g. a future browser using a quantum other
    // than 128, or a misconfigured blockSize) must NOT return false here:
    // returning false permanently terminates the AudioWorkletProcessor and
    // silently kills the node mid-stream. Instead degrade gracefully by
    // processing min(frames, blockSize) and zero-filling any remainder, mirroring
    // the sonare-rt processor's behaviour.
    const usable = Math.min(frames, this.blockSize);

    for (let strip = 0; strip < this.realtime.leftInputs.length; strip++) {
      const input = inputs[strip];
      const left = input?.[0];
      const right = input?.[1];
      const leftTarget = this.realtime.leftInputs[strip];
      const rightTarget = this.realtime.rightInputs[strip];
      if (left && left.length >= usable) {
        leftTarget.set(left.subarray(0, usable));
        if (right && right.length >= usable) {
          rightTarget.set(right.subarray(0, usable));
        } else {
          rightTarget.set(left.subarray(0, usable));
        }
      } else {
        leftTarget.fill(0);
        rightTarget.fill(0);
      }
    }

    this.realtime.process(usable);
    if (usable === frames) {
      leftOut.set(this.realtime.outLeft.subarray(0, usable));
      if (rightOut) {
        rightOut.set(this.realtime.outRight.subarray(0, usable));
      }
    } else {
      // frames > blockSize: fill the produced part and zero the remaining tail.
      leftOut.fill(0);
      leftOut.set(this.realtime.outLeft.subarray(0, usable));
      if (rightOut) {
        rightOut.fill(0);
        rightOut.set(this.realtime.outRight.subarray(0, usable));
      }
    }
    this.processedFrames += usable;
    this.publishMeter(
      this.realtime.outLeft.subarray(0, usable),
      this.realtime.outRight.subarray(0, usable),
    );
    this.publishSpectrum(
      this.realtime.outLeft.subarray(0, usable),
      this.realtime.outRight.subarray(0, usable),
    );
    return true;
  }

  receiveMessage(message: SonareWorkletMessage): void {
    if (this.closed) {
      return;
    }
    if (message.type === 'destroy') {
      this.destroy();
      return;
    }
    if (message.type === 'setMeterInterval') {
      this.meterIntervalFrames = Math.max(0, Math.floor(message.frames));
      return;
    }
    if (message.type === 'scheduleInsertAutomation') {
      this.mixer.scheduleInsertAutomation(
        message.stripIndex,
        message.insertIndex,
        message.paramId,
        message.samplePos ?? this.processedFrames,
        message.value,
        message.curve ?? 'linear',
      );
    }
  }

  destroy(): void {
    if (!this.closed) {
      this.mixer.delete();
      this.closed = true;
    }
  }

  private publishMeter(left: Float32Array, right: Float32Array): void {
    if (!this.transport || this.meterIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastMeterFrame < this.meterIntervalFrames) {
      return;
    }
    this.lastMeterFrame = this.processedFrames;

    let peakL = 0;
    let peakR = 0;
    let sumL = 0;
    let sumR = 0;
    let sumLR = 0;
    for (let i = 0; i < left.length; i++) {
      const l = left[i] ?? 0;
      const r = right[i] ?? 0;
      const absL = Math.abs(l);
      const absR = Math.abs(r);
      if (absL > peakL) {
        peakL = absL;
      }
      if (absR > peakR) {
        peakR = absR;
      }
      sumL += l * l;
      sumR += r * r;
      sumLR += l * r;
    }
    const rmsL = Math.sqrt(sumL / Math.max(1, left.length));
    const rmsR = Math.sqrt(sumR / Math.max(1, right.length));
    const denominator = Math.sqrt(sumL * sumR);
    const meter: SonareWorkletMeterSnapshot = {
      type: 'meter',
      targetId: 0,
      frame: this.processedFrames,
      peakDbL: toDb(peakL),
      peakDbR: toDb(peakR),
      rmsDbL: toDb(rmsL),
      rmsDbR: toDb(rmsR),
      correlation: denominator > 0 ? sumLR / denominator : 0,
      truePeakDbL: toDb(peakL),
      truePeakDbR: toDb(peakR),
      momentaryLufs: Number.NaN,
      shortTermLufs: Number.NaN,
      integratedLufs: Number.NaN,
      gainReductionDb: Number.NaN,
    };
    this.transport.onMeter?.(meter);
    if (this.meterRing) {
      this.writeMeterRing(meter);
    } else {
      this.transport.postMessage?.(meter);
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

  private publishSpectrum(left: Float32Array, right: Float32Array): void {
    if (this.spectrumIntervalFrames <= 0) {
      return;
    }
    if (this.processedFrames - this.lastSpectrumFrame < this.spectrumIntervalFrames) {
      return;
    }
    this.lastSpectrumFrame = this.processedFrames;
    this.computeSpectrum(left, right);
    if (this.spectrumRing) {
      this.writeSpectrumRing(this.processedFrames, this.spectrumBands);
      return;
    }
    const spectrum: SonareWorkletSpectrumSnapshot = {
      type: 'spectrum',
      frame: this.processedFrames,
      bands: new Float32Array(this.spectrumBands),
    };
    this.transport?.onSpectrum?.(spectrum);
    this.transport?.postMessage?.(spectrum);
  }

  private computeSpectrum(left: Float32Array, right: Float32Array): void {
    // Coarse per-render-quantum band energy, NOT a full FFT analyzer: each band
    // is a single-bin DFT (bin = band + 1) evaluated over the current block of n
    // samples. Bins at or above the block Nyquist (band + 1 > floor(n / 2))
    // alias, so the evaluated band count is clamped to floor(n / 2) and any
    // higher bands are pinned to the silence floor. Bin resolution is therefore
    // tied to the render quantum (typically 128 samples); treat the output as a
    // rough spectral tilt, not a precise spectrum.
    const n = Math.max(1, Math.min(left.length, right.length));
    const maxBand = Math.floor(n / 2);
    for (let band = 0; band < this.spectrumBands.length; band++) {
      if (band >= maxBand) {
        this.spectrumBands[band] = magnitudeToDb(0);
        continue;
      }
      const bin = band + 1;
      let real = 0;
      let imag = 0;
      for (let i = 0; i < n; i++) {
        const sample = 0.5 * ((left[i] ?? 0) + (right[i] ?? 0));
        const phase = (-2 * Math.PI * bin * i) / n;
        real += sample * Math.cos(phase);
        imag += sample * Math.sin(phase);
      }
      this.spectrumBands[band] = magnitudeToDb((2 * Math.hypot(real, imag)) / n);
    }
  }

  private writeSpectrumRing(frame: number, bands: Float32Array): void {
    const ring = this.spectrumRing;
    if (!ring) {
      return;
    }
    const writeIndex = Atomics.load(ring.header, 0);
    const offset = (writeIndex % ring.capacity) * ring.recordFloats;
    ring.records[offset] = encodeFrameLo(frame);
    ring.records[offset + 1] = encodeFrameHi(frame);
    ring.records[offset + 2] = bands.length;
    ring.records.set(bands.subarray(0, ring.bands), offset + 3);
    Atomics.store(ring.header, 0, writeIndex + 1);
    // See writeMeterRing: header slot 4 (the spectrum-ring overflow slot) is
    // left at its initial 0; readers detect silent overrun via the
    // firstReadable = max(readIndex, writeIndex - capacity) clamp. (Slot 3 here
    // holds the band count and is still written at ring creation.)
  }
}

/**
 * AudioWorklet-style bridge for the DAW realtime engine facade.
 *
 * The default mode uses the existing `sonare.wasm` embind facade. The
 * `sonare-rt` target is exposed as a selectable runtime target for hosts that
 * load the dedicated Emscripten AudioWorklet module.
 */
export class SonareRealtimeEngineWorkletProcessor {
  private static warnedChannelScratchOverflow = false;
  readonly sampleRate: number;
  readonly blockSize: number;
  readonly channelCount: number;
  readonly runtimeTarget: 'embind' | 'sonare-rt';
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
    this.runtimeTarget = options.runtimeTarget ?? 'embind';
    if (this.runtimeTarget === 'sonare-rt') {
      throw new Error(
        'sonare-rt runtime is provided by the dedicated Emscripten AudioWorklet module; use SonareRealtimeEngineNode.create({ runtimeTarget: "sonare-rt", moduleUrl: ... }) to load it.',
      );
    }
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
      case 'syncMidiPanic':
        this.engine.pushMidiPanic(message.renderFrame);
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
  // the lock-free SAB scope ring. Only the embind runtime publishes scope
  // telemetry; the sonare-rt runtime owns its own transport. No allocation on
  // the render path: records are written field-by-field into the ring.
  private publishScope(): void {
    const ring = this.scopeRing;
    if (!ring) {
      return;
    }
    for (const item of this.engine.drainScopeTelemetry(64)) {
      this.writeScopeRing(ring, item);
    }
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
    const runtimeTarget = options.runtimeTarget ?? 'embind';
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
        : runtimeTarget === 'embind'
          ? engineCapabilities()
          : undefined;
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
    // Meter ring: only the embind runtime publishes engine meters into a SAB
    // ring (the sonare-rt runtime owns its own meter transport). Lock-free
    // meter delivery matches the telemetry path and keeps the audio render
    // callback allocation-free in SAB mode.
    const meterRing =
      mode === 'sab' && runtimeTarget === 'embind'
        ? createSonareMeterRingBuffer(options.meterRingCapacity ?? 128)
        : undefined;
    // Scope ring (FFT spectrum + goniometer): opt-in, embind-only. The
    // per-block FFT is heavier than the meter path, so it is created only when
    // the caller requests scope telemetry via scopeIntervalFrames > 0.
    const scopeIntervalFrames = Math.max(0, Math.floor(options.scopeIntervalFrames ?? 0));
    const scopeRing =
      mode === 'sab' && runtimeTarget === 'embind' && scopeIntervalFrames > 0
        ? createSonareScopeRingBuffer(options.scopeRingCapacity ?? 64, options.scopeBands ?? 48)
        : undefined;
    const channelCount = Math.max(1, Math.floor(options.channelCount ?? 2));
    const processorOptions: SonareRealtimeEngineWorkletProcessorOptions = {
      runtimeTarget,
      rtModuleUrl: options.rtModuleUrl,
      rtWasmBinary: options.rtWasmBinary,
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
        runtimeTarget,
        sharedArrayBuffer,
        atomics,
        audioWorklet,
        engineAbiVersion: detectedCapabilities?.engineAbiVersion,
        expectedEngineAbiVersion: detectedCapabilities?.expectedEngineAbiVersion,
        abiCompatible: detectedCapabilities?.abiCompatible,
        degradedReason,
        readyMessage:
          runtimeTarget === 'sonare-rt' ||
          (runtimeTarget === 'embind' && moduleUrl !== undefined && !options.nodeFactory),
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

export class SonareEngine {
  readonly node: AudioWorkletNode;
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  readonly transport: SonareEngineTransportFacade;
  private readonly realtimeNode: SonareRealtimeEngineNode;
  private readonly offlineEngine: RealtimeEngine;
  private readonly context: SuspendableAudioContext;
  private readonly sampleRate: number;
  private readonly offlineBlockSize: number;
  private readonly offlineChannelCount: number;
  private readonly automationLanes = new Map<number, EngineAutomationPoint[]>();
  private readonly clips = new Map<number, EngineClip>();
  private readonly midiClips = new Map<number, EngineMidiClipSchedule>();
  private readonly markers = new Map<number, EngineMarker>();
  private readonly trackLaneIds: number[] = [];
  private readonly trackSends = new Map<number, EngineTrackSend[]>();
  private readonly trackOutputBus = new Map<number, number>();
  private readonly laneSidechains = new Map<
    string,
    { trackId: number; insertIndex: number; sourceTrackId: number }
  >();
  private readonly buses: EngineBus[] = [];
  private readonly trackStripJson = new Map<number, string>();
  private readonly busStripJson = new Map<number, string>();
  private masterStripJson: string | undefined;
  private captureConfig: Omit<SonareEngineSyncCaptureMessage, 'type'> | undefined;
  private tempoBpm = 120;
  private timeSignature = { numerator: 4, denominator: 4 };
  private tempoSegments: EngineTempoSegment[] = [{ startPpq: 0, bpm: 120 }];
  private timeSignatureSegments: EngineTimeSignatureSegment[] = [
    { startPpq: 0, numerator: 4, denominator: 4 },
  ];
  private latestTransportState: EngineTransportState | undefined;
  private nextClipId = 1;
  private nextMarkerId = 1;
  private transportPlaying = false;
  private readonly pendingInstrumentSync: SonareEngineInstrumentSyncMessage[] = [];
  private destroyed = false;

  private constructor(
    context: BaseAudioContext,
    realtimeNode: SonareRealtimeEngineNode,
    offlineEngine: RealtimeEngine,
    sampleRate: number,
    offlineBlockSize: number,
    offlineChannelCount: number,
  ) {
    this.context = context;
    this.realtimeNode = realtimeNode;
    this.offlineEngine = offlineEngine;
    this.node = realtimeNode.node;
    this.capabilities = realtimeNode.capabilities;
    this.sampleRate = sampleRate;
    this.offlineBlockSize = offlineBlockSize;
    this.offlineChannelCount = offlineChannelCount;
    this.transport = {
      play: (sampleTime = -1) => {
        const ok = this.realtimeNode.play(sampleTime);
        if (ok) {
          this.transportPlaying = true;
        }
        return ok;
      },
      stop: (sampleTime = -1) => {
        const ok = this.realtimeNode.stop(sampleTime);
        if (ok) {
          this.transportPlaying = false;
          this.flushPendingInstrumentSync();
        }
        return ok;
      },
      seekPpq: (ppq, sampleTime = -1) => {
        this.offlineEngine.seekPpq(ppq, sampleTime);
        return this.realtimeNode.seekPpq(ppq, sampleTime);
      },
      seekSeconds: (seconds, sampleTime = -1) => {
        const timelineSample = Math.max(0, Math.round(seconds * this.sampleRate));
        this.offlineEngine.seekSample(timelineSample, sampleTime);
        return this.realtimeNode.seekSample(timelineSample, sampleTime);
      },
      setTempo: (bpm) => this.setTempo(bpm),
      setTempoSegments: (segments) => this.setTempoSegments(segments),
      setLoop: (startPpq, endPpq, enabled = true) => this.setLoop(startPpq, endPpq, enabled),
    };
  }

  static async create(
    context: BaseAudioContext,
    options: SonareEngineOptions = {},
  ): Promise<SonareEngine> {
    const sampleRate = options.sampleRate ?? context.sampleRate;
    const blockSize = options.offlineBlockSize ?? options.blockSize ?? 128;
    const channelCount = Math.max(
      1,
      Math.floor(options.offlineChannelCount ?? options.channelCount ?? 2),
    );
    const realtimeNode = await SonareRealtimeEngineNode.create(context, options);
    const offlineEngine = options.offlineEngine ?? new RealtimeEngine(sampleRate, blockSize);
    return new SonareEngine(
      context,
      realtimeNode,
      offlineEngine,
      sampleRate,
      blockSize,
      channelCount,
    );
  }

  async suspend(): Promise<void> {
    if (this.destroyed) {
      return;
    }
    await this.context.suspend?.();
  }

  async resume(): Promise<void> {
    if (this.destroyed) {
      return;
    }
    await this.context.resume?.();
  }

  setTempo(bpm: number): void {
    this.tempoBpm = bpm;
    this.tempoSegments = [{ startPpq: 0, bpm }];
    this.offlineEngine.setTempo(bpm);
    this.postTempoSync();
    this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetTempoMap,
      sampleTime: -1,
      argFloat: bpm,
    });
  }

  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    this.tempoSegments = segments.map((segment) => ({ ...segment }));
    this.tempoBpm = this.tempoSegments[0]?.bpm ?? this.tempoBpm;
    this.offlineEngine.setTempoSegments(this.tempoSegments);
    this.postTempoSync();
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.timeSignature = { numerator, denominator };
    this.timeSignatureSegments = [{ startPpq: 0, numerator, denominator }];
    this.offlineEngine.setTimeSignature(numerator, denominator);
    this.postTempoSync();
  }

  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    this.timeSignatureSegments = segments.map((segment) => ({ ...segment }));
    const first = this.timeSignatureSegments[0];
    if (first) {
      this.timeSignature = { numerator: first.numerator, denominator: first.denominator };
    }
    this.offlineEngine.setTimeSignatureSegments(this.timeSignatureSegments);
    this.postTempoSync();
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): boolean {
    this.offlineEngine.setLoop(startPpq, endPpq, enabled);
    // Transport precision contract: the SAB command record carries exactly one
    // Float64 lane (argFloat) and one Int64 lane (argInt). startPpq travels in
    // argFloat with full double precision, matching the offline engine; endPpq
    // is carried as micro-PPQ (round(endPpq * 1e6)) in the integer lane and
    // divided back by 1e6 on the consumer. Loop ENDS are therefore snapped to
    // the nearest 1e-6 PPQ over the realtime transport (max 5e-7 PPQ drift),
    // while loop STARTS and the offline path stay exact. This is intentional:
    // the record has no second free Float64 lane, and a micro-PPQ grid on the
    // loop end is well below audible/sample-accurate resolution at any tempo.
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetLoop,
      targetId: enabled ? 1 : 0,
      sampleTime: -1,
      argFloat: startPpq,
      argInt: Math.round(endPpq * 1_000_000),
    });
  }

  countInEndSample(startSample: number, bars: number): number {
    return this.offlineEngine.countInEndSample(startSample, bars);
  }

  async getTransportState(): Promise<EngineTransportState> {
    const state = await this.realtimeNode.requestTransportState();
    this.latestTransportState = state;
    return state;
  }

  cachedTransportState(): EngineTransportState | undefined {
    return this.latestTransportState;
  }

  setParam(nodeId: string, param: string | number, value: number): boolean {
    const paramId = this.resolveParamId(nodeId, param);
    // Mirror the change into the offline engine so a subsequent offline render
    // reflects the live value, then push a sample-accurate command to the
    // realtime runtime (mirrors setTempo/setLoop above).
    this.offlineEngine.setParameter(paramId, value);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetParam,
      targetId: paramId,
      sampleTime: -1,
      argFloat: value,
    });
  }

  scheduleParam(
    nodeId: string,
    param: string | number,
    ppq: number,
    value: number,
    curve: number | 'linear' | 'exponential' = 'linear',
  ): void {
    const paramId = this.resolveParamId(nodeId, param);
    const lane = this.automationLanes.get(paramId) ?? [];
    lane.push({ ppq, value, curveToNext: this.curveCode(curve) });
    lane.sort((a, b) => a.ppq - b.ppq);
    this.automationLanes.set(paramId, lane);
    this.offlineEngine.setAutomationLane(paramId, lane);
    // Mirror the lane to the live worklet engine so scheduled automation plays
    // back in real time, not just in renderOffline(). Lanes can exceed the
    // fixed-size SAB command record, so they ride an out-of-band 'syncAutomation'
    // message applied outside process() (like syncClips/syncMarkers).
    this.postSync({ type: 'syncAutomation', paramId, points: lane });
  }

  addAutomationPoint(
    laneId: string | number,
    ppq: number,
    value: number,
    curve: number | 'linear' | 'exponential' = 'linear',
  ): void {
    this.scheduleParam('', laneId, ppq, value, curve);
  }

  /**
   * Replaces the automation lane for `paramId` with the given breakpoints.
   *
   * Unlike scheduleParam (which appends a single point), this sets the whole
   * lane at once; an empty array clears the lane. The points are defensively
   * copied and sorted by ppq before being mirrored to the offline engine and
   * the live worklet engine.
   *
   * @param paramId Automation target id (registered parameter or a reserved
   *   engine mixer target from automationParamId/busAutomationParamId).
   * @param points Lane breakpoints; order does not matter.
   */
  setAutomationLane(paramId: number, points: ReadonlyArray<EngineAutomationPoint>): void {
    const sorted = points.map((point) => ({ ...point })).sort((a, b) => a.ppq - b.ppq);
    if (sorted.length === 0) {
      this.automationLanes.delete(paramId);
    } else {
      this.automationLanes.set(paramId, sorted);
    }
    this.offlineEngine.setAutomationLane(paramId, sorted);
    this.postSync({ type: 'syncAutomation', paramId, points: sorted });
  }

  /**
   * Returns the automation target id for a mixer strip parameter.
   *
   * The id addresses the engine's reserved mixer namespace, so it can be fed
   * straight to setAutomationLane to automate a fader or pan without
   * registering a parameter.
   *
   * @param target Track id (declares a mixer lane on first use) or 'master'.
   * @param kind Strip parameter to address.
   * @returns Reserved engine parameter id for the strip parameter.
   */
  automationParamId(target: string | number, kind: 'faderDb' | 'pan'): number {
    const paramKind = kind === 'pan' ? ENGINE_MIXER_PARAM_PAN : ENGINE_MIXER_PARAM_FADER_DB;
    if (target === 'master') {
      return engineMixerMasterTarget(paramKind);
    }
    return engineMixerLaneTarget(this.ensureTrackLane(target), paramKind);
  }

  /**
   * Returns the automation target id for a bus fader.
   *
   * @param busId Bus id (declares the mixer bus on first use).
   * @returns Reserved engine parameter id for the bus fader gain (dB).
   */
  busAutomationParamId(busId: number): number {
    return engineMixerBusTarget(this.ensureBus(busId), ENGINE_MIXER_PARAM_FADER_DB);
  }

  /**
   * Returns the number of automation lanes installed on the engine, including
   * lanes whose breakpoint list is currently empty.
   *
   * @returns Engine-side automation lane count.
   */
  automationLaneCount(): number {
    return this.offlineEngine.automationLaneCount();
  }

  listParameters(): EngineParameterInfo[] {
    const parameters: EngineParameterInfo[] = [];
    for (let index = 0; index < this.offlineEngine.parameterCount(); index++) {
      parameters.push(this.offlineEngine.parameterInfoByIndex(index));
    }
    return parameters;
  }

  setSoloMute(target: string | number, solo: boolean, mute: boolean): boolean {
    const laneIndex = this.ensureTrackLane(target);
    this.offlineEngine.setSoloMute(laneIndex, solo, mute);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetSoloMute,
      targetId: laneIndex,
      sampleTime: -1,
      argInt: (mute ? 0x1 : 0) | (solo ? 0x2 : 0),
    });
  }

  setStripGain(target: string | number, db: number): boolean {
    if (target === 'master') {
      const paramId = engineMixerMasterTarget(ENGINE_MIXER_PARAM_FADER_DB);
      this.offlineEngine.setParameter(paramId, db);
      return this.realtimeNode.sendCommand({
        type: SonareEngineCommandType.SetParamSmoothed,
        targetId: paramId,
        sampleTime: -1,
        argFloat: db,
      });
    }
    const laneIndex = this.ensureTrackLane(target);
    const paramId = engineMixerLaneTarget(laneIndex, ENGINE_MIXER_PARAM_FADER_DB);
    this.offlineEngine.setParameter(paramId, db);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetParamSmoothed,
      targetId: paramId,
      sampleTime: -1,
      argFloat: db,
    });
  }

  setStripPan(target: string | number, pan: number): boolean {
    if (target === 'master') {
      const paramId = engineMixerMasterTarget(ENGINE_MIXER_PARAM_PAN);
      this.offlineEngine.setParameter(paramId, pan);
      return this.realtimeNode.sendCommand({
        type: SonareEngineCommandType.SetParamSmoothed,
        targetId: paramId,
        sampleTime: -1,
        argFloat: pan,
      });
    }
    const laneIndex = this.ensureTrackLane(target);
    const paramId = engineMixerLaneTarget(laneIndex, ENGINE_MIXER_PARAM_PAN);
    this.offlineEngine.setParameter(paramId, pan);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetParamSmoothed,
      targetId: paramId,
      sampleTime: -1,
      argFloat: pan,
    });
  }

  /**
   * Declares the mixer track lanes in an explicit order.
   *
   * Lane indices are append-only: once a track id occupies a lane, its index
   * stays fixed for the engine's lifetime. The given list must therefore start
   * with the already-declared lane ids in their current order and may only
   * append new track ids after them. Entries carrying `sends` replace that
   * track's send list; entries without `sends` leave existing sends untouched.
   *
   * @param lanes Track ids or lane descriptors in the desired lane order.
   */
  setTrackLanes(lanes: ReadonlyArray<number | EngineTrackLane>): void {
    const entries = lanes.map((lane) => (typeof lane === 'number' ? { trackId: lane } : lane));
    const ids: number[] = [];
    for (const entry of entries) {
      if (!Number.isInteger(entry.trackId) || entry.trackId <= 0) {
        throw new Error(`Invalid track id for mixer lane: ${String(entry.trackId)}`);
      }
      ids.push(entry.trackId);
    }
    if (new Set(ids).size !== ids.length) {
      throw new Error('Duplicate track id in mixer lane list');
    }
    for (let index = 0; index < this.trackLaneIds.length; index++) {
      if (ids[index] !== this.trackLaneIds[index]) {
        throw new Error(
          'Mixer lanes are append-only: keep existing lanes in order and only append new track ids',
        );
      }
    }
    for (const entry of entries) {
      if (entry.sends) {
        this.trackSends.set(
          entry.trackId,
          entry.sends.map((send) => ({ ...send })),
        );
      }
      if (entry.outputBusId !== undefined) {
        if (entry.outputBusId === 0) {
          this.trackOutputBus.delete(entry.trackId);
        } else {
          this.trackOutputBus.set(entry.trackId, entry.outputBusId);
        }
      }
    }
    this.trackLaneIds.splice(0, this.trackLaneIds.length, ...ids);
    this.syncMixer();
  }

  /**
   * Routes a track lane's post-fader output into a declared bus instead of
   * the master mix (group/folder routing); busId 0 restores the master mix.
   */
  setTrackOutputBus(target: string | number, busId: number): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    if (busId === 0) {
      this.trackOutputBus.delete(trackId);
    } else {
      this.trackOutputBus.set(trackId, busId);
    }
    this.syncMixer();
  }

  /**
   * Keys one insert of a lane strip from another lane's post-strip pre-fader
   * audio (ducking/sidechainRouter inserts). sourceTarget null removes the
   * binding.
   */
  setLaneSidechain(
    target: string | number,
    insertIndex: number,
    sourceTarget: string | number | null,
  ): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    const key = `${trackId}:${insertIndex}`;
    let sourceTrackId = 0;
    if (sourceTarget !== null) {
      const sourceIndex = this.ensureTrackLane(sourceTarget);
      sourceTrackId = this.trackLaneIds[sourceIndex];
    }
    if (sourceTrackId === 0) {
      this.laneSidechains.delete(key);
    } else {
      this.laneSidechains.set(key, { trackId, insertIndex, sourceTrackId });
    }
    this.offlineEngine.setLaneSidechain(trackId, insertIndex, sourceTrackId);
    this.postSync({
      type: 'syncMixer',
      lanes: this.mixerLanes(),
      laneSidechains: [{ trackId, insertIndex, sourceTrackId }],
    });
  }

  setSends(target: string | number, sends: EngineTrackSend[]): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    this.trackSends.set(
      trackId,
      sends.map((send) => ({ ...send })),
    );
    this.syncMixer();
  }

  setTrackBuses(buses: EngineBus[]): void {
    this.buses.splice(0, this.buses.length, ...buses.map((bus) => ({ ...bus })));
    this.syncMixer();
  }

  setBusGain(busId: number, db: number): boolean {
    const busIndex = this.ensureBus(busId);
    this.buses[busIndex] = { ...this.buses[busIndex], busId, gainDb: db };
    this.offlineEngine.setTrackBuses(this.buses);
    const paramId = engineMixerBusTarget(busIndex, ENGINE_MIXER_PARAM_FADER_DB);
    this.offlineEngine.setParameter(paramId, db);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetParamSmoothed,
      targetId: paramId,
      sampleTime: -1,
      argFloat: db,
    });
  }

  setTrackStripJson(target: string | number, sceneJson: string): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    this.offlineEngine.setTrackStripJson(trackId, sceneJson);
    this.trackStripJson.set(trackId, sceneJson);
    this.syncMixer();
  }

  setTrackStripEqBand(target: string | number, bandIndex: number, band: EqBand | string): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
    this.offlineEngine.setTrackStripEqBandJson(trackId, bandIndex, bandJson);
    this.postSync({ type: 'syncTrackStripEqBand', trackId, bandIndex, bandJson });
  }

  setTrackStripInsertBypassed(
    target: string | number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    this.offlineEngine.setTrackStripInsertBypassed(trackId, insertIndex, bypassed, resetOnBypass);
    this.postSync({
      type: 'syncTrackStripInsertBypassed',
      trackId,
      insertIndex,
      bypassed,
      resetOnBypass,
    });
  }

  setTrackStripInsertParamByName(
    target: string | number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    this.offlineEngine.setTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
    this.postSync({
      type: 'syncTrackStripInsertParamByName',
      trackId,
      insertIndex,
      paramName,
      value,
    });
  }

  setTrackStripPan(target: string | number, pan: number): void {
    const trackId = this.trackLaneIds[this.ensureTrackLane(target)];
    this.offlineEngine.setTrackStripPan(trackId, pan);
    this.postSync({ type: 'syncTrackStripPan', trackId, pan });
  }

  setTrackStripPanLaw(target: string | number, panLaw: PanLaw | number): void {
    const trackId = this.trackLaneIds[this.ensureTrackLane(target)];
    const code = panLawCode(panLaw);
    this.offlineEngine.setTrackStripPanLaw(trackId, code);
    this.postSync({ type: 'syncTrackStripPanLaw', trackId, panLaw: code });
  }

  setTrackStripPanMode(target: string | number, panMode: PanMode | number): void {
    const trackId = this.trackLaneIds[this.ensureTrackLane(target)];
    const code = panModeCode(panMode);
    this.offlineEngine.setTrackStripPanMode(trackId, code);
    this.postSync({ type: 'syncTrackStripPanMode', trackId, panMode: code });
  }

  setTrackStripDualPan(target: string | number, leftPan: number, rightPan: number): void {
    const trackId = this.trackLaneIds[this.ensureTrackLane(target)];
    this.offlineEngine.setTrackStripDualPan(trackId, leftPan, rightPan);
    this.postSync({ type: 'syncTrackStripDualPan', trackId, leftPan, rightPan });
  }

  setTrackStripChannelDelaySamples(target: string | number, delaySamples: number): void {
    const trackId = this.trackLaneIds[this.ensureTrackLane(target)];
    this.offlineEngine.setTrackStripChannelDelaySamples(trackId, delaySamples);
    this.postSync({ type: 'syncTrackStripChannelDelaySamples', trackId, delaySamples });
  }

  setStripEq(target: string | number, bandIndex: number, band: EqBand | string): void {
    if (target === 'master') {
      this.setMasterStripEqBand(bandIndex, band);
      return;
    }
    this.setTrackStripEqBand(target, bandIndex, band);
  }

  setStripInsertBypassed(
    target: string | number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    if (target === 'master') {
      this.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
      return;
    }
    this.setTrackStripInsertBypassed(target, insertIndex, bypassed, resetOnBypass);
  }

  setStripInserts(target: string | number, sceneJson: string): void {
    if (target === 'master') {
      this.setMasterStripJson(sceneJson);
      return;
    }
    this.setTrackStripJson(target, sceneJson);
  }

  setBusStripJson(busId: number, sceneJson: string): void {
    this.ensureBus(busId);
    this.offlineEngine.setBusStripJson(busId, sceneJson);
    this.busStripJson.set(busId, sceneJson);
    this.syncMixer();
  }

  setMasterStripJson(sceneJson: string): void {
    this.offlineEngine.setMasterStripJson(sceneJson);
    this.masterStripJson = sceneJson;
    this.syncMixer();
  }

  setMasterStripEqBand(bandIndex: number, band: EqBand | string): void {
    const bandJson = typeof band === 'string' ? band : JSON.stringify(band);
    this.offlineEngine.setMasterStripEqBandJson(bandIndex, bandJson);
    this.postSync({ type: 'syncMasterStripEqBand', bandIndex, bandJson });
  }

  setMasterStripInsertBypassed(
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.offlineEngine.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
    this.postSync({
      type: 'syncMasterStripInsertBypassed',
      insertIndex,
      bypassed,
      resetOnBypass,
    });
  }

  setMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.offlineEngine.setMasterStripInsertParamByName(insertIndex, paramName, value);
    this.postSync({
      type: 'syncMasterStripInsertParamByName',
      insertIndex,
      paramName,
      value,
    });
  }

  setStripInsertParamByName(
    target: string | number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    if (target === 'master') {
      this.setMasterStripInsertParamByName(insertIndex, paramName, value);
      return;
    }
    this.setTrackStripInsertParamByName(target, insertIndex, paramName, value);
  }

  setMasterChain(sceneJson: string): void {
    this.setMasterStripJson(sceneJson);
  }

  addClip(
    trackId: string | number,
    buffer: Float32Array[],
    startPpq: number,
    opts: Partial<Omit<EngineClip, 'channels' | 'startPpq'>> = {},
  ): number {
    const id = opts.id ?? this.nextClipId++;
    const clip: EngineClip = {
      ...opts,
      id,
      channels: buffer,
      startPpq,
      trackId: this.resolveTargetId(trackId),
    };
    this.ensureTrackLane(trackId);
    this.clips.set(id, clip);
    this.syncClipsDelta([clip], []);
    return id;
  }

  removeClip(clipId: number): void {
    this.clips.delete(clipId);
    this.syncClipsDelta([], [clipId]);
  }

  setMidiClips(clips: readonly EngineMidiClipSchedule[]): void {
    this.midiClips.clear();
    for (const clip of clips) {
      const id = clip.id ?? this.nextClipId++;
      this.midiClips.set(id, { ...clip, id, events: clip.events.map((event) => ({ ...event })) });
    }
    this.syncMidiClips();
  }

  setBuiltinInstrument(
    trackId: string | number,
    config: { destinationId?: number } & Record<string, unknown> = {},
  ): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.setBuiltinInstrument(config, destinationId);
    this.postInstrumentSync({ type: 'syncBuiltinInstrument', destinationId, config });
  }

  setSynthInstrument(trackId: string | number, patch: Record<string, unknown> | string = {}): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.setSynthInstrument(patch, destinationId);
    this.postInstrumentSync({ type: 'syncSynthInstrument', destinationId, patch });
  }

  loadSoundFont(data: Uint8Array): void {
    this.offlineEngine.loadSoundFont(data);
    this.postInstrumentSync({ type: 'syncLoadSoundFont', data });
  }

  setSf2Instrument(
    trackId: string | number,
    config: { destinationId?: number; gain?: number; polyphony?: number } = {},
  ): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.setSf2Instrument(config, destinationId);
    this.postInstrumentSync({ type: 'syncSf2Instrument', destinationId, config });
  }

  pushMidiNoteOn(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame = -1,
  ): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.pushMidiNoteOn(destinationId, group, channel, note, velocity, renderFrame);
    this.postSync({
      type: 'syncMidiNoteOn',
      destinationId,
      group,
      channel,
      note,
      velocity,
      renderFrame,
    });
  }

  pushMidiNoteOff(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame = -1,
  ): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.pushMidiNoteOff(destinationId, group, channel, note, velocity, renderFrame);
    this.postSync({
      type: 'syncMidiNoteOff',
      destinationId,
      group,
      channel,
      note,
      velocity,
      renderFrame,
    });
  }

  pushMidiCc(
    trackId: string | number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame = -1,
  ): void {
    const destinationId = this.resolveTargetId(trackId);
    this.offlineEngine.pushMidiCc(destinationId, group, channel, controller, value, renderFrame);
    this.postSync({
      type: 'syncMidiCc',
      destinationId,
      group,
      channel,
      controller,
      value,
      renderFrame,
    });
  }

  pushMidiPanic(renderFrame = -1): void {
    this.offlineEngine.pushMidiPanic(renderFrame);
    this.postSync({ type: 'syncMidiPanic', renderFrame });
  }

  configureCapture(options: {
    bufferFrames: number;
    channels?: number;
    source?: EngineCaptureStatus['source'];
    recordOffsetSamples?: number;
    inputMonitor?: { enabled: boolean; gain?: number };
  }): void {
    const bufferFrames = Math.trunc(options.bufferFrames);
    const channels = Math.trunc(options.channels ?? this.offlineChannelCount);
    const source = options.source ?? 'output';
    const recordOffsetSamples = Math.trunc(options.recordOffsetSamples ?? 0);
    const inputMonitor = {
      enabled: Boolean(options.inputMonitor?.enabled),
      gain: options.inputMonitor?.gain ?? 1,
    };
    this.offlineEngine.setCaptureBuffer(channels, bufferFrames);
    this.offlineEngine.setCaptureSource(source);
    this.offlineEngine.setRecordOffsetSamples(recordOffsetSamples);
    this.offlineEngine.setInputMonitor(inputMonitor.enabled, inputMonitor.gain);
    this.captureConfig = { bufferFrames, channels, source, recordOffsetSamples, inputMonitor };
    this.postSync({ type: 'syncCapture', ...this.captureConfig });
  }

  armRecord(trackId: string | number, enabled: boolean): boolean {
    if (enabled && !this.captureConfig) {
      throw new Error('Capture buffer is not configured');
    }
    this.offlineEngine.armCapture(enabled);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.ArmRecord,
      targetId: this.resolveTargetId(trackId),
      sampleTime: -1,
      argInt: enabled ? 1 : 0,
    });
  }

  punch(inPpq: number, outPpq: number): boolean {
    const inSample = this.offlineEngine.sampleAtPpq(inPpq);
    const outSample = this.offlineEngine.sampleAtPpq(outPpq);
    this.offlineEngine.setCapturePunch(inSample, outSample, true);
    // Carry BOTH endpoints as already-converted SAMPLES so the realtime engine
    // agrees with the offline engine. The previous code sent the raw PPQ out
    // point and let the consumer multiply by sampleRate (treating PPQ as
    // seconds), which ignored tempo and produced a punch-out ~2x too large at
    // 120 BPM. argInt = in sample, argFloat = out sample (full-precision double).
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.Punch,
      sampleTime: -1,
      argInt: inSample,
      argFloat: outSample,
    });
  }

  captureStatus(): Promise<EngineCaptureStatus> {
    return this.realtimeNode.requestCaptureStatus();
  }

  capturedAudio(): Promise<Float32Array[]> {
    return this.realtimeNode.requestCapturedAudio();
  }

  async resetCapture(): Promise<void> {
    this.offlineEngine.resetCapture();
    await this.realtimeNode.requestCaptureReset();
  }

  setMetronome(opts: EngineMetronomeConfig): void {
    this.offlineEngine.setMetronome(opts);
    // The full config (beatGain/accentGain/clickSamples/clickSeconds) cannot fit
    // the fixed-size SAB command record, so it is delivered out-of-band; the
    // SetMetronome command then toggles enabled state on the audio thread.
    this.postSync({ type: 'syncMetronome', config: opts });
    this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetMetronome,
      sampleTime: -1,
      argInt: opts.enabled ? 1 : 0,
    });
  }

  addMarker(ppq: number, name = ''): number {
    const id = this.nextMarkerId++;
    this.markers.set(id, { id, ppq, name });
    this.syncMarkers();
    return id;
  }

  /**
   * Replaces the whole marker set in one call.
   *
   * Entries without an `id` are assigned fresh ids; entries carrying an `id`
   * keep it (ids must be positive and unique within the list). Returns the
   * resolved markers in the order given, so a caller can map its own marker
   * identities to the engine ids used by `seekMarker`/`setLoopFromMarkers`.
   *
   * @param markers The full marker list (an empty list clears all markers).
   * @returns The markers with their resolved engine ids.
   */
  setMarkers(markers: ReadonlyArray<{ ppq: number; name?: string; id?: number }>): EngineMarker[] {
    const resolved: EngineMarker[] = [];
    const seen = new Set<number>();
    for (const marker of markers) {
      if (!Number.isFinite(marker.ppq)) {
        throw new Error(`Invalid marker ppq: ${String(marker.ppq)}`);
      }
      if (marker.id !== undefined) {
        if (!Number.isInteger(marker.id) || marker.id <= 0) {
          throw new Error(`Invalid marker id: ${String(marker.id)}`);
        }
        if (seen.has(marker.id)) {
          throw new Error(`Duplicate marker id: ${marker.id}`);
        }
      }
      const id = marker.id ?? this.nextMarkerId++;
      seen.add(id);
      if (id >= this.nextMarkerId) {
        this.nextMarkerId = id + 1;
      }
      resolved.push({ id, ppq: marker.ppq, name: marker.name ?? '' });
    }
    this.markers.clear();
    for (const marker of resolved) {
      this.markers.set(marker.id, marker);
    }
    this.syncMarkers();
    return resolved.map((marker) => ({ ...marker }));
  }

  markerCount(): number {
    return this.offlineEngine.markerCount();
  }

  markerByIndex(index: number): EngineMarker {
    return this.offlineEngine.markerByIndex(index);
  }

  marker(markerId: number): EngineMarker {
    return this.offlineEngine.marker(markerId);
  }

  seekMarker(markerId: number): boolean {
    this.offlineEngine.seekMarker(markerId);
    // Forward to the live worklet engine. Its marker set is kept in sync via the
    // 'syncMarkers' message (see syncMarkers), so a queued kSeekMarker resolves
    // the marker id to its frame on the audio thread. Returns the sendCommand
    // result (previously this method always returned a misleading `false`).
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SeekMarker,
      targetId: markerId,
      sampleTime: -1,
    });
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): boolean {
    this.offlineEngine.setLoopFromMarkers(startMarkerId, endMarkerId);
    const start = this.offlineEngine.marker(startMarkerId);
    const end = this.offlineEngine.marker(endMarkerId);
    return this.setLoop(start.ppq, end.ppq, true);
  }

  async renderOffline(totalFrames: number): Promise<Float32Array[]> {
    const frames = Math.max(0, Math.floor(totalFrames));
    const inputs: Float32Array[] = [];
    for (let ch = 0; ch < this.offlineChannelCount; ch++) {
      inputs.push(new Float32Array(frames));
    }
    return this.offlineEngine.renderOffline(inputs, this.offlineBlockSize);
  }

  onMeter(callback: (meter: SonareWorkletMeterSnapshot) => void): () => void {
    return this.realtimeNode.onMeter(callback);
  }

  onScope(callback: (scope: SonareWorkletScopeSnapshot) => void): () => void {
    return this.realtimeNode.onScope(callback);
  }

  onTelemetry(callback: (telemetry: SonareEngineTelemetryRecord) => void): () => void {
    return this.realtimeNode.onTelemetry(callback);
  }

  pollTelemetry(): SonareEngineTelemetryRecord[] {
    return this.realtimeNode.pollTelemetry();
  }

  pollMeters(): SonareWorkletMeterSnapshot[] {
    return this.realtimeNode.pollMeters();
  }

  pollScope(): SonareWorkletScopeSnapshot[] {
    return this.realtimeNode.pollScope();
  }

  destroy(): void {
    if (this.destroyed) {
      return;
    }
    this.destroyed = true;
    this.transport.stop();
    this.realtimeNode.pollTelemetry();
    this.realtimeNode.destroy();
    this.offlineEngine.destroy();
  }

  private syncClipsDelta(upserts: EngineClip[], removeIds: number[]): void {
    const clips = Array.from(this.clips.values());
    this.offlineEngine.setClips(clips);
    this.postSync({
      type: 'syncClipsDelta',
      upserts,
      removeIds,
    });
  }

  private syncMidiClips(): void {
    const clips = Array.from(this.midiClips.values());
    this.offlineEngine.setMidiClips(clips);
    this.postSync({ type: 'syncMidiClips', clips });
  }

  private mixerLanes(): EngineTrackLane[] {
    return this.trackLaneIds.map((trackId) => {
      const sends = this.trackSends.get(trackId);
      const outputBusId = this.trackOutputBus.get(trackId);
      return {
        trackId,
        ...(sends && sends.length > 0 ? { sends: sends.map((send) => ({ ...send })) } : {}),
        ...(outputBusId !== undefined ? { outputBusId } : {}),
      };
    });
  }

  private syncMixer(): void {
    const lanes = this.mixerLanes();
    const buses = this.buses.map((bus) => ({ ...bus }));
    this.offlineEngine.setTrackBuses(buses);
    if (lanes.length > 0) {
      this.offlineEngine.setTrackLanes(lanes);
    }
    const trackStrips = Array.from(this.trackStripJson, ([trackId, sceneJson]) => ({
      trackId,
      sceneJson,
    }));
    const busStrips = Array.from(this.busStripJson, ([busId, sceneJson]) => ({
      busId,
      sceneJson,
    }));
    this.postSync({
      type: 'syncMixer',
      lanes,
      buses,
      trackStrips,
      laneSidechains: Array.from(this.laneSidechains.values()),
      busStrips,
      masterStripJson: this.masterStripJson,
    });
  }

  private syncMarkers(): void {
    const markers = Array.from(this.markers.values()).sort((a, b) => a.ppq - b.ppq);
    this.offlineEngine.setMarkers(markers);
    this.postSync({ type: 'syncMarkers', markers });
  }

  private postInstrumentSync(message: SonareEngineInstrumentSyncMessage): void {
    if (this.destroyed) {
      return;
    }
    if (this.transportPlaying) {
      this.pendingInstrumentSync.push(message);
      return;
    }
    this.postSync(message);
  }

  private flushPendingInstrumentSync(): void {
    if (this.destroyed || this.pendingInstrumentSync.length === 0) {
      return;
    }
    const pending = this.pendingInstrumentSync.splice(0);
    for (const message of pending) {
      this.postSync(message);
    }
  }

  private postTempoSync(): void {
    this.postSync({
      type: 'syncTempo',
      bpm: this.tempoBpm,
      timeSignature: { ...this.timeSignature },
      tempoSegments: this.tempoSegments.map((segment) => ({ ...segment })),
      timeSignatureSegments: this.timeSignatureSegments.map((segment) => ({ ...segment })),
    });
  }

  // Posts an out-of-band control-sync message to the worklet engine processor.
  // Sync messages use a string `type` so the worklet's message handler routes
  // them to receiveSync() (numeric `type` is reserved for SonareEngineCommandRecord).
  private postSync(message: SonareEngineSyncMessage): void {
    if (this.destroyed) {
      return;
    }
    this.realtimeNode.node.port.postMessage(message);
  }

  private resolveParamId(nodeId: string, param: string | number): number {
    if (typeof param === 'number') {
      return param;
    }
    const byName = this.listParameters().find((info) => info.name === param);
    if (byName) {
      return byName.id;
    }
    return this.resolveTargetId(param || nodeId);
  }

  private resolveTargetId(target: string | number): number {
    if (typeof target === 'number') {
      return target;
    }
    const parsed = Number.parseInt(target, 10);
    return Number.isFinite(parsed) ? parsed : 0;
  }

  private ensureTrackLane(target: string | number): number {
    const trackId = this.resolveTargetId(target);
    if (!Number.isInteger(trackId) || trackId <= 0) {
      throw new Error(`Invalid track id for mixer lane: ${String(target)}`);
    }
    const existing = this.trackLaneIds.indexOf(trackId);
    if (existing >= 0) {
      return existing;
    }
    this.trackLaneIds.push(trackId);
    this.syncMixer();
    return this.trackLaneIds.length - 1;
  }

  private ensureBus(busId: number): number {
    const resolved = Math.trunc(busId);
    if (!Number.isInteger(resolved) || resolved <= 0) {
      throw new Error(`Invalid bus id for mixer bus: ${String(busId)}`);
    }
    const existing = this.buses.findIndex((bus) => bus.busId === resolved);
    if (existing >= 0) {
      return existing;
    }
    this.buses.push({ busId: resolved });
    this.syncMixer();
    return this.buses.length - 1;
  }

  private curveCode(curve: number | 'linear' | 'exponential'): number {
    if (typeof curve === 'number') {
      return curve;
    }
    return curve === 'exponential' ? 1 : 0;
  }
}

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

export function registerSonareWorkletProcessor(name = 'sonare-worklet-processor'): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareWorkletProcessor extends Base {
    private bridge: SonareWorkletProcessor;
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareWorkletProcessorOptions }) {
      super();
      const port = this.port;
      this.bridge = new SonareWorkletProcessor(options?.processorOptions ?? { sceneJson: '' }, {
        postMessage: (message) => port?.postMessage?.(message),
      });
      const onMessage = (event: { data: unknown }) => {
        if (isWorkletMessage(event.data)) {
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
  scope.registerProcessor(name, RegisteredSonareWorkletProcessor);
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

export function registerSonareRealtimeEngineWorkletProcessor(
  name = 'sonare-realtime-engine-processor',
): void {
  const scope = globalThis as unknown as {
    AudioWorkletProcessor?: new () => object;
    registerProcessor?: (processorName: string, processorCtor: unknown) => void;
  };
  if (!scope.AudioWorkletProcessor || !scope.registerProcessor) {
    throw new Error('AudioWorkletProcessor is not available in this context.');
  }
  const Base = scope.AudioWorkletProcessor;
  class RegisteredSonareRealtimeEngineWorkletProcessor extends Base {
    private bridge?: SonareRealtimeEngineWorkletProcessor;
    private rtBridge?: SonareRtRealtimeEngineRuntime;
    private readonly pendingMessages: unknown[] = [];
    readonly port?: WorkletPort;

    constructor(options?: { processorOptions?: SonareRealtimeEngineWorkletProcessorOptions }) {
      super();
      const port = this.port;
      const processorOptions = options?.processorOptions ?? {};
      if (processorOptions.runtimeTarget === 'sonare-rt') {
        void this.initializeSonareRt(processorOptions, port);
      } else {
        void this.initializeEmbind(processorOptions, port);
      }
      const onMessage = (event: { data: unknown }) => {
        if (!this.bridge && !this.rtBridge) {
          if (this.pendingMessages.length < 1024) {
            this.pendingMessages.push(event.data);
          }
          return;
        }
        if (isEngineCommandRecord(event.data)) {
          this.bridge?.receiveCommand(event.data);
          this.rtBridge?.receiveCommand(event.data);
        } else if (isEngineSyncMessage(event.data)) {
          this.bridge?.receiveSync(event.data);
          this.rtBridge?.receiveSync(event.data);
        } else if (isEngineCaptureRequestMessage(event.data)) {
          this.bridge?.receiveCaptureRequest(event.data);
          this.rtBridge?.receiveCaptureRequest(event.data, port);
        } else if (isEngineTransportRequestMessage(event.data)) {
          this.bridge?.receiveTransportRequest(event.data);
          this.rtBridge?.receiveTransportRequest(event.data, port);
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
      if (this.rtBridge) {
        return this.rtBridge.process(inputs, outputs);
      }
      if (this.bridge) {
        return this.bridge.process(inputs, outputs);
      }
      const output = outputs[0];
      for (const channel of output ?? []) {
        channel.fill(0);
      }
      return true;
    }

    private replayPendingMessages(port?: WorkletPort): void {
      const messages = this.pendingMessages.splice(0);
      for (const data of messages) {
        if (isEngineCommandRecord(data)) {
          this.bridge?.receiveCommand(data);
          this.rtBridge?.receiveCommand(data);
        } else if (isEngineSyncMessage(data)) {
          this.bridge?.receiveSync(data);
          this.rtBridge?.receiveSync(data);
        } else if (isEngineCaptureRequestMessage(data)) {
          this.bridge?.receiveCaptureRequest(data);
          this.rtBridge?.receiveCaptureRequest(data, port);
        } else if (isEngineTransportRequestMessage(data)) {
          this.bridge?.receiveTransportRequest(data);
          this.rtBridge?.receiveTransportRequest(data, port);
        }
      }
    }

    private async initializeEmbind(
      options: SonareRealtimeEngineWorkletProcessorOptions,
      port?: WorkletPort,
    ): Promise<void> {
      try {
        const initPromise = (
          globalThis as typeof globalThis & { SonareEmbindInitPromise?: Promise<void> }
        ).SonareEmbindInitPromise;
        if (initPromise) {
          await initPromise;
        }
        if (!isInitialized()) {
          type EmbindModuleFactory = (options?: {
            locateFile?: (path: string, prefix: string) => string;
            wasmBinary?: ArrayBuffer | Uint8Array;
          }) => Promise<SonareModule>;
          const moduleFactory = (
            globalThis as typeof globalThis & {
              SonareEmbindModuleFactory?: EmbindModuleFactory;
            }
          ).SonareEmbindModuleFactory;
          if (!moduleFactory) {
            throw new Error('embind realtime engine module is not initialized.');
          }
          await initSonareModule({
            locateFile: (path) => path,
            wasmBinary: options.wasmBinary,
            moduleFactory,
          });
        }
        this.bridge = new SonareRealtimeEngineWorkletProcessor(options, {
          postMessage: (message) => port?.postMessage?.(message),
          onMeter: (meter) => port?.postMessage?.(meter),
        });
        for (const message of options.initialSyncMessages ?? []) {
          this.bridge.receiveSync(message);
        }
        for (const command of options.initialCommands ?? []) {
          this.bridge.receiveCommand(command);
        }
        this.replayPendingMessages(port);
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'embind' });
      } catch (error) {
        port?.postMessage?.({
          type: 'error',
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }

    private async initializeSonareRt(
      options: SonareRealtimeEngineWorkletProcessorOptions,
      port?: WorkletPort,
    ): Promise<void> {
      try {
        if (!options.rtModuleUrl) {
          throw new Error('rtModuleUrl is required for sonare-rt AudioWorklet runtime.');
        }
        const rtModuleUrl = options.rtModuleUrl;
        const memory = new WebAssembly.Memory({ initial: 1024, maximum: 1024, shared: true });
        const globalFactory = (
          globalThis as typeof globalThis & {
            SonareRtModuleFactory?: (options?: {
              wasmMemory?: WebAssembly.Memory;
              wasmBinary?: ArrayBuffer | Uint8Array;
              locateFile?: (path: string) => string;
            }) => Promise<SonareRtModule>;
          }
        ).SonareRtModuleFactory;
        const moduleFactory = globalFactory
          ? { default: globalFactory }
          : ((await import(rtModuleUrl)) as {
              default: (options?: {
                wasmMemory?: WebAssembly.Memory;
                wasmBinary?: ArrayBuffer | Uint8Array;
                locateFile?: (path: string) => string;
              }) => Promise<SonareRtModule>;
            });
        const module = await moduleFactory.default({
          wasmMemory: memory,
          wasmBinary: options.rtWasmBinary,
          locateFile: (path) => rtModuleUrl.replace(/[^/]*$/, path),
        });
        this.rtBridge = new SonareRtRealtimeEngineRuntime({
          module,
          memory,
          sampleRate: options.sampleRate,
          blockSize: options.blockSize,
          channelCount: options.channelCount,
          commandSharedBuffer: options.commandSharedBuffer,
          commandRingCapacity: options.commandRingCapacity,
          telemetrySharedBuffer: options.telemetrySharedBuffer,
          telemetryRingCapacity: options.telemetryRingCapacity,
        });
        this.replayPendingMessages(port);
        port?.postMessage?.({ type: 'ready', runtimeTarget: 'sonare-rt' });
      } catch (error) {
        port?.postMessage?.({
          type: 'error',
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }
  }
  scope.registerProcessor(name, RegisteredSonareRealtimeEngineWorkletProcessor);
}
