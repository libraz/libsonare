import type { EngineClip, SynthPatch } from '../index.js';
import { ClipPageProvider, RealtimeEngine } from '../index.js';
import { requireCommandOperand } from '../internal/engine_command_ring.js';
import { engineNativeHandle } from '../internal/engine_native_handle.js';
import { refuse, refuseType } from '../internal/entry.js';
import { errorText, failurePayload } from '../internal/failure_payload.js';
import { umpWord0 } from '../internal/ump_events.js';
import { UINT32_MAX } from '../internal/value_guards.js';
import {
  DEFAULT_METRONOME_CONFIG,
  type EngineClipPageRequestMessage,
  type ResolvedMetronomeConfig,
  resolveMetronomeConfig,
  type SonareEngineCaptureRequestMessage,
  type SonareEngineCaptureResponseMessageInternal,
  type SonareEngineInsertGainReductionRequestMessage,
  type SonareEngineInsertGainReductionResponseMessage,
  type SonareEngineReadRequestMessage,
  type SonareEngineReadResponseMessage,
  type SonareEngineSyncMessage,
  type SonareEngineSyncMidiInputEventMessage,
  type SonareEngineTransportRequestMessage,
  type SonareEngineTransportResponseMessage,
  type TypedWorkletTransport,
} from '../internal/worklet_messages.js';
import type { SonareRealtimeEngineWorkletProcessorConstructorOptions } from '../public_types_worklet.js';
import { copyPlanesToOutput, type WorkletInput, type WorkletOutput } from './audio_types.js';
import { buildCaptureConfig } from './engine-offline.js';
import {
  assertEngineProcessorOptionTypes,
  assertKnownProcessorOptions,
  isEngineCaptureRequestMessage,
  isEngineCommandRecord,
  isEngineInsertGainReductionRequestMessage,
  isEngineReadRequestMessage,
  isEngineSyncMessage,
  isEngineTransportRequestMessage,
  requireChannelCount,
  requireIntegerOption,
  resolveScopeBandCount,
  resolveScopeIntervalFrames,
  resolveScopeSampleRate,
  SONARE_SCOPE_MAX_BANDS,
  SONARE_SCOPE_MAX_POINTS,
} from './guards.js';
import {
  clipPageRequestRingFromSharedBuffer,
  commitOverwriteWrite,
  commitSonareEngineTelemetryRingWrite,
  type EngineTelemetryRecord,
  type EngineTelemetryRingBuffer,
  encodeFrameHi,
  encodeFrameLo,
  engineRingFromSharedBuffer,
  engineTelemetryRingFromSharedBuffer,
  externalMidiRingFromSharedBuffer,
  isRecord,
  meterFromEngine,
  meterRingFromSharedBuffer,
  pendingSonareEngineCommandCount,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareExternalMidiRingBuffer,
  type SharedClipPageRequestRingWriter,
  type SharedExternalMidiRingWriter,
  type SharedMeterRingWriter,
  type SharedScopeRingWriter,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_CHANNEL_COUNT_OFFSET,
  SONARE_METER_RING_MAX_CHANNELS,
  SONARE_METER_RING_PLANES_OFFSET,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  scopeRingFromSharedBuffer,
  sonareExternalMidiRingHasRoom,
  telemetryFromEngine,
  toSafeInteger,
  tryBeginOverwriteWrite,
  type WorkletMeterSnapshot,
  type WorkletScopeSnapshot,
  writePreparedInt64Words,
  writeSonareEngineTelemetryRingBuffer,
} from './protocol.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './render_quantum.js';

function captureTransferList(channels: readonly Float32Array[]): Transferable[] {
  const transfers: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const channel of channels) {
    const buffer = channel.buffer;
    if (!(buffer instanceof ArrayBuffer)) {
      throw refuseType('capture response channels must use plain ArrayBuffers');
    }
    if (!seen.has(buffer)) {
      seen.add(buffer);
      transfers.push(buffer);
    }
  }
  return transfers;
}

// An unchanged, still-missing page set is re-posted this often so a failed read is retried.
const CLIP_PAGE_REPOST_SECONDS = 0.25;
/** Shared options object so the render callback allocates nothing. */
const WITH_MONITOR = Object.freeze({ withMonitor: true });
const METER_TARGET_SLOTS = 42;
// `meterScratchValue` fields past the stereo set: channel count, then four 8-plane blocks.
const METER_SCRATCH_CHANNEL_COUNT = 11;
const METER_SCRATCH_PLANES = 12;
/** Plane blocks a surround meter record carries after the stereo set. */
const METER_PLANE_KINDS = 4;
/** Most events one render quantum drains from a telemetry, meter, scope or command queue. */
const DRAIN_LIMIT_PER_QUANTUM = 64;
/** Most external MIDI events one render quantum drains. */
const EXTERNAL_MIDI_DRAIN_LIMIT = 256;

/** The wire spells "apply at the next block head" as a negative frame; the engine facade as absence. */
function wireRenderFrame(frame: number | bigint | undefined): number | undefined {
  const value = Number(frame ?? -1);
  return value < 0 ? undefined : value;
}

/** The AudioContext frame of the quantum the scope is rendering; 0 outside an AudioWorklet scope. */
function scopeCurrentFrame(): number {
  const frame = (globalThis as { currentFrame?: unknown }).currentFrame;
  return typeof frame === 'number' ? frame : 0;
}

/** UMP status nibble of each typed live MIDI input event. */
const MIDI1_STATUS = {
  syncMidiInputNoteOff: 0x8,
  syncMidiInputNoteOn: 0x9,
  syncMidiInputPolyPressure: 0xa,
  syncMidiInputCc: 0xb,
  syncMidiInputChannelPressure: 0xd,
  syncMidiInputPitchBend: 0xe,
} as const satisfies Record<SonareEngineSyncMidiInputEventMessage['type'], number>;

/** One-word scratch a timed typed event is packed into; embind copies it out. */
const MIDI1_WORD = new Uint32Array(1);

/** The MIDI 1.0 channel-voice packet of a typed event, laid out as `sonare::midi::make_midi1_*`; the facade validated it. */
function midi1InputWords(message: SonareEngineSyncMidiInputEventMessage): Uint32Array {
  let byte2 = message.data0;
  let byte3 = message.data1;
  if (message.type === 'syncMidiInputPitchBend') {
    byte2 = message.data0 & 0x7f;
    byte3 = (message.data0 >> 7) & 0x7f;
  }
  MIDI1_WORD[0] = umpWord0(
    0x2,
    message.group,
    MIDI1_STATUS[message.type],
    message.channel,
    byte2,
    byte3,
  );
  return MIDI1_WORD;
}

/** Pushes a typed live MIDI input event at its block offset through the engine's own encoder. */
function pushMidiInputEvent(
  engine: RealtimeEngine,
  message: SonareEngineSyncMidiInputEventMessage,
): void {
  switch (message.type) {
    case 'syncMidiInputNoteOn':
      engine.pushMidiInputNoteOn(
        message.group,
        message.channel,
        message.data0,
        message.data1,
        message.offsetInBlockSamples,
      );
      break;
    case 'syncMidiInputNoteOff':
      engine.pushMidiInputNoteOff(
        message.group,
        message.channel,
        message.data0,
        message.data1,
        message.offsetInBlockSamples,
      );
      break;
    case 'syncMidiInputCc':
      engine.pushMidiInputCc(
        message.group,
        message.channel,
        message.data0,
        message.data1,
        message.offsetInBlockSamples,
      );
      break;
    case 'syncMidiInputPitchBend':
      engine.pushMidiInputPitchBend(
        message.group,
        message.channel,
        message.data0,
        message.offsetInBlockSamples,
      );
      break;
    case 'syncMidiInputChannelPressure':
      engine.pushMidiInputChannelPressure(
        message.group,
        message.channel,
        message.data0,
        message.offsetInBlockSamples,
      );
      break;
    case 'syncMidiInputPolyPressure':
      engine.pushMidiInputPolyPressure(
        message.group,
        message.channel,
        message.data0,
        message.data1,
        message.offsetInBlockSamples,
      );
      break;
  }
}

/**
 * AudioWorklet-style bridge for the DAW realtime engine facade.
 *
 * Backed by the `sonare.wasm` embind facade.
 */
export class SonareRealtimeEngineWorkletProcessor {
  /** Engine sample rate in Hz. */
  readonly sampleRate: number;
  /** Largest render quantum rendered per `process()` call, in frames. */
  readonly blockSize: number;
  /** Channel count of every input, output and monitor bus, fixed at construction. */
  readonly numChannels: number;
  private engine: RealtimeEngine;
  /** The embind object behind `engine`: scratch reads and the code-carrying sync messages. */
  private native: ReturnType<typeof engineNativeHandle>;
  private closed = false;
  /** Timed MIDI events, live input and track-addressed, whose render frame had already been rendered on arrival. */
  private lateMidiEventCount = 0;
  /** Render frame and AudioContext frame of the last quantum's head; maps a context time to a render frame. */
  private headRenderFrame: number | undefined;
  private headContextFrame = 0;
  private commandRing?: SonareEngineCommandRingBuffer;
  /** Sequence number of the last control sync received; SAB commands wait for theirs. */
  private syncSequence = 0;
  /** A ring command popped ahead of the sync it follows, applied once that sync arrives. */
  private heldCommand: SonareEngineCommandRecord | undefined;
  private telemetryRing?: EngineTelemetryRingBuffer;
  private meterRing?: SharedMeterRingWriter;
  private scopeRing?: SharedScopeRingWriter;
  private clipPageRequestRing?: SharedClipPageRequestRingWriter;
  private externalMidiRing?: SharedExternalMidiRingWriter;
  private transport?: TypedWorkletTransport;
  private meterIntervalFrames: number;
  // Last delivered meter frame per target slot: master, 32 lanes, 8 buses, then every other id.
  private readonly lastMeterFrameBySlot = new Float64Array(METER_TARGET_SLOTS).fill(
    Number.NEGATIVE_INFINITY,
  );
  private scopeIntervalFrames: number;
  private scopeEnabled: boolean;
  private scopeBands: number;
  // Prepared overwrite-ring payloads. These are allocated once with the
  // processor and are filled completely before a producer claims a slot.
  private readonly meterRingScratch = new Float32Array(SONARE_METER_RING_RECORD_FLOATS);
  private readonly scopeRingBandScratch = new Float32Array(SONARE_SCOPE_MAX_BANDS);
  private readonly scopeRingPointScratch = new Float32Array(SONARE_SCOPE_MAX_POINTS * 2);
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
  // clipId -> native provider; `streamKey` marks a provider an OPFS stream owns,
  // which outlives its clip's schedule until that stream closes.
  private readonly pagedClipProviders = new Map<
    number,
    { provider: ClipPageProvider; streamKey: number | undefined }
  >();
  private readonly pagedClipPageFrames = new Map<number, number>();
  private readonly pendingPagedClips = new Map<number, EngineClip>();
  // The worklet drains at most this many distinct page misses per render
  // quantum. The fixed scalar buffers avoid retaining an unbounded queue on
  // the audio thread; any remaining native requests stay queued for the next
  // quantum and render silence meanwhile.
  private readonly clipPageRequestClipIds = new Float64Array(64);
  private readonly clipPageRequestPageIndices = new Float64Array(64);
  private clipPageRequestOverflowReported = 0;
  private readonly postedClipPageRequestClipIds = new Float64Array(64);
  private readonly postedClipPageRequestPageIndices = new Float64Array(64);
  private postedClipPageRequestCount = 0;
  private clipPageRequestFrameClock = 0;
  private lastClipPageRequestPostFrame = 0;
  private readonly clipPageRepostFrames: number;
  private readonly clipPageRequestPayloadEntries = Array.from({ length: 64 }, () => ({
    clipId: 0,
    pageIndex: 0,
  }));
  // One preallocated request array per batch size, so a post builds no objects.
  private readonly clipPageRequestPayloadArrays = Array.from({ length: 65 }, (_, n) =>
    this.clipPageRequestPayloadEntries.slice(0, n),
  );
  private readonly clipPageRequestMessage: EngineClipPageRequestMessage = {
    type: 'clipPageRequest',
    requests: [],
  };
  private readonly clipPageRequestMessageWithDropped: EngineClipPageRequestMessage = {
    type: 'clipPageRequest',
    requests: [],
    dropped: 0,
  };

  constructor(options: SonareRealtimeEngineWorkletProcessorConstructorOptions) {
    assertKnownProcessorOptions('engineProcessorConstructor', options);
    assertEngineProcessorOptionTypes('engineProcessorConstructor', options);
    this.sampleRate = resolveScopeSampleRate(options.sampleRate);
    this.clipPageRepostFrames = Math.max(1, Math.round(this.sampleRate * CLIP_PAGE_REPOST_SECONDS));
    this.blockSize = options.maxBlockSize ?? AUDIO_WORKLET_RENDER_QUANTUM;
    this.numChannels = requireChannelCount(options.numChannels, 'numChannels');
    this.transport = options.transport;
    // Zero is the off switch for meter publication, so the floor is 0.
    this.meterIntervalFrames = requireIntegerOption(
      options.meterIntervalFrames,
      2048,
      'meterIntervalFrames',
      0,
    );
    const scopeBuffer = options.scopeSharedBuffer;
    this.scopeIntervalFrames = resolveScopeIntervalFrames(
      options.scopeIntervalFrames,
      scopeBuffer == null ? 0 : this.blockSize,
    );
    this.scopeBands = resolveScopeBandCount(options.scopeBands);
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
    this.scopeRing =
      scopeBuffer == null
        ? undefined
        : scopeRingFromSharedBuffer(scopeBuffer, options.scopeRingCapacity, options.scopeBands);
    this.scopeEnabled =
      this.scopeIntervalFrames > 0 &&
      (this.scopeRing !== undefined || typeof this.transport?.postMessage === 'function');
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
    if (this.commandRing && this.commandRing.capacity > 65536) {
      throw refuse('commandRingCapacity must be at most 65536');
    }
    this.engine = new RealtimeEngine({
      sampleRate: this.sampleRate,
      maxBlockSize: this.blockSize,
      commandCapacity: Math.max(1024, this.commandRing?.capacity ?? 0),
      telemetryCapacity: 1024,
      maxChannels: this.numChannels,
    });
    this.native = engineNativeHandle(this.engine);
    // Allocate persistent WASM-heap scratch (worst case: numChannels channels x
    // blockSize frames) and acquire the per-channel heap views once.
    this.engine.prepareChannels(this.numChannels, this.blockSize);
    this.channelBuffers = new Array(this.numChannels);
    for (let ch = 0; ch < this.numChannels; ch++) {
      this.channelBuffers[ch] = this.engine.channelBuffer(ch, this.blockSize);
    }
    this.cueOutput = options.cueOutput === true;
    if (this.cueOutput) {
      this.engine.prepareMonitorChannels(this.numChannels, this.blockSize);
      this.monitorBuffers = new Array(this.numChannels);
      for (let ch = 0; ch < this.numChannels; ch++) {
        this.monitorBuffers[ch] = this.engine.monitorChannelBuffer(ch, this.blockSize);
      }
    }
    // Arm the native scope producer only when a SAB ring or postMessage sink exists.
    if (this.scopeEnabled) {
      this.engine.configureScopeTelemetry(
        this.scopeIntervalFrames,
        this.scopeRing?.bands ?? this.scopeBands,
      );
    }
  }

  /**
   * Renders one quantum: drains pending commands, feeds `inputs[0]` to the engine and writes the
   * program mix to `outputs[0]`. A quantum longer than `blockSize` renders silence.
   *
   * @returns `false` once the processor is destroyed, else `true`.
   */
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
    // A quantum larger than the pre-allocated scratch renders silence and still
    // publishes telemetry; it is never clamped and never reallocates.
    if (frames > this.blockSize) {
      for (const channel of output) {
        channel.fill(0);
      }
      this.publishTelemetry();
      return true;
    }

    this.drainCommands();
    this.headRenderFrame = this.native.renderFrame();
    this.headContextFrame = scopeCurrentFrame();

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
    for (let ch = 0; ch < this.numChannels; ch++) {
      const dst = this.channelBuffers[ch];
      const source = input?.[ch];
      if (source && source.length === frames) {
        dst.set(source);
      } else {
        dst.fill(0, 0, frames);
      }
    }

    // Run the engine in place over the prepared scratch (allocation-free). The
    // monitor variant keeps the cue bus out of the program planes so it can go
    // to its own output; the plain call folds it in, as it always has.
    if (this.cueOutput) {
      this.engine.processPrepared(frames, WITH_MONITOR);
    } else {
      this.engine.processPrepared(frames);
    }

    copyPlanesToOutput(output, this.channelBuffers, frames);
    if (this.cueOutput) {
      const cue = outputs[1];
      if (cue) {
        copyPlanesToOutput(cue, this.monitorBuffers, frames);
      }
    }
    this.publishClipPageRequests(frames);
    this.publishTelemetry();
    this.publishMeters();
    this.publishScope();
    this.publishExternalMidi();
    return true;
  }

  private reacquireChannelBuffers(): void {
    for (let ch = 0; ch < this.numChannels; ch++) {
      this.channelBuffers[ch] = this.engine.channelBuffer(ch, this.blockSize);
    }
  }

  private reacquireMonitorBuffers(): void {
    for (let ch = 0; ch < this.numChannels; ch++) {
      this.monitorBuffers[ch] = this.engine.monitorChannelBuffer(ch, this.blockSize);
    }
  }

  /**
   * Routes one message the node posted to the handler for its type. An AudioWorklet entry of a
   * host's own passes every message it does not handle itself here. A message of a type this
   * build does not know is answered with a `syncError` carrying a `TypeError` failure, and its
   * sequence number is still noted, so the commands sent after it are not held.
   */
  receiveMessage(message: unknown): void {
    if (isEngineCommandRecord(message)) {
      this.receiveCommand(message);
    } else if (isEngineSyncMessage(message)) {
      this.receiveSync(message);
    } else if (isEngineCaptureRequestMessage(message)) {
      this.receiveCaptureRequest(message);
    } else if (isEngineTransportRequestMessage(message)) {
      this.receiveTransportRequest(message);
    } else if (isEngineInsertGainReductionRequestMessage(message)) {
      this.receiveInsertGainReductionRequest(message);
    } else if (isEngineReadRequestMessage(message)) {
      this.receiveReadRequest(message);
    } else {
      this.refuseUnknownMessage(message);
    }
  }

  private refuseUnknownMessage(message: unknown): void {
    const record = isRecord(message) ? message : {};
    const syncType = typeof record.type === 'string' ? record.type : String(record.type);
    this.receiveOrdered(record, () => {
      const error = refuseType(`Unrecognized worklet message: ${syncType}`);
      this.transport?.postMessage?.({
        type: 'syncError',
        syncType,
        message: errorText(error),
        failure: failurePayload(error),
        ...(typeof record.entry === 'string' ? { entry: record.entry } : {}),
      });
    });
  }

  private receiveCommand(command: SonareEngineCommandRecord): void {
    if (!this.closed) {
      this.safeApplyCommand(command);
    }
  }

  // Applies an out-of-band control-plane sync message on the AudioWorklet
  // thread. These handlers must remain bounded: expensive clip transforms are
  // performed on the main-thread mirror, and long pre-baked PCM arrives in
  // small pages before the final lightweight clip schedule is committed.
  private receiveSync(message: SonareEngineSyncMessage): void {
    this.receiveOrdered(message, () => {
      try {
        this.settleCommands();
        this.applySync(message);
      } catch (error) {
        const entry = (message as { entry?: unknown }).entry;
        this.transport?.postMessage?.({
          type: 'syncError',
          syncType: message.type,
          message: errorText(error),
          failure: failurePayload(error),
          ...(typeof entry === 'string' ? { entry } : {}),
        });
      }
    });
  }

  /**
   * Handles one port message in order with the SAB command ring. Its sequence number is noted
   * even when @p handle throws, so a command published after it is never held forever.
   */
  private receiveOrdered(message: object, handle: () => void): void {
    if (this.closed) {
      return;
    }
    try {
      handle();
    } finally {
      this.noteSyncSequence((message as { syncSequence?: unknown }).syncSequence);
    }
  }

  /**
   * Applies every command published before the message being handled: the whole ring snapshot,
   * past the per-quantum budget, then the immediate ones. Scheduled commands keep their frame.
   */
  private settleCommands(): void {
    if (this.commandRing) {
      this.drainCommands(
        pendingSonareEngineCommandCount(this.commandRing) + (this.heldCommand ? 1 : 0),
      );
    }
    this.engine.applyCommandsDueNowPreservingFuture();
  }

  /** Records the sequence number a posted sync carried; a sync without one changes nothing. */
  private noteSyncSequence(sequence: unknown): void {
    if (typeof sequence === 'number' && Number.isInteger(sequence) && sequence >= 0) {
      this.syncSequence = sequence >>> 0;
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
        this.applyClipDelta(message.upserts, message.removeIds);
        for (const clipId of message.removeIds) {
          // A stream-owned provider stays, so re-adding its clip sounds again.
          if (this.pagedClipProviders.get(clipId)?.streamKey === undefined) {
            this.removePagedClipProvider(clipId);
          }
        }
        this.liveClips.clear();
        for (const [clipId, clip] of nextClips) {
          this.liveClips.set(clipId, clip);
        }
        break;
      }
      case 'syncClipPageProvider': {
        const provider = new ClipPageProvider({
          numChannels: message.numChannels,
          numSamples: message.numSamples,
          pageFrames: message.pageFrames,
        });
        this.removePagedClipProvider(message.clipId);
        this.pagedClipProviders.set(message.clipId, {
          provider,
          streamKey: message.streamKey,
        });
        this.pagedClipPageFrames.set(message.clipId, message.pageFrames);
        if (message.clip) {
          this.pendingPagedClips.set(message.clipId, message.clip);
        }
        break;
      }
      case 'syncClipPage': {
        const provider = this.ownedPagedClipProvider(message.clipId, message.streamKey);
        if (provider !== undefined) {
          provider.supply(message.pageIndex, message.channels);
        }
        break;
      }
      case 'syncClipPageClear': {
        const provider = this.ownedPagedClipProvider(message.clipId, message.streamKey);
        if (provider !== undefined) {
          provider.clear(message.pageIndex);
        }
        break;
      }
      case 'syncClipPagePrefetchFrames': {
        this.engine.setClipPagePrefetchFrames(message.frames);
        break;
      }
      case 'syncWarpVoiceCapacity': {
        this.engine.setWarpVoiceCapacity(message.voices);
        break;
      }
      case 'syncClipPageCommit': {
        const provider = this.ownedPagedClipProvider(message.clipId, message.streamKey);
        const clip = message.clip ?? this.pendingPagedClips.get(message.clipId);
        if (provider !== undefined && clip) {
          const nextClip = { ...clip, pageProvider: provider };
          this.engine.upsertClip(nextClip);
          this.liveClips.set(message.clipId, nextClip);
          this.pendingPagedClips.delete(message.clipId);
        }
        break;
      }
      case 'syncClipPageDestroy': {
        const provider = this.ownedPagedClipProvider(message.clipId, message.streamKey);
        if (provider === undefined) {
          break;
        }
        // Only a clip still reading this provider goes; a same-id successor stays.
        if (this.liveClips.get(message.clipId)?.pageProvider === provider) {
          this.engine.removeClip(message.clipId);
          this.liveClips.delete(message.clipId);
        }
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
      case 'syncInputMonitor':
        this.engine.setInputMonitor(message.monitor);
        break;
      case 'syncInputTrack':
        if (message.trackId === null) {
          this.engine.clearInputTrack();
        } else {
          this.engine.setInputTrack(message.trackId);
        }
        break;
      case 'syncAutomation':
        this.engine.setAutomationLane(message.paramId, message.points);
        break;
      case 'syncParameters': {
        // An empty list is the facade's clearParameters; any other list only
        // adds the ids this engine lacks. Neither touches an automation lane.
        if (message.parameters.length === 0) {
          this.engine.clearParameters();
          break;
        }
        const registered = new Set<number>();
        for (const info of this.engine.parameters()) {
          registered.add(info.id);
        }
        for (const info of message.parameters) {
          if (!registered.has(info.id)) {
            this.engine.addParameter(info);
          }
        }
        break;
      }
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
      case 'syncMixer': {
        if (message.sidechainDelta === true) {
          // Sidechain setters send a binding delta through this message type.
          // Their `lanes` field is context, not a request to rebuild routing;
          // rebuilding it here would discard live insert smoother slots.
          for (const binding of message.laneSidechains ?? []) {
            this.native.setLaneSidechain(
              binding.trackId,
              binding.insertIndex,
              binding.sourceTrackId,
            );
          }
          for (const binding of message.busSidechains ?? []) {
            this.native.setBusSidechain(
              binding.busId,
              binding.insertIndex,
              binding.sourceKind,
              binding.sourceId,
            );
          }
          for (const binding of message.masterSidechains ?? []) {
            this.native.setMasterSidechain(
              binding.insertIndex,
              binding.sourceKind,
              binding.sourceId,
            );
          }
          break;
        }
        if (message.buses) {
          this.engine.setTrackBuses(message.buses);
        }
        this.engine.setTrackLanes(message.lanes);
        for (const strip of message.trackStrips ?? []) {
          this.native.setTrackStripJson(strip.trackId, strip.settingsJson);
        }
        for (const strip of message.busStrips ?? []) {
          this.native.setBusStripJson(strip.busId, strip.settingsJson);
        }
        if (message.masterStripJson) {
          this.native.setMasterStripJson(message.masterStripJson);
        }
        for (const target of message.insertBaseResets ?? []) {
          switch (target.kind) {
            case 'track':
              this.native.clearTrackInsertParameterBases(target.trackId);
              break;
            case 'bus':
              this.native.clearBusInsertParameterBases(target.busId);
              break;
            case 'master':
              this.native.clearMasterInsertParameterBases();
              break;
          }
        }
        for (const binding of message.laneSidechains ?? []) {
          this.native.setLaneSidechain(binding.trackId, binding.insertIndex, binding.sourceTrackId);
        }
        for (const binding of message.busSidechains ?? []) {
          this.native.setBusSidechain(
            binding.busId,
            binding.insertIndex,
            binding.sourceKind,
            binding.sourceId,
          );
        }
        for (const binding of message.masterSidechains ?? []) {
          this.native.setMasterSidechain(binding.insertIndex, binding.sourceKind, binding.sourceId);
        }
        for (const override of message.insertParamOverrides ?? []) {
          switch (override.kind) {
            case 'track':
              this.native.restoreTrackStripInsertParamByName(
                override.trackId,
                override.insertIndex,
                override.paramName,
                override.value,
              );
              break;
            case 'bus':
              this.native.restoreBusStripInsertParamByName(
                override.busId,
                override.insertIndex,
                override.paramName,
                override.value,
              );
              break;
            case 'master':
              this.native.restoreMasterStripInsertParamByName(
                override.insertIndex,
                override.paramName,
                override.value,
              );
              break;
          }
        }
        break;
      }
      case 'syncCapture': {
        // Validate every field before the buffer replacement discards the recording.
        const { offlineOptions } = buildCaptureConfig(
          {
            capacityFrames: message.bufferFrames,
            numChannels: message.channels,
            source: message.source,
            recordOffsetSamples: message.recordOffsetSamples,
          },
          message.channels,
          this.sampleRate,
          (ppq) => this.engine.sampleAtPpq(ppq),
        );
        this.engine.configureCapture({
          ...offlineOptions,
          ...(message.punch === undefined
            ? {}
            : {
                punch: {
                  startSample: message.punch.startSample,
                  endSample: message.punch.endSample,
                },
              }),
        });
        break;
      }
      case 'syncTrackStripEqBand':
        this.native.setTrackStripEqBandJson(message.trackId, message.bandIndex, message.bandJson);
        break;
      case 'syncMasterStripEqBand':
        this.native.setMasterStripEqBandJson(message.bandIndex, message.bandJson);
        break;
      case 'syncBusStripEqBand':
        this.native.setBusStripEqBandJson(message.busId, message.bandIndex, message.bandJson);
        break;
      case 'syncTrackStripInsertBypassed':
        this.native.setTrackStripInsertBypassed(
          message.trackId,
          message.insertIndex,
          message.bypassed,
          message.resetOnBypass,
        );
        break;
      case 'syncMasterStripInsertBypassed':
        this.native.setMasterStripInsertBypassed(
          message.insertIndex,
          message.bypassed,
          message.resetOnBypass,
        );
        break;
      case 'syncTrackStripInsertParamByName':
        if (
          !this.native.applyTrackStripInsertParamByNameNow(
            message.trackId,
            message.insertIndex,
            message.paramName,
            message.value,
          )
        ) {
          // A burst can exhaust the fixed smoother slots before the next
          // render quantum. Apply the last knob value exactly in that case.
          this.native.restoreTrackStripInsertParamByName(
            message.trackId,
            message.insertIndex,
            message.paramName,
            message.value,
          );
        }
        break;
      case 'syncMasterStripInsertParamByName':
        if (
          !this.native.applyMasterStripInsertParamByNameNow(
            message.insertIndex,
            message.paramName,
            message.value,
          )
        ) {
          this.native.restoreMasterStripInsertParamByName(
            message.insertIndex,
            message.paramName,
            message.value,
          );
        }
        break;
      case 'syncBusStripInsertParamByName':
        if (
          !this.native.applyBusStripInsertParamByNameNow(
            message.busId,
            message.insertIndex,
            message.paramName,
            message.value,
          )
        ) {
          this.native.restoreBusStripInsertParamByName(
            message.busId,
            message.insertIndex,
            message.paramName,
            message.value,
          );
        }
        break;
      case 'syncBusStripInsertBypassed':
        this.native.setBusStripInsertBypassed(
          message.busId,
          message.insertIndex,
          message.bypassed,
          message.resetOnBypass,
        );
        break;
      case 'syncTrackStripPan':
        this.native.setTrackStripPan(message.trackId, message.pan);
        break;
      case 'syncTrackStripPanLaw':
        this.native.setTrackStripPanLaw(message.trackId, message.panLaw);
        break;
      case 'syncTrackStripPanMode':
        this.native.setTrackStripPanMode(message.trackId, message.panMode);
        break;
      case 'syncTrackStripDualPan':
        this.native.setTrackStripDualPan(message.trackId, message.leftPan, message.rightPan);
        break;
      case 'syncTrackStripSurroundPan':
        this.native.setTrackStripSurroundPan(message.trackId, message.pan);
        break;
      case 'syncBusStripPan':
        this.native.setBusStripPan(message.busId, message.pan);
        break;
      case 'syncBusStripPanLaw':
        this.native.setBusStripPanLaw(message.busId, message.panLaw);
        break;
      case 'syncBusStripPanMode':
        this.native.setBusStripPanMode(message.busId, message.panMode);
        break;
      case 'syncBusStripDualPan':
        this.native.setBusStripDualPan(message.busId, message.leftPan, message.rightPan);
        break;
      case 'syncTrackStripChannelDelaySamples':
        this.native.setTrackStripChannelDelaySamples(message.trackId, message.delaySamples);
        break;
      case 'syncBuiltinInstrument':
        this.engine.setBuiltinInstrument(
          message.destinationId,
          message.config as Parameters<RealtimeEngine['setBuiltinInstrument']>[1],
        );
        break;
      case 'syncSynthInstrument':
        this.engine.setSynthInstrument(message.destinationId, message.patch as SynthPatch);
        break;
      case 'syncArticulation':
        this.engine.setArticulation(message.destinationId, message.channel, message.articulation);
        break;
      case 'syncParamSmoothing':
        this.engine.setParamSmoothingMs(message.smoothingMs);
        break;
      case 'syncLoadSoundFont':
        this.engine.loadSoundFont(message.data);
        break;
      case 'syncSf2Instrument':
        this.engine.setSf2Instrument(message.destinationId, message.config);
        break;
      case 'syncMidiFx':
        this.engine.setMidiFx(message.destinationId, message.config ?? {});
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
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiNoteOff':
        this.engine.pushMidiNoteOff(
          message.destinationId,
          message.group,
          message.channel,
          message.note,
          message.velocity,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiCc':
        this.engine.pushMidiCc(
          message.destinationId,
          message.group,
          message.channel,
          message.controller,
          message.value,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiPitchBend':
        this.engine.pushMidiPitchBend(
          message.destinationId,
          message.group,
          message.channel,
          message.data0,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiChannelPressure':
        this.engine.pushMidiChannelPressure(
          message.destinationId,
          message.group,
          message.channel,
          message.data0,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiPolyPressure':
        this.engine.pushMidiPolyPressure(
          message.destinationId,
          message.group,
          message.channel,
          message.data0,
          message.data1,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiUmp':
        this.engine.pushMidiUmp(
          message.destinationId,
          message.words,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiSysex':
        this.engine.pushMidiSysex(
          message.destinationId,
          message.data,
          this.trackRenderFrame(message),
        );
        break;
      case 'syncMidiPanic':
        this.engine.pushMidiPanic(wireRenderFrame(message.renderFrame));
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
        this.engine.bindMidiCc(message.binding);
        break;
      case 'syncMidiInputNoteOn':
      case 'syncMidiInputNoteOff':
      case 'syncMidiInputCc':
      case 'syncMidiInputPitchBend':
      case 'syncMidiInputChannelPressure':
      case 'syncMidiInputPolyPressure':
        if (message.contextTimeSec !== undefined) {
          this.native.pushMidiInputUmpAtRenderFrame(
            midi1InputWords(message),
            this.contextTimeRenderFrame(message.contextTimeSec),
          );
        } else {
          pushMidiInputEvent(this.engine, message);
        }
        break;
      case 'syncMidiInputUmp':
        if (message.contextTimeSec !== undefined) {
          this.native.pushMidiInputUmpAtRenderFrame(
            message.words,
            this.contextTimeRenderFrame(message.contextTimeSec),
          );
        } else {
          this.engine.pushMidiInputUmp(message.words, message.offsetInBlockSamples);
        }
        break;
    }
  }

  private receiveCaptureRequest(message: SonareEngineCaptureRequestMessage): void {
    this.receiveOrdered(message, () => {
      try {
        this.settleCommands();
        if (message.op === 'status') {
          const status = this.engine.captureStatus();
          this.transport?.postMessage?.({
            type: 'captureResponse',
            requestId: message.requestId,
            ok: true,
            status: {
              capturedFrames: status.capturedFrames,
              droppedFrames: status.droppedFrames,
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
          error: errorText(error),
          failure: failurePayload(error),
        } satisfies SonareEngineCaptureResponseMessageInternal);
      }
    });
  }

  private receiveTransportRequest(message: SonareEngineTransportRequestMessage): void {
    this.receiveOrdered(message, () => {
      try {
        this.settleCommands();
        this.transport?.postMessage?.({
          type: 'transportResponse',
          requestId: message.requestId,
          ok: true,
          state: this.engine.transportState(),
        } satisfies SonareEngineTransportResponseMessage);
      } catch (error) {
        this.transport?.postMessage?.({
          type: 'transportResponse',
          requestId: message.requestId,
          ok: false,
          error: errorText(error),
          failure: failurePayload(error),
        } satisfies SonareEngineTransportResponseMessage);
      }
    });
  }

  /**
   * The render frame rendered at `contextTimeSec`, through the last quantum head's render-frame ↔
   * context-frame pair, for live input and track-addressed events alike. A frame already rendered
   * is late: the event plays at the next block's start and is counted.
   */
  private contextTimeRenderFrame(contextTimeSec: number): number {
    const nextRenderFrame = this.native.renderFrame();
    // Before the first quantum the two clocks are read as they stand.
    const headRender = this.headRenderFrame ?? nextRenderFrame;
    const headContext =
      this.headRenderFrame === undefined ? scopeCurrentFrame() : this.headContextFrame;
    const target = headRender + Math.round(contextTimeSec * this.sampleRate) - headContext;
    if (target < nextRenderFrame) {
      this.lateMidiEventCount++;
      return nextRenderFrame;
    }
    return target;
  }

  /** The render frame a track-addressed push names: its context time when given, else its wire frame. */
  private trackRenderFrame(message: {
    renderFrame: number;
    contextTimeSec?: number;
  }): number | undefined {
    return message.contextTimeSec !== undefined
      ? this.contextTimeRenderFrame(message.contextTimeSec)
      : wireRenderFrame(message.renderFrame);
  }

  private receiveReadRequest(message: SonareEngineReadRequestMessage): void {
    this.receiveOrdered(message, () => {
      try {
        this.settleCommands();
        const value =
          message.op === 'lateMidiEventCount'
            ? this.lateMidiEventCount
            : message.op === 'articulation'
              ? this.engine.articulation(message.destinationId, message.channel)
              : this.engine.legatoFallbackCount(message.destinationId);
        this.transport?.postMessage?.({
          type: 'engineReadResponse',
          requestId: message.requestId,
          ok: true,
          value,
        } satisfies SonareEngineReadResponseMessage);
      } catch (error) {
        this.transport?.postMessage?.({
          type: 'engineReadResponse',
          requestId: message.requestId,
          ok: false,
          error: errorText(error),
          failure: failurePayload(error),
        } satisfies SonareEngineReadResponseMessage);
      }
    });
  }

  private receiveInsertGainReductionRequest(
    message: SonareEngineInsertGainReductionRequestMessage,
  ): void {
    this.receiveOrdered(message, () => {
      try {
        this.settleCommands();
        this.transport?.postMessage?.({
          type: 'insertGainReductionResponse',
          requestId: message.requestId,
          ok: true,
          values: this.engine.meterTargetInsertGainReduction(message.targetId),
        } satisfies SonareEngineInsertGainReductionResponseMessage);
      } catch (error) {
        this.transport?.postMessage?.({
          type: 'insertGainReductionResponse',
          requestId: message.requestId,
          ok: false,
          error: errorText(error),
          failure: failurePayload(error),
        } satisfies SonareEngineInsertGainReductionResponseMessage);
      }
    });
  }

  /** Releases the engine and its paged clip providers; later `process()` calls return `false`. Safe to call twice. */
  destroy(): void {
    if (!this.closed) {
      this.clearPagedClipProviders();
      this.engine.destroy();
      this.closed = true;
    }
  }

  /** Same as {@link destroy}, for `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private drainCommands(limit = DRAIN_LIMIT_PER_QUANTUM): void {
    if (!this.commandRing) {
      return;
    }
    for (let i = 0; i < limit; i++) {
      const command = this.heldCommand ?? popSonareEngineCommandRingBuffer(this.commandRing);
      if (!command) {
        return;
      }
      // Serial-number order, so the comparison survives the uint32 sequence wrapping.
      if (
        command.syncSequence !== undefined &&
        ((command.syncSequence - this.syncSequence) | 0) > 0
      ) {
        this.heldCommand = command;
        return;
      }
      this.heldCommand = undefined;
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
        graphLatencySamples: 0,
        value: Number(command.type),
      });
    }
  }

  private applyCommand(command: SonareEngineCommandRecord): void {
    const renderFrame = wireRenderFrame(command.renderFrame);
    switch (command.type) {
      case SonareEngineCommandType.SetParam:
        // paramId is carried in targetId, the new value in argFloat (matches the
        // SonareEngine.setParam producer). renderFrame is the render frame.
        this.engine.setParameter(
          Math.trunc(Number(requireCommandOperand(command, 'targetId'))),
          Number(requireCommandOperand(command, 'argFloat')),
          renderFrame,
        );
        break;
      case SonareEngineCommandType.SetParamSmoothed:
        this.engine.setParameter(
          Math.trunc(Number(requireCommandOperand(command, 'targetId'))),
          Number(requireCommandOperand(command, 'argFloat')),
          renderFrame,
          { smoothed: true },
        );
        break;
      case SonareEngineCommandType.TransportPlay:
        this.engine.play(renderFrame);
        break;
      case SonareEngineCommandType.TransportStop:
        this.engine.stop(renderFrame);
        break;
      case SonareEngineCommandType.TransportSeekSample:
        this.engine.seekSample(Number(requireCommandOperand(command, 'argInt')), renderFrame);
        break;
      case SonareEngineCommandType.TransportSeekPpq:
        this.engine.seekPpq(Number(requireCommandOperand(command, 'argFloat')), renderFrame);
        break;
      case SonareEngineCommandType.SetLoop:
        this.engine.setLoop({
          startPpq: Number(requireCommandOperand(command, 'argFloat')),
          endPpq: Number(requireCommandOperand(command, 'argFloat2')),
          enabled: requireCommandOperand(command, 'targetId') !== 0,
        });
        break;
      case SonareEngineCommandType.ArmRecord:
        this.engine.armCapture({ armed: Boolean(command.argInt) });
        break;
      case SonareEngineCommandType.Punch: {
        // Both endpoints already arrive as samples (see SonareEngine.punch);
        // do NOT re-scale by sampleRate. A fractional out point is refused
        // (reported as InvalidCommand telemetry), never rounded.
        const outSample = Number(requireCommandOperand(command, 'argFloat'));
        if (!Number.isSafeInteger(outSample)) {
          throw refuse(
            `punch out sample must be an integer in [${-Number.MAX_SAFE_INTEGER}, ${Number.MAX_SAFE_INTEGER}]`,
          );
        }
        this.native.setCapturePunch(
          Number(requireCommandOperand(command, 'argInt')),
          outSample,
          true,
        );
        break;
      }
      case SonareEngineCommandType.SetMetronome:
        // Metronome config (beatGain/accentGain/clickSamples/clickSec) is
        // delivered out-of-band via the 'syncMetronome' message so it carries
        // the caller's full config; the command only toggles enabled state as a
        // sample-aligned fallback.
        this.engine.setMetronome({
          enabled: Boolean(command.argInt),
          beatGain: this.metronomeConfig.beatGain,
          accentGain: this.metronomeConfig.accentGain,
          // An unset length is 0 here; the engine refuses an explicit 0, so it is left out.
          ...(this.metronomeConfig.clickSamples > 0
            ? { clickSamples: this.metronomeConfig.clickSamples }
            : {}),
          ...(this.metronomeConfig.clickSeconds > 0
            ? { clickSec: this.metronomeConfig.clickSeconds }
            : {}),
        });
        break;
      case SonareEngineCommandType.SeekMarker:
        // The realtime engine's markers are kept in sync via 'syncMarkers'
        // (RtPublisher-style swap), so a queued kSeekMarker resolves correctly.
        this.engine.seekMarker(
          Math.trunc(Number(requireCommandOperand(command, 'targetId'))),
          renderFrame,
        );
        break;
      case SonareEngineCommandType.SetSoloMute:
        this.native.setSoloMute(
          Math.trunc(Number(requireCommandOperand(command, 'targetId'))),
          (Number(requireCommandOperand(command, 'argInt')) & 0x2) !== 0,
          (Number(requireCommandOperand(command, 'argInt')) & 0x1) !== 0,
          renderFrame,
        );
        break;
      case SonareEngineCommandType.SetTrackMonitorMode: {
        const rawMode = command.argInt;
        const mode = typeof rawMode === 'bigint' ? Number(rawMode) : rawMode;
        if (typeof mode !== 'number' || !Number.isSafeInteger(mode) || mode < 0 || mode > 2) {
          throw refuse(`Invalid track monitor mode: ${String(rawMode)}`);
        }
        const laneIndex = command.targetId;
        if (
          typeof laneIndex !== 'number' ||
          !Number.isSafeInteger(laneIndex) ||
          laneIndex < 0 ||
          laneIndex > UINT32_MAX
        ) {
          throw refuse(`Invalid track monitor lane index: ${String(laneIndex)}`);
        }
        this.native.setTrackMonitorMode(laneIndex, mode, renderFrame);
        break;
      }
      case SonareEngineCommandType.ResetMasterLoudnessMeter:
        this.engine.resetMasterLoudnessMeter(renderFrame);
        break;
      case SonareEngineCommandType.ResetProcessorState:
        this.engine.resetProcessorState(renderFrame);
        break;
      default:
        this.publishTelemetryRecord({
          type: SonareEngineTelemetryType.Error,
          error: SonareEngineTelemetryError.UnknownTarget,
          renderFrame: 0,
          timelineSample: 0,
          audibleTimelineSample: 0,
          graphLatencySamples: 0,
          value: Number(command.type),
        });
        break;
    }
  }

  private publishTelemetry(): void {
    const ring = this.telemetryRing;
    if (ring) {
      for (
        let count = 0;
        count < DRAIN_LIMIT_PER_QUANTUM && this.native.popTelemetryToScratch();
        count++
      ) {
        this.writeTelemetryScratch(ring);
      }
      return;
    }
    for (const item of this.engine.drainTelemetry(DRAIN_LIMIT_PER_QUANTUM)) {
      this.publishTelemetryRecord(telemetryFromEngine(item));
    }
  }

  private writeTelemetryScratch(ring: EngineTelemetryRingBuffer): void {
    const header = ring.header;
    const view = ring.view;
    const slotStates = ring.slotStates;
    const publicationWrapped = ring.publicationWrapped;
    const capacity = ring.capacity;
    // All facade calls and checked conversions happen before the claim. The
    // native scratch remains untouched by the fixed primitive store region.
    const type = +this.native.telemetryScratchType();
    const error = +this.native.telemetryScratchError();
    const renderFrame = toSafeInteger(this.native.telemetryScratchRenderFrame(), 0);
    const timelineSample = toSafeInteger(this.native.telemetryScratchTimelineSample(), 0);
    const scratch = this.native;
    const audibleTimelineSample = toSafeInteger(scratch.telemetryScratchAudibleTimelineSample(), 0);
    const graphLatencySamplesQ8 = +this.native.telemetryScratchGraphLatencySamplesQ8();
    const value = +this.native.telemetryScratchValue();
    const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
    if (writeIndex === undefined) {
      Atomics.add(header, 4, 1);
      return;
    }
    const offset = (writeIndex % capacity) * SONARE_ENGINE_TELEMETRY_RECORD_BYTES;
    view.setUint32(offset, type, true);
    view.setUint32(offset + 4, error, true);
    writePreparedInt64Words(view, offset + 8, renderFrame);
    writePreparedInt64Words(view, offset + 16, timelineSample);
    writePreparedInt64Words(view, offset + 24, audibleTimelineSample);
    view.setInt32(offset + 32, graphLatencySamplesQ8, true);
    view.setUint32(offset + 36, value, true);
    view.setUint32(offset + 40, 0, true);
    view.setUint32(offset + 44, 0, true);
    commitSonareEngineTelemetryRingWrite(
      header,
      slotStates,
      publicationWrapped,
      capacity,
      writeIndex,
    );
  }

  private publishTelemetryRecord(record: EngineTelemetryRecord): void {
    if (this.telemetryRing) {
      writeSonareEngineTelemetryRingBuffer(this.telemetryRing, record);
      return;
    }
    this.transport?.postMessage?.(record);
  }

  // Drains the engine meter telemetry queue into the meter ring / transport.
  //
  // The engine's meter drain is the only one: it carries every plane of every
  // target (stereo is planes 0/1), so both transports deliver surround meters
  // and nothing else may pop this queue on a worklet-driven engine.
  private publishMeters(): void {
    if (this.meterIntervalFrames <= 0 || (!this.transport && !this.meterRing)) {
      return;
    }
    if (this.meterRing) {
      for (
        let count = 0;
        count < DRAIN_LIMIT_PER_QUANTUM && this.native.popMeterTelemetryToScratch();
        count++
      ) {
        if (
          !this.meterDue(
            this.native.meterScratchTargetId(),
            Number(this.native.meterScratchRenderFrame()),
          )
        ) {
          continue;
        }
        this.writeMeterScratch(this.meterRing);
      }
      return;
    }
    const delivered: WorkletMeterSnapshot[] = [];
    const newestByTarget = new Map<number, WorkletMeterSnapshot>();
    for (const item of this.engine.drainMeterTelemetry(DRAIN_LIMIT_PER_QUANTUM)) {
      const meter = meterFromEngine(item);
      if (!this.meterDue(meter.targetId, meter.frame)) {
        continue;
      }
      delivered.push(meter);
      newestByTarget.set(meter.targetId, meter);
    }
    for (const meter of delivered) {
      // The board holds only the latest block, so only the newest record per
      // target in this drain can carry it; the ring layout is fixed and cannot.
      if (newestByTarget.get(meter.targetId) === meter) {
        meter.insertGainReductionDb = this.engine.meterTargetInsertGainReduction(meter.targetId);
      }
      // The ring branch returned above, so this is the structured-clone
      // fallback. `onMeter` and `postMessage` are alternative channels for the
      // same record, not a broadcast pair: the AudioWorklet registration
      // resolves BOTH to `port.postMessage`, so calling them in turn delivered
      // every record twice and doubled the host's peak-hold decay rate. Take
      // the first channel the transport offers.
      if (this.transport?.onMeter) {
        this.transport.onMeter(meter);
      } else {
        this.transport?.postMessage?.(meter);
      }
    }
  }

  // Interval gate per target, so one busy target cannot hold back another's cadence.
  private meterDue(targetId: number, frame: number): boolean {
    const slot = Math.min(targetId, METER_TARGET_SLOTS - 1);
    const last = this.lastMeterFrameBySlot[slot];
    if (frame !== last && frame - last < this.meterIntervalFrames) {
      return false;
    }
    this.lastMeterFrameBySlot[slot] = frame;
    return true;
  }

  private writeMeterScratch(ring: SharedMeterRingWriter): void {
    const header = ring.header;
    const records = ring.records;
    const slotStates = ring.slotStates;
    const capacity = ring.capacity;
    const scratch = this.meterRingScratch;
    const frame = Number(this.native.meterScratchRenderFrame());
    scratch[0] = encodeFrameLo(frame);
    scratch[1] = encodeFrameHi(frame);
    scratch[2] = +this.native.meterScratchTargetId();
    for (let field = 0; field < 11; field++) {
      scratch[3 + field] = +this.native.meterScratchValue(field);
    }
    scratch[14] = +this.native.meterScratchInputPeakDbL();
    scratch[15] = +this.native.meterScratchInputPeakDbR();
    const channelCount = +this.native.meterScratchValue(METER_SCRATCH_CHANNEL_COUNT);
    scratch[SONARE_METER_RING_CHANNEL_COUNT_OFFSET] = channelCount;
    const planes = channelCount > 2 ? Math.min(channelCount, SONARE_METER_RING_MAX_CHANNELS) : 0;
    for (let kind = 0; kind < METER_PLANE_KINDS && planes > 0; kind++) {
      const out = SONARE_METER_RING_PLANES_OFFSET + kind * SONARE_METER_RING_MAX_CHANNELS;
      const field = METER_SCRATCH_PLANES + kind * SONARE_METER_RING_MAX_CHANNELS;
      for (let ch = 0; ch < planes; ch++) {
        scratch[out + ch] = +this.native.meterScratchValue(field + ch);
      }
    }
    const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
    if (writeIndex === undefined) {
      return;
    }
    const offset = (writeIndex % capacity) * SONARE_METER_RING_RECORD_FLOATS;
    for (let field = 0; field < SONARE_METER_RING_PLANES_OFFSET; field++) {
      records[offset + field] = scratch[field];
    }
    for (let kind = 0; kind < METER_PLANE_KINDS && planes > 0; kind++) {
      const out = offset + SONARE_METER_RING_PLANES_OFFSET + kind * SONARE_METER_RING_MAX_CHANNELS;
      const source = SONARE_METER_RING_PLANES_OFFSET + kind * SONARE_METER_RING_MAX_CHANNELS;
      for (let ch = 0; ch < planes; ch++) {
        records[out + ch] = scratch[source + ch];
      }
    }
    commitOverwriteWrite(header, slotStates, capacity, writeIndex);
  }

  // writeMeterScratch is the only ring writer; it reads the engine's scratch registers directly.
  //
  // An overwrite ring keeps no drop count: readers detect overrun from the
  // cursor distance, so header slot 3 is left at its initial 0.

  // Drains the engine's scope producer (FFT spectrum + goniometer points) into
  // the lock-free SAB scope ring or the bounded postMessage fallback. The SAB
  // path remains allocation-free; postMessage materialises only records that
  // are ready to publish.
  private publishScope(): void {
    if (!this.scopeEnabled) {
      return;
    }
    const ring = this.scopeRing;
    const transport = this.transport;
    for (
      let count = 0;
      count < DRAIN_LIMIT_PER_QUANTUM && this.native.popScopeTelemetryToScratch();
      count++
    ) {
      if (ring) {
        this.writeScopeScratch(ring);
      } else {
        this.postScopeScratch(transport);
      }
    }
  }

  private postScopeScratch(transport: TypedWorkletTransport | undefined): void {
    if (!transport?.postMessage) {
      return;
    }
    const bandCount = Math.min(
      SONARE_SCOPE_MAX_BANDS,
      Math.max(0, Math.trunc(Number(this.native.scopeScratchBandCount()))),
    );
    const pointCount = Math.min(
      SONARE_SCOPE_MAX_POINTS,
      Math.max(0, Math.trunc(Number(this.native.scopeScratchPointCount()))),
    );
    const bands = new Float32Array(bandCount);
    for (let i = 0; i < bandCount; i++) {
      bands[i] = this.native.scopeScratchBand(i);
    }
    const points = new Float32Array(pointCount * 2);
    for (let i = 0; i < pointCount; i++) {
      points[2 * i] = this.native.scopeScratchPointLeft(i);
      points[2 * i + 1] = this.native.scopeScratchPointRight(i);
    }
    const scope: WorkletScopeSnapshot = {
      type: 'scope',
      targetId: this.native.scopeScratchTargetId(),
      frame: Number(this.native.scopeScratchRenderFrame()),
      bands,
      points,
    };
    transport.postMessage(scope, [bands.buffer, points.buffer]);
  }

  private writeScopeScratch(ring: SharedScopeRingWriter): void {
    const header = ring.header;
    const records = ring.records;
    const slotStates = ring.slotStates;
    const capacity = ring.capacity;
    const recordFloats = ring.recordFloats;
    const ringBands = ring.bands;
    const ringMaxPoints = ring.maxPoints;
    const bandScratch = this.scopeRingBandScratch;
    const pointScratch = this.scopeRingPointScratch;
    const frame = Number(this.native.scopeScratchRenderFrame());
    const frameLo = encodeFrameLo(frame);
    const frameHi = encodeFrameHi(frame);
    const targetId = +this.native.scopeScratchTargetId();
    const bandCount = Math.min(ringBands, +this.native.scopeScratchBandCount());
    const pointCount = Math.min(ringMaxPoints, +this.native.scopeScratchPointCount());
    if (bandCount > SONARE_SCOPE_MAX_BANDS || pointCount > SONARE_SCOPE_MAX_POINTS) {
      throw refuse('scope scratch exceeds the processor scratch capacity.');
    }
    for (let i = 0; i < bandCount; i++) {
      bandScratch[i] = +this.native.scopeScratchBand(i);
    }
    for (let i = 0; i < pointCount; i++) {
      pointScratch[2 * i] = +this.native.scopeScratchPointLeft(i);
      pointScratch[2 * i + 1] = +this.native.scopeScratchPointRight(i);
    }
    const writeIndex = tryBeginOverwriteWrite(header, slotStates, capacity);
    if (writeIndex === undefined) {
      return;
    }
    const base = (writeIndex % capacity) * recordFloats;
    records[base] = frameLo;
    records[base + 1] = frameHi;
    records[base + 2] = targetId;
    records[base + 3] = bandCount;
    records[base + 4] = pointCount;
    const bandsBase = base + SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS;
    for (let i = 0; i < bandCount; i++) {
      records[bandsBase + i] = bandScratch[i];
    }
    const pointsBase = bandsBase + ringBands;
    for (let i = 0; i < pointCount; i++) {
      records[pointsBase + 2 * i] = pointScratch[2 * i];
      records[pointsBase + 2 * i + 1] = pointScratch[2 * i + 1];
    }
    commitOverwriteWrite(header, slotStates, capacity, writeIndex);
  }

  // Drains queued external-MIDI events (already lowered to MIDI 1.0 bytes) and
  // forwards them to the main thread for delivery to Web MIDI output ports.
  // One batch per render block; skipped entirely when nothing is queued, so an
  // all-internal project never allocates or posts here.
  private publishExternalMidi(): void {
    const ring = this.externalMidiRing;
    if (ring) {
      for (
        let count = 0;
        count < EXTERNAL_MIDI_DRAIN_LIMIT && this.native.popExternalMidiToScratch();
        count++
      ) {
        // A full ring leaves the event in the scratch for the next quantum; only
        // a record the ring refuses as malformed is consumed and counted.
        if (!sonareExternalMidiRingHasRoom(ring)) {
          break;
        }
        pushSonareExternalMidiRingBuffer(
          ring,
          this.native.externalMidiScratchDestinationId(),
          Number(this.native.externalMidiScratchRenderFrame()),
          this.native.externalMidiScratchByteWord(),
          this.native.externalMidiScratchByteCount(),
        );
        this.native.consumeExternalMidiScratch();
      }
      return;
    }
    if (!this.transport?.postMessage) {
      return;
    }
    if (this.engine.externalMidiPendingCount === 0) {
      return;
    }
    const events = this.engine.drainExternalMidi(EXTERNAL_MIDI_DRAIN_LIMIT);
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
  private publishClipPageRequests(frames: number): void {
    const ring = this.clipPageRequestRing;
    const transport = this.transport;
    if (!ring && !transport?.postMessage) {
      return;
    }
    let count = 0;
    let drained = 0;
    while (drained < this.clipPageRequestClipIds.length) {
      drained += 1;
      // The scratch pop avoids embind materialising an object per page miss.
      if (!this.native.popClipPageRequestToScratch()) {
        break;
      }
      const clipId = this.native.clipPageRequestScratchClipId();
      const sample = Number(this.native.clipPageRequestScratchSample());
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
    const overflowCount = this.engine.clipPageRequestOverflowCount;
    // The native tally is 64-bit; the transport word carries the delta modulo
    // 2^32.
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
    if (!transport?.postMessage) {
      return;
    }
    this.clipPageRequestFrameClock += frames;
    // Post when the missing set changed or the repost interval elapsed; the page repeats every block until it lands.
    let changed = count !== this.postedClipPageRequestCount;
    for (let i = 0; i < count && !changed; ++i) {
      let found = false;
      for (let j = 0; j < count; ++j) {
        if (
          this.postedClipPageRequestClipIds[j] === this.clipPageRequestClipIds[i] &&
          this.postedClipPageRequestPageIndices[j] === this.clipPageRequestPageIndices[i]
        ) {
          found = true;
          break;
        }
      }
      changed = !found;
    }
    const due =
      count > 0 &&
      this.clipPageRequestFrameClock - this.lastClipPageRequestPostFrame >=
        this.clipPageRepostFrames;
    if (!changed && !due && dropped === 0) {
      return;
    }
    this.lastClipPageRequestPostFrame = this.clipPageRequestFrameClock;
    for (let i = 0; i < count; ++i) {
      this.postedClipPageRequestClipIds[i] = this.clipPageRequestClipIds[i];
      this.postedClipPageRequestPageIndices[i] = this.clipPageRequestPageIndices[i];
      const entry = this.clipPageRequestPayloadEntries[i];
      entry.clipId = this.clipPageRequestClipIds[i];
      entry.pageIndex = this.clipPageRequestPageIndices[i];
    }
    this.postedClipPageRequestCount = count;
    if (count === 0 && dropped === 0) {
      return;
    }
    const requests = this.clipPageRequestPayloadArrays[count];
    if (dropped > 0) {
      this.clipPageRequestMessageWithDropped.requests = requests;
      this.clipPageRequestMessageWithDropped.dropped = dropped;
      transport.postMessage(this.clipPageRequestMessageWithDropped);
    } else {
      this.clipPageRequestMessage.requests = requests;
      transport.postMessage(this.clipPageRequestMessage);
    }
  }

  /**
   * Applies a clip delta one clip at a time, so clips it does not name keep
   * their storage instead of being copied again. A refusal part-way restores
   * the published set from the live mirror and rethrows.
   */
  private applyClipDelta(upserts: readonly EngineClip[], removeIds: readonly number[]): void {
    try {
      for (const clipId of removeIds) {
        if (this.liveClips.has(clipId)) {
          this.engine.removeClip(clipId);
        }
      }
      for (const clip of upserts) {
        if (clip.id !== undefined) {
          this.engine.upsertClip(clip);
        }
      }
    } catch (error) {
      this.engine.setClips(Array.from(this.liveClips.values()));
      throw error;
    }
  }

  /** The provider of `clipId` when `streamKey` names its owner, else undefined. */
  private ownedPagedClipProvider(
    clipId: number,
    streamKey: number | undefined,
  ): ClipPageProvider | undefined {
    const entry = this.pagedClipProviders.get(clipId);
    return entry !== undefined && entry.streamKey === streamKey ? entry.provider : undefined;
  }

  private removePagedClipProvider(clipId: number): void {
    const entry = this.pagedClipProviders.get(clipId);
    if (entry !== undefined) {
      entry.provider.destroy();
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
  ): EngineTelemetryRingBuffer {
    return engineTelemetryRingFromSharedBuffer(sharedBuffer, fallbackCapacity);
  }
}
