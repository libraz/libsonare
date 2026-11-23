import type { ChannelLayout } from '../audio.js';
import {
  type ClipPagePageRequest,
  ClipPageStreamer,
  type OpfsClipStreamOptions,
} from '../clip_page_streamer.js';
import type {
  Articulation,
  BuiltinSynthConfig,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCaptureStatus,
  EngineClip,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineMarker,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineTelemetry,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  EqBandInput,
  MixSceneBusInput,
  MixSceneStripInput,
  PanLaw,
  PanMode,
  ProjectMidiCcBinding,
  Sf2InstrumentConfig,
  SidechainCheck,
  SynthPatch,
  UmpWords,
} from '../index.js';
import { RealtimeEngine } from '../index.js';
import { opfsClipPageProviderFactoryFor } from '../internal/clip_page_native.js';
import { entryClass, invalidState, refuse } from '../internal/entry.js';
import { assertHandleLive } from '../internal/handle_guard.js';
import { stringifyWithinBudget } from '../internal/list_budget.js';
import { packMidi2ProgramRequest } from '../internal/midi_program_pack.js';
import { mixSceneBusToNative, mixSceneStripToNative } from '../internal/mix_scene.js';
import { boundedUmpWords } from '../internal/ump_events.js';
import type { SonareEngineSyncMessage } from '../internal/worklet_messages.js';
import {
  createOpfsClipPageProvider,
  type OpfsClipPageProviderBinding,
} from '../opfs_clip_pages.js';
import { OwnerEpoch } from '../owner_epoch.js';
import type { SurroundPan } from '../public_types.js';
import type {
  CaptureArmState,
  CaptureOptions,
  EngineLoop,
  EngineMarkerInput,
  ExternalMidiEvent,
  InsertBypassState,
  InsertParamValue,
  MidiFxConfig,
  SidechainSource,
  TrackSoloMuteState,
} from '../public_types_engine.js';
import type { MidiBank } from '../public_types_midi_messages.js';
import type {
  EngineSyncErrorMessage,
  SonareRealtimeEngineNodeCapabilities,
} from '../public_types_worklet.js';
import type { ClipPageProvider } from '../realtime_engine.js';
import { resolveRenderFrame } from '../validation.js';
import type { EngineAutomationContext } from './engine-automation.js';
import * as automation from './engine-automation.js';
import type { EngineCaptureContext } from './engine-capture-facade.js';
import * as capture from './engine-capture-facade.js';
import type { EngineClipContext } from './engine-clips.js';
import * as clips from './engine-clips.js';
import { commitCommand, commitSync, type EngineCommitContext } from './engine-commit.js';
import type {
  EngineExportContext,
  OpfsExportSource,
  SonareEngineRenderOfflineRequest,
} from './engine-export.js';
import * as exporter from './engine-export.js';
import type { EngineMarkerContext } from './engine-markers.js';
import * as markerOps from './engine-markers.js';
import {
  type MidiInputPushName,
  resolveMidiInputTiming,
  type SonareEngineMidiTimingOptions,
} from './engine-midi-input.js';
import type { EngineMixerContext, InsertParamOverrideMap } from './engine-mixer-facade.js';
import * as mixer from './engine-mixer-facade.js';
import { SonareRealtimeEngineNode } from './engine-node.js';
import {
  buildTransportFacade,
  type CaptureSyncConfig,
  type SonareEngineTransportFacade,
} from './engine-offline.js';
import type { SonareEngineOptions, SuspendableAudioContext } from './engine-options.js';
import type {
  EngineParameterContext,
  SonareEngineSetParamOptions,
} from './engine-parameter-facade.js';
import * as parameter from './engine-parameter-facade.js';
import type { EngineStripContext } from './engine-strips.js';
import * as strips from './engine-strips.js';
import { resolveParamId, resolveTargetId } from './engine-sync.js';
import type { EngineTempoContext } from './engine-tempo-facade.js';
import * as tempo from './engine-tempo-facade.js';
import { requireChannelCount, resolveContextSampleRate } from './guards.js';
import {
  SonareEngineCommandType,
  type WorkletMeterSnapshot,
  type WorkletScopeSnapshot,
} from './protocol.js';
import { AUDIO_WORKLET_RENDER_QUANTUM } from './render_quantum.js';

const MAX_PENDING_WORKLET_CLIP_PAGE_REQUESTS = 256;

function transferableAudioBuffers(channels: readonly Float32Array[]): Transferable[] {
  const transfers: ArrayBuffer[] = [];
  const seen = new Set<ArrayBuffer>();
  for (const channel of channels) {
    const buffer = channel.buffer;
    // SharedArrayBuffer is cloneable but cannot appear in a transfer list. A
    // worker may intentionally return SAB-backed pages, so transfer only plain,
    // distinct ArrayBuffers and let structured clone share SABs by reference.
    if (buffer instanceof ArrayBuffer && !seen.has(buffer)) {
      seen.add(buffer);
      transfers.push(buffer);
    }
  }
  return transfers;
}

/**
 * The main-thread facade of a DAW engine running in an AudioWorklet.
 *
 * It shares one vocabulary with {@link RealtimeEngine}: the same method names,
 * target id first, the same request and config types, the same units. Every
 * edit applies to an offline mirror engine on this thread and is forwarded to
 * the worklet's live engine, so a refused edit changes neither.
 *
 * A track is addressed by its id (a number, or a numeric string naming it) and
 * the master strip by `'master'`. Setters return `void`; one that cannot queue
 * its command because the command ring is full throws a `SonareError` with
 * `ErrorCode.OutOfMemory`. Calls that need the worklet's answer return a
 * `Promise` because the audio thread cannot be read synchronously
 * ({@link transportState}, {@link captureStatus}, {@link capturedAudio},
 * {@link resetCapture}, {@link pollInsertGainReduction}); every other read is
 * answered synchronously from the offline mirror, which holds the same state.
 * The worklet sees every call in the order it was made, so `armCapture({ armed: false })`
 * followed by `configureCapture` or `captureStatus` needs no wait in between.
 * Argument refusals are `TypeError` / `RangeError`, as on {@link RealtimeEngine}.
 */
@entryClass('SonareEngine', [
  'mirrorMidiInput',
  'ensureWorkletClipStreamer',
  'startWorkletClipPagePolling',
  'pumpWorkletClipPages',
  'enqueueWorkletClipPageRequest',
  'popWorkletClipPageRequest',
  'workletClipStream',
  'mixerLanes',
  'syncMixer',
  'insertAutomationTargetKey',
  'rememberInsertAutomationId',
  'clearInsertAutomationLanes',
  'postSync',
  'flushOfflineMirror',
  'sendMirroredCommand',
  'mixerContext',
  'stripContext',
  'automationContext',
  'captureContext',
  'parameterContext',
  'tempoContext',
  'exportContext',
  'clipContext',
  'markerContext',
  'sendSmoothedParam',
  'commitContext',
  'resolveParamId',
  'syncParameters',
  'resolveTargetId',
  'ensureTrackLane',
  'ensureBus',
])
export class SonareEngine {
  /** The underlying AudioWorkletNode; connect it into the audio graph. */
  readonly node: AudioWorkletNode;
  /** Transport mode and feature support detected when the engine was created. */
  readonly capabilities: SonareRealtimeEngineNodeCapabilities;
  /** Transport control: play, stop, seek and loop, mirrored on the offline engine. */
  readonly transport: SonareEngineTransportFacade;
  private readonly realtimeNode: SonareRealtimeEngineNode;
  private readonly offlineEngine: RealtimeEngine;
  private readonly context: SuspendableAudioContext;
  private readonly sampleRate: number;
  private readonly offlineBlockSize: number;
  private readonly offlineNumChannels: number;
  private readonly automationLanes = new Map<number, EngineAutomationPoint[]>();
  private readonly insertAutomationIdsByTarget = new Map<string, Set<number>>();
  private readonly clips = new Map<number, EngineClip>();
  private readonly midiClips = new Map<number, EngineMidiClipSchedule>();
  private readonly markerStore = new Map<number, EngineMarker>();
  private readonly trackLaneIds: number[] = [];
  private readonly trackSends = new Map<number, EngineTrackSend[]>();
  private readonly trackOutputBus = new Map<number, number>();
  private readonly trackSourceChannelLayout = new Map<number, ChannelLayout>();
  private readonly laneSidechains = new Map<
    string,
    { trackId: number; insertIndex: number; sourceTrackId: number }
  >();
  private readonly busSidechains = new Map<
    string,
    { busId: number; insertIndex: number; sourceKind: number; sourceId: number }
  >();
  private readonly masterSidechains = new Map<
    number,
    { insertIndex: number; sourceKind: number; sourceId: number }
  >();
  private readonly buses: EngineBus[] = [];
  private readonly trackStripJson = new Map<number, string>();
  private readonly busStripJson = new Map<number, string>();
  private readonly insertParamOverrides: InsertParamOverrideMap = new Map();
  private masterStripJson: string | undefined;
  private captureConfig: CaptureSyncConfig | undefined;
  private tempoBpm = 120;
  private timeSignature = { numerator: 4, denominator: 4 };
  private tempoSegments: EngineTempoSegment[] = [{ startPpq: 0, bpm: 120 }];
  private timeSignatureSegments: EngineTimeSignatureSegment[] = [
    { startPpq: 0, numerator: 4, denominator: 4 },
  ];
  private latestTransportState: EngineTransportState | undefined;
  private nextClipId = 1;
  private nextMarkerId = 1;
  // One latest frontier per clip is sufficient: ClipPageStreamer expands it to
  // the bounded read window. A Map both coalesces repeated page misses and
  // places a hard cap on work queued while OPFS I/O is stalled.
  private readonly workletClipPageRequests = new Map<number, ClipPagePageRequest>();
  private readonly workletPageProviderClipIds = new Map<
    ClipPageProvider,
    { clipId: number; streamKey: number }
  >();
  private nextClipStreamKey = 1;
  private readonly opfsExportSources = new Map<ClipPageProvider, OpfsExportSource>();
  private readonly exportSession: { chunkOrigin: number | undefined } = { chunkOrigin: undefined };
  private workletClipStreamer: ClipPageStreamer | undefined;
  private workletClipPump: Promise<void> | undefined;
  private workletClipPagePollTimer: ReturnType<typeof setInterval> | undefined;
  private unsubscribeWorkletClipRequests: (() => void) | undefined;
  private readonly epoch = new OwnerEpoch('SonareEngine');

  private constructor(
    context: BaseAudioContext,
    realtimeNode: SonareRealtimeEngineNode,
    offlineEngine: RealtimeEngine,
    sampleRate: number,
    offlineBlockSize: number,
    offlineNumChannels: number,
  ) {
    this.context = context;
    this.realtimeNode = realtimeNode;
    this.offlineEngine = offlineEngine;
    this.node = realtimeNode.node;
    this.capabilities = realtimeNode.capabilities;
    this.sampleRate = sampleRate;
    this.offlineBlockSize = offlineBlockSize;
    this.offlineNumChannels = offlineNumChannels;
    this.unsubscribeWorkletClipRequests = this.realtimeNode.onClipPageRequests((message) => {
      for (const request of message.requests) {
        this.enqueueWorkletClipPageRequest(request);
      }
      this.pumpWorkletClipPages();
    });
    this.transport = buildTransportFacade({
      sampleRate: this.sampleRate,
      realtimeNode: this.realtimeNode,
      offlineEngine: this.offlineEngine,
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      sendCommand: (command) => this.sendMirroredCommand(command),
      setTempo: (bpm) => this.setTempo(bpm),
      setTempoSegments: (segments) => this.setTempoSegments(segments),
      setLoop: (loop) => this.setLoop(loop),
    });
  }

  /**
   * Creates the worklet node, waits for the processor to be ready and returns the engine.
   *
   * @param context Audio context that owns the node.
   * @param options Node options plus `offlineEngine`, `offlineBlockSize` and `offlineNumChannels` for the
   * offline mirror; the mirror block size defaults to `maxBlockSize`, then 128 frames.
   * @throws TypeError for an unknown option key or a wrong-typed option.
   * @throws RangeError for an out-of-range option.
   * @throws SonareError `AbiMismatch` on an engine ABI mismatch, `NotSupported` when `mode: 'sab'` is
   * requested without SharedArrayBuffer and Atomics, or the worklet's own error if it fails to start.
   */
  static async create(
    context: BaseAudioContext,
    options: SonareEngineOptions,
  ): Promise<SonareEngine> {
    const sampleRate = resolveContextSampleRate(options.sampleRate, context);
    const {
      offlineEngine: offlineEngineOption,
      offlineBlockSize,
      offlineNumChannels,
      ...nodeOptions
    } = options;
    const blockSize = offlineBlockSize ?? options.maxBlockSize ?? AUDIO_WORKLET_RENDER_QUANTUM;
    const numChannels = requireChannelCount(
      offlineNumChannels ?? options.numChannels,
      offlineNumChannels === undefined ? 'numChannels' : 'offlineNumChannels',
    );
    const realtimeNode = await SonareRealtimeEngineNode.create(context, nodeOptions);
    try {
      // Do not expose the facade while the registered processor is still
      // buffering messages. Long clip sync must either begin after the bridge
      // is ready or fail explicitly; it may never be silently truncated.
      await realtimeNode.ready;
    } catch (error) {
      await realtimeNode.destroy();
      throw error;
    }
    const offlineEngine =
      offlineEngineOption ?? new RealtimeEngine({ sampleRate, maxBlockSize: blockSize });
    const engine = new SonareEngine(
      context,
      realtimeNode,
      offlineEngine,
      sampleRate,
      blockSize,
      numChannels,
    );
    engine.syncParameters();
    return engine;
  }

  /** Suspends the audio context; resolves immediately when the engine is destroyed. */
  async suspend(): Promise<void> {
    if (this.epoch.closed) {
      return;
    }
    await this.context.suspend?.();
  }

  /** Resumes the audio context; resolves immediately when the engine is destroyed. */
  async resume(): Promise<void> {
    if (this.epoch.closed) {
      return;
    }
    await this.context.resume?.();
  }

  /** Sets one constant tempo, `bpm` in beats per minute (finite, greater than 0, at most 100000), on both engines. */
  setTempo(bpm: number): void {
    tempo.setTempo(this.tempoContext, bpm);
  }

  /** Replaces the tempo map with segments keyed by `startPpq` (quarter notes), on both engines. */
  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    tempo.setTempoSegments(this.tempoContext, segments);
  }

  /** Sets one constant time signature, on both engines. */
  setTimeSignature(numerator: number, denominator: number): void {
    tempo.setTimeSignature(this.tempoContext, numerator, denominator);
  }

  /** Replaces the time-signature map with segments keyed by position, on both engines. */
  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    tempo.setTimeSignatureSegments(this.tempoContext, segments);
  }

  /**
   * Sets the loop region. A loop wrap, seek or stop sends note-offs plus CC64=0,
   * CC121, CC123 and a centred pitch bend on every channel played since the last
   * reset. When playback next rolls, the controller, program, channel pressure
   * and pitch bend values the clips set before the new position are sent again,
   * so each loop pass and each seek starts from the state continuous playback
   * reaches.
   */
  setLoop(loop: EngineLoop): void {
    tempo.setLoop(this.tempoContext, loop);
  }

  /**
   * Sample at which a count-in of `bars` bars from `startSample` ends. Counting starts at the beginning
   * of the bar containing `startSample` (a mid-bar start gets a short first bar), and every counted bar has
   * the length of the time signature in effect at `startSample`. Returns `startSample` when `bars <= 0`.
   */
  countInEndSample(startSample: number, bars: number): number {
    return tempo.countInEndSample(this.tempoContext, startSample, bars);
  }

  /** Reads the transport from the worklet's live engine; the answer arrives asynchronously. */
  transportState(): Promise<EngineTransportState> {
    return tempo.transportState(this.tempoContext);
  }

  /** The latest {@link transportState} answer, or `undefined` before the first one. */
  cachedTransportState(): EngineTransportState | undefined {
    return tempo.cachedTransportState(this.tempoContext);
  }

  /**
   * Sets one registered parameter by id or name, at the next block head unless
   * `renderFrame` names the frame. `{ smoothed: true }` ramps to the value through
   * the engine smoother, as {@link RealtimeEngine.setParameter} does; the refusals
   * are that method's.
   */
  setParam(
    nodeId: string,
    param: string | number,
    value: number,
    options?: SonareEngineSetParamOptions,
  ): void {
    parameter.setParam(this.parameterContext, nodeId, param, value, options);
  }

  /**
   * Sets the ramp time (ms) of the engine-level smoother in the worklet; the
   * domain, default and refusals are {@link RealtimeEngine.setParamSmoothingMs}'s.
   */
  setParamSmoothingMs(smoothingMs: number): void {
    parameter.setParamSmoothingMs(this.parameterContext, smoothingMs);
  }

  /** Adds one breakpoint to the automation lane of a registered parameter. */
  scheduleParam(nodeId: string, param: string | number, point: EngineAutomationPoint): void {
    automation.scheduleParam(this.automationContext, nodeId, param, point);
  }

  /** Adds one breakpoint to an automation lane, addressed by lane id. */
  addAutomationPoint(laneId: string | number, point: EngineAutomationPoint): void {
    automation.addAutomationPoint(this.automationContext, laneId, point);
  }

  /**
   * Replaces the automation lane for `paramId` with the given breakpoints. An
   * empty array clears the lane; the points are defensively copied and sorted
   * by ppq before mirroring to the offline and live worklet engines.
   */
  setAutomationLane(paramId: number, points: ReadonlyArray<EngineAutomationPoint>): void {
    automation.setAutomationLane(this.automationContext, paramId, points);
  }

  /**
   * Returns the automation target id for a strip's fader, pan or width.
   *
   * The id addresses the engine's reserved mixer namespace, so it can be fed
   * straight to setAutomationLane to automate a fader or pan without
   * registering a parameter.
   *
   * @param target Track id (declares a mixer lane on first use) or 'master'.
   * @param kind Strip parameter to address; a track has no `'width'` (`RangeError`).
   * @returns Reserved engine parameter id for the strip parameter.
   */
  resolveStripAutomationId(target: string | number, kind: 'faderDb' | 'pan' | 'width'): number {
    return parameter.resolveStripAutomationId(this.parameterContext, target, kind);
  }

  /**
   * Returns the automation target id for a bus fader. The id names the bus, so
   * it keeps driving that bus across bus reorders and the removal of other
   * buses, and applies nothing once the bus is removed.
   *
   * @param busId Bus id (declares the mixer bus on first use).
   * @param paramName Bus parameter; only `'faderDb'`.
   * @returns Reserved engine parameter id for the bus fader gain (dB).
   */
  resolveBusAutomationId(busId: number, paramName: 'faderDb'): number {
    return parameter.resolveBusAutomationId(this.parameterContext, busId, paramName);
  }

  /**
   * Resolves a strip insert parameter (JSON-key name) to the reserved
   * insert-automation id fed straight to setAutomationLane. Declares a track's
   * mixer lane first (like resolveStripAutomationId). The offline engine and the
   * worklet's engine receive the same mixer configuration in the same order, so
   * they number insert ids identically and an id resolved here drives the
   * worklet.
   *
   * Track, bus and master insert ids share one lifetime rule: an id names its
   * strip by identity and the kind of processor in its slot (for
   * `effects.gsEfx`, its EFX type too), so it survives lane and bus reorders
   * and the removal of other strips, and stays valid until its track or bus is
   * removed or its slot comes to hold another kind of processor. After that it
   * applies nothing, its queued edits are dropped, and it is never reissued;
   * resolve again for the new processor. A change that would take the engine
   * past its 8192 insert-id entries is refused.
   *
   * @param target Track id (declares a mixer lane on first use) or 'master'.
   * @param insertIndex Index into the strip's combined insert sequence.
   * @param paramName Processor JSON-key parameter name.
   * @returns Reserved insert-automation id.
   * @throws RangeError when the strip, insert or key is unknown.
   */
  resolveStripInsertAutomationId(
    target: string | number,
    insertIndex: number,
    paramName: string,
  ): number {
    const id = parameter.resolveStripInsertAutomationId(
      this.parameterContext,
      target,
      insertIndex,
      paramName,
    );
    this.rememberInsertAutomationId(
      target === 'master'
        ? { kind: 'master' }
        : { kind: 'track', trackId: this.trackLaneIds[this.ensureTrackLane(target)] },
      id,
    );
    return id;
  }

  /**
   * Resolves a bus-strip insert parameter to its reserved insert-automation id.
   * Declares the mixer bus first so the offline engine resolves the same bus
   * selector.
   *
   * @param busId Bus id (declares the mixer bus on first use).
   * @param insertIndex Index into the bus strip's insert sequence.
   * @param paramName Processor JSON-key parameter name.
   * @returns Reserved insert-automation id.
   * @throws RangeError when the bus, insert or key is unknown.
   */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    const id = parameter.resolveBusInsertAutomationId(
      this.parameterContext,
      busId,
      insertIndex,
      paramName,
    );
    this.rememberInsertAutomationId({ kind: 'bus', busId }, id);
    return id;
  }

  /**
   * Resolves a hosted instrument's continuous parameter to its reserved
   * instrument-automation id, so an instrument parameter follows a breakpoint
   * lane at audio-block precision like a strip insert. Bind the instrument
   * (`setSynthInstrument`) before resolving.
   *
   * @param destinationId MIDI destination the instrument is bound to.
   * @param paramName Instrument JSON-key parameter name (e.g. `cutoffHz`).
   * @returns Reserved instrument-automation id.
   * @throws RangeError when the destination or key is unknown.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return parameter.resolveInstrumentAutomationId(this.parameterContext, destinationId, paramName);
  }

  /**
   * The number of automation lanes installed on the engine, including lanes
   * whose breakpoint list is currently empty.
   */
  get automationLaneCount(): number {
    return parameter.automationLaneCount(this.parameterContext);
  }

  /** The registered parameters, read from the offline mirror. */
  parameters(): EngineParameterInfo[] {
    return parameter.parameters(this.parameterContext);
  }

  /** Registers a custom parameter on the offline mirror and worklet engine. */
  addParameter(info: EngineParameterInfo): void {
    parameter.addParameter(this.parameterContext, info);
  }

  /** Clears custom parameters on both engines; automation lanes stay in place. */
  clearParameters(): void {
    parameter.clearParameters(this.parameterContext);
  }

  /**
   * Queues a track's solo/mute state. An armed PFL/AFL cue ({@link setTrackMonitorMode}) stays off
   * the program output here, which the facade renders with the cue kept separate; offline renders,
   * bounces and freezes never include it.
   */
  setTrackSoloMute(target: string | number, state: TrackSoloMuteState): void {
    parameter.setTrackSoloMute(this.parameterContext, target, state);
  }

  /**
   * Queues a per-track PFL/AFL monitor tap mode change; `mode` is a string and an
   * ordinal is a `TypeError`. The facade renders with the cue kept separate from
   * the program output; offline renders, bounces and freezes never include it.
   */
  setTrackMonitorMode(
    target: string | number,
    mode: EngineTrackMonitorMode,
    renderFrame?: number,
  ): void {
    parameter.setTrackMonitorMode(this.parameterContext, target, mode, renderFrame);
  }

  /** Queues a strip fader change in dB, smoothed on the audio thread. */
  setStripFaderDb(target: string | number, faderDb: number): void {
    this.sendSmoothedParam(this.resolveStripAutomationId(target, 'faderDb'), faderDb, () => {
      if (target === 'master') {
        strips.cacheMasterStripScalar(this.stripContext, 'faderDb', faderDb);
      }
    });
  }

  /** Queues a strip pan change, smoothed on the audio thread. */
  setStripPan(target: string | number, pan: number): void {
    this.sendSmoothedParam(this.resolveStripAutomationId(target, 'pan'), pan, () => {
      if (target === 'master') {
        strips.cacheMasterStripScalar(this.stripContext, 'pan', pan);
      }
    });
  }

  /** Resets the master's integrated loudness accumulator in both engine mirrors. */
  resetMasterLoudnessMeter(renderFrame?: number): void {
    const frame = resolveRenderFrame(renderFrame);
    commitCommand(
      this.commitContext,
      { type: SonareEngineCommandType.ResetMasterLoudnessMeter, renderFrame: frame },
      (offline) => offline.resetMasterLoudnessMeter(renderFrame),
    );
  }

  /**
   * Queues a reset of every mixer and effect processor to its prepared state,
   * applied to the offline mirror immediately and to the live engine at
   * `renderFrame` (omitted: the next block head). The same command that
   * follows it on the queue, such as `play()`, starts from the state an offline
   * render starts from. Instruments are not reset. During playback it cuts
   * running insert tails and delay lines mid-sound, like a seek.
   */
  resetProcessorState(renderFrame?: number): void {
    const frame = resolveRenderFrame(renderFrame);
    commitCommand(
      this.commitContext,
      { type: SonareEngineCommandType.ResetProcessorState, renderFrame: frame },
      (offline) => offline.resetProcessorState(renderFrame),
    );
  }

  /**
   * Declares the mixer track lanes in an explicit order.
   *
   * Lane indices are append-only: the list must start with the already-declared
   * track ids in their current order and may only append new ones after them;
   * a reorder or removal throws. (The raw `RealtimeEngine.setTrackLanes` does
   * accept both.) Existing queued and published track automation follows the
   * track id, and so do track insert automation ids (see
   * resolveStripInsertAutomationId). Entries carrying
   * `sends` replace that track's send list; entries without `sends` leave
   * existing sends untouched. At most `MAX_TRACK_LANES` (32) lanes are accepted.
   *
   * @param lanes Lane descriptors in the desired lane order.
   */
  setTrackLanes(lanes: readonly EngineTrackLane[]): void {
    mixer.setTrackLanes(this.mixerContext, lanes);
  }

  /**
   * Routes a track lane's post-fader output into a declared bus instead of
   * the master mix (group/folder routing); `null` restores the master mix and
   * `0` is a `RangeError`.
   */
  setTrackOutputBus(target: string | number, busId: number | null): void {
    mixer.setTrackOutputBus(this.mixerContext, target, busId);
  }

  /**
   * Keys one insert of a strip from another source (ducking/sidechainRouter
   * inserts); `null` removes the binding. A track strip accepts only a `'track'`
   * source, the master strip a track or a bus; a source `id` of `0` is a
   * `RangeError`. Throws, leaving the existing bindings unchanged, for a lane
   * keying itself or a binding that would close a cycle over the lane bindings;
   * a refused binding is never recorded, so it is not replayed on resync.
   */
  setStripSidechain(
    target: string | number,
    insertIndex: number,
    source: SidechainSource | null,
  ): void {
    mixer.setStripSidechain(this.mixerContext, target, insertIndex, source);
  }

  /**
   * Replaces the sends of one track or the master strip, on both engines; an empty array clears them.
   *
   * @throws RangeError for an unknown target, an undeclared bus, a duplicate send bus or a level outside
   * the send range.
   */
  setTrackSends(target: string | number, sends: EngineTrackSend[]): void {
    mixer.setTrackSends(this.mixerContext, target, sends);
  }

  /**
   * Replaces the bus list, on both engines. Each bus names its output bus, channel layout and sends.
   *
   * @throws RangeError for an invalid bus id, layout or send.
   */
  setTrackBuses(buses: EngineBus[]): void {
    mixer.setTrackBuses(this.mixerContext, buses);
  }

  /**
   * Keys one insert of a bus strip from a track lane or another bus
   * (ducking/sidechainRouter inserts); `null` removes the binding.
   */
  setBusSidechain(busId: number, insertIndex: number, source: SidechainSource | null): void {
    mixer.setBusSidechain(this.mixerContext, busId, insertIndex, source);
  }

  /**
   * Reports whether {@link setStripSidechain} would accept the binding, without
   * changing anything. Answers from the offline mirror, which holds the same
   * configuration as the live engine, and never declares a missing lane or bus.
   */
  canSetStripSidechain(
    target: string | number,
    insertIndex: number,
    source: SidechainSource,
  ): SidechainCheck {
    return this.offlineEngine.canSetStripSidechain(
      target === 'master' ? target : this.resolveTargetId(target),
      insertIndex,
      source,
    );
  }

  /** Reports whether {@link setBusSidechain} would accept the binding, without changing anything. */
  canSetBusSidechain(busId: number, insertIndex: number, source: SidechainSource): SidechainCheck {
    return this.offlineEngine.canSetBusSidechain(busId, insertIndex, source);
  }

  /**
   * Longest audible tail in samples (an upper bound; `2147483647` is
   * unbounded), answered from the offline mirror. The bound is conservative
   * before the first note and narrows once a patch has sounded.
   */
  get tailSamples(): number {
    return this.offlineEngine.tailSamples;
  }

  /** Processing latency in samples (fractional), answered from the offline mirror. */
  get graphLatencySamples(): number {
    return this.offlineEngine.graphLatencySamples;
  }

  /** Queues a bus fader change in dB, smoothed on the audio thread. */
  setBusGainDb(busId: number, gainDb: number): void {
    mixer.setBusGain(this.mixerContext, busId, gainDb);
  }

  /**
   * Replaces a strip (a track's, or the master) with `settings`; the document
   * is sent as `{ strips: [settings] }`. A track is declared first when it has
   * no lane yet.
   */
  setStrip(target: string | number, settings: MixSceneStripInput): void {
    const sceneJson = stringifyWithinBudget(
      { strips: [mixSceneStripToNative(settings, 'settings')] },
      'settings',
    );
    if (target === 'master') {
      mixer.setMasterStripJson(this.mixerContext, sceneJson);
      return;
    }
    // Parse before declaring a new lane so malformed text cannot declare one.
    mixer.decodedInsertSignature(sceneJson, { kind: 'track', trackId: 0 });
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    const { insertBaseResets, constructionOverrides } = strips.setTrackStripJson(
      this.stripContext,
      trackId,
      sceneJson,
      this.trackStripJson,
    );
    mixer.pruneStripSidechains(this.mixerContext, { kind: 'track', trackId }, sceneJson);
    this.syncMixer(insertBaseResets, constructionOverrides);
  }

  /** Replaces a bus strip with `settings`; the document is sent as `{ buses: [settings] }`. */
  setBusStrip(busId: number, settings: MixSceneBusInput): void {
    const sceneJson = stringifyWithinBudget(
      { buses: [mixSceneBusToNative(settings, 'settings')] },
      'settings',
    );
    mixer.setBusStripJson(this.mixerContext, busId, sceneJson);
  }

  /** Sets one EQ band of a strip (a track's, or the master). */
  setStripEqBand(target: string | number, bandIndex: number, band: EqBandInput): void {
    strips.setStripEqBand(this.stripContext, target, bandIndex, band);
  }

  /** Sets one EQ band of a bus strip. */
  setBusEqBand(busId: number, bandIndex: number, band: EqBandInput): void {
    this.ensureBus(busId);
    strips.setBusEqBand(this.stripContext, busId, bandIndex, band);
  }

  /**
   * Bypasses or re-enables one insert of a strip. Bypassing keeps the insert's
   * internal state (reverb tail, compressor envelope) unless `resetOnBypass`
   * (default `false`) is set; re-posting the scene, the only alternative,
   * rebuilds the chain and loses it.
   */
  setStripInsertBypassed(
    target: string | number,
    insertIndex: number,
    state: InsertBypassState,
  ): void {
    strips.setStripInsertBypassed(this.stripContext, target, insertIndex, state);
  }

  /** Bus counterpart of {@link setStripInsertBypassed}. */
  setBusInsertBypassed(busId: number, insertIndex: number, state: InsertBypassState): void {
    this.ensureBus(busId);
    strips.setBusInsertBypassed(this.stripContext, busId, insertIndex, state);
  }

  /** Sets one insert parameter of a strip by its JSON-key name. */
  setStripInsertParam(target: string | number, insertIndex: number, param: InsertParamValue): void {
    strips.setStripInsertParam(this.stripContext, target, insertIndex, param);
  }

  /** Bus counterpart of {@link setStripInsertParam}. */
  setBusInsertParam(busId: number, insertIndex: number, param: InsertParamValue): void {
    this.ensureBus(busId);
    strips.setBusInsertParam(this.stripContext, busId, insertIndex, param);
  }

  /** Sets a track strip's pan law; `'master'` is a `RangeError`. */
  setStripPanLaw(target: string | number, panLaw: PanLaw): void {
    strips.setStripPanLaw(this.stripContext, target, panLaw);
  }

  /** Sets a track strip's pan mode; `'master'` is a `RangeError`. */
  setStripPanMode(target: string | number, panMode: PanMode): void {
    strips.setStripPanMode(this.stripContext, target, panMode);
  }

  /** Sets a track strip's dual-pan positions; `'master'` is a `RangeError`. */
  setStripDualPan(target: string | number, leftPan: number, rightPan: number): void {
    strips.setStripDualPan(this.stripContext, target, leftPan, rightPan);
  }

  /**
   * Sets a track strip's surround placement (glitch-free, 5 ms placement
   * glide). Takes effect on a destination wider than stereo, so set `numChannels`
   * to the layout's plane count. Omitted fields default to a centered point
   * source. `'master'` is a `RangeError`.
   */
  setStripSurroundPan(target: string | number, pan: SurroundPan): void {
    strips.setStripSurroundPan(this.stripContext, target, pan);
  }

  /** Sets a track strip's channel delay in samples; `'master'` is a `RangeError`. */
  setStripChannelDelaySamples(target: string | number, delaySamples: number): void {
    strips.setStripChannelDelaySamples(this.stripContext, target, delaySamples);
  }

  /** Bus counterpart of {@link setStripPan}'s scene form; refused on a surround bus. */
  setBusPan(busId: number, pan: number): void {
    this.ensureBus(busId);
    strips.setBusPan(this.stripContext, busId, pan);
  }

  /** Sets a bus strip's pan law on both engines; an ordinal is a `TypeError`, an unknown bus a `RangeError`. */
  setBusPanLaw(busId: number, panLaw: PanLaw): void {
    this.ensureBus(busId);
    strips.setBusPanLaw(this.stripContext, busId, panLaw);
  }

  /** Sets a bus strip's pan mode on both engines; an ordinal is a `TypeError`, an unknown bus a `RangeError`. */
  setBusPanMode(busId: number, panMode: PanMode): void {
    this.ensureBus(busId);
    strips.setBusPanMode(this.stripContext, busId, panMode);
  }

  /** Sets a bus strip's dual-pan left and right positions (each -1 to 1, left to right) on both engines; an unknown bus is a `RangeError`. */
  setBusDualPan(busId: number, leftPan: number, rightPan: number): void {
    this.ensureBus(busId);
    strips.setBusDualPan(this.stripContext, busId, leftPan, rightPan);
  }

  /**
   * Creates and primes an OPFS-backed page provider for this live worklet
   * engine. Pass the returned `provider` to {@link addClip} in place of a
   * `Float32Array[]`; the `clipId` in `options` must equal that clip's explicit
   * `opts.id`. Subsequent cache misses are fetched on the main thread and
   * supplied back to the worklet through its bounded pull protocol.
   */
  async attachOpfsClipStream(
    options: OpfsClipStreamOptions,
  ): Promise<{ binding: OpfsClipPageProviderBinding; provider: ClipPageProvider }> {
    assertHandleLive(this.epoch.closed, 'SonareEngine');
    const { clipId, primePages = 1, ...providerOptions } = options;
    if ([...this.workletPageProviderClipIds.values()].some((entry) => entry.clipId === clipId)) {
      throw invalidState(`An OPFS stream is already attached for clip ${clipId}.`);
    }
    const streamer = this.ensureWorkletClipStreamer();
    // Allocate the worklet-owned provider before priming: port message order
    // ensures every prime page is adopted before addClip commits its schedule.
    this.postSync({
      type: 'syncClipPageProvider',
      clipId,
      numChannels: providerOptions.numChannels,
      numSamples: providerOptions.numSamples,
      pageFrames: providerOptions.pageFrames,
    });
    const token = this.epoch.current();
    // Distinguishes this provider from a later one attached under the same clip
    // id, so a stale page, clear or destroy cannot reach its successor.
    const streamKey = this.nextClipStreamKey++;
    this.postSync({
      type: 'syncClipPageProvider',
      clipId,
      streamKey,
      numChannels: providerOptions.numChannels,
      numSamples: providerOptions.numSamples,
      pageFrames: providerOptions.pageFrames,
    });
    let binding: OpfsClipPageProviderBinding;
    // The offline engine may come from the index bundle, whose natives only take its own providers.
    const createProvider = opfsClipPageProviderFactoryFor(
      this.offlineEngine,
      createOpfsClipPageProvider,
    );
    binding = createProvider({
      ...providerOptions,
      onPageSupplied: (pageIndex, channels) => {
        this.opfsExportSources.get(binding.provider)?.residentPages.add(pageIndex);
        this.postSync(
          { type: 'syncClipPage', clipId, streamKey, pageIndex, channels },
          transferableAudioBuffers(channels),
        );
      },
      onPageCleared: (pageIndex) => {
        this.opfsExportSources.get(binding.provider)?.residentPages.delete(pageIndex);
        this.postSync({ type: 'syncClipPageClear', clipId, streamKey, pageIndex });
      },
      onClose: () => {
        this.workletPageProviderClipIds.delete(binding.provider);
        this.opfsExportSources.delete(binding.provider);
        this.postSync({ type: 'syncClipPageDestroy', clipId, streamKey });
      },
    });
    // Destroy during priming closes the binding before the engines go away.
    const releaseOnDestroy = this.epoch.onClose(() => binding.destroy());
    this.workletPageProviderClipIds.set(binding.provider, { clipId, streamKey });
    this.opfsExportSources.set(binding.provider, {
      binding,
      pageFrames: providerOptions.pageFrames,
      numSamples: providerOptions.numSamples,
      residentPages: new Set(),
    });
    const lastPage = Math.ceil(providerOptions.numSamples / providerOptions.pageFrames) - 1;
    const primed: number[] = [];
    try {
      for (let page = 0; page < primePages && page <= lastPage; ++page) {
        const ok = await binding.supplyPage(page);
        this.epoch.assertCurrent(token);
        if (ok) {
          primed.push(page);
        }
      }
      streamer.addSource(
        {
          clipId,
          binding,
          pageFrames: providerOptions.pageFrames,
          numSamples: providerOptions.numSamples,
        },
        primed,
      );
      this.startWorkletClipPagePolling();
    } catch (error) {
      binding.destroy();
      throw error;
    } finally {
      releaseOnDestroy();
    }
    return { binding, provider: binding.provider };
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames on both the worklet
   * engine and this thread's offline engine.
   *
   * The audio thread reports the pages it is *about to* read that are not
   * resident yet, so the sliding-window streamer can service them before the
   * playhead reaches them. Without look-ahead a page miss is only reported
   * after the read already produced silence, which costs one block of silence
   * at every page boundary that was not primed.
   *
   * Defaults to half a second at the engine's sample rate. `0` disables it.
   * Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    assertHandleLive(this.epoch.closed, 'SonareEngine');
    commitSync(
      this.commitContext,
      (offline) => offline.setClipPagePrefetchFrames(frames),
      () => this.postSync({ type: 'syncClipPagePrefetchFrames', frames }),
    );
  }

  /**
   * Sets the number of concurrent time-stretch voices on both this thread's
   * offline engine and the live worklet engine. `voices` must be an integer
   * in `[0, 64]`; a non-integer, negative, or larger value throws and leaves
   * the capacity unchanged on both engines. Default is 8. Capacity 0 disables
   * time-stretch, so every warped clip plays resampled instead. A change
   * applied while the engine is running restarts the splice state of any clip
   * stretching through a voice at that moment.
   */
  setWarpVoiceCapacity(voices: number): void {
    assertHandleLive(this.epoch.closed, 'SonareEngine');
    commitSync(
      this.commitContext,
      (offline) => offline.setWarpVoiceCapacity(voices),
      () => this.postSync({ type: 'syncWarpVoiceCapacity', voices }),
    );
  }

  /** The current time-stretch voice capacity (default 8). */
  get warpVoiceCapacity(): number {
    return this.offlineEngine.warpVoiceCapacity;
  }

  /**
   * Adds a clip to a track and returns its id (`clip.id`, or a fresh one).
   * Give exactly one of `clip.channels` and `clip.pageProvider`; a page provider
   * must come from {@link attachOpfsClipStream}, with `clip.id` equal to the
   * stream's `clipId`.
   */
  addClip(trackId: string | number, clip: Omit<EngineClip, 'trackId'>): number {
    return clips.addClip(this.clipContext, trackId, clip);
  }

  /**
   * Removes the clip carrying `clipId`, on both engines.
   *
   * @throws RangeError when no clip has that id.
   */
  removeClip(clipId: number): void {
    clips.removeClip(this.clipContext, clipId);
  }

  /** Replaces the scheduled MIDI clips, on both engines. */
  setMidiClips(schedules: readonly EngineMidiClipSchedule[]): void {
    clips.setMidiClips(this.clipContext, schedules);
  }

  /**
   * Binds an instrument to a track's MIDI destination on both engines. A change
   * made while the transport is rolling reaches the worklet at once and takes
   * effect at its next render block; the outgoing instrument's sounding notes
   * fade out over 10 ms. The incoming instrument starts from its default
   * channel state (program, bank, controllers), so a host that needs the
   * current position's state resends it with {@link pushMidiUmp} afterwards.
   * A binding is applied at a block boundary; for a frame-exact switch, bind both instruments ahead of time and switch with timed MIDI, or use a timed program change.
   */
  setBuiltinInstrument(
    trackId: string | number,
    config: Omit<BuiltinSynthConfig, 'destinationId'> = {},
  ): void {
    strips.setBuiltinInstrument(this.stripContext, trackId, config);
  }

  /** Binds a NativeSynth patch; see {@link setBuiltinInstrument} for swap timing. */
  setSynthInstrument(trackId: string | number, patch: SynthPatch = {}): void {
    strips.setSynthInstrument(this.stripContext, trackId, patch);
  }

  /**
   * Sets how one MIDI channel (0–15) of the instrument bound to a track's MIDI destination treats
   * a note-on while another note on that channel is held, on both engines; see
   * `RealtimeEngine.setArticulation` for the three modes and the refusals, which are the same here.
   * The setting belongs to the bound instrument: binding another one (for example with
   * {@link setSynthInstrument}) replaces it with that instrument's own, `'poly'` for a new one.
   */
  setArticulation(trackId: string | number, channel: number, articulation: Articulation): void {
    strips.setArticulation(this.stripContext, trackId, channel, articulation);
  }

  /**
   * Reads one channel's articulation from the worklet's live engine — what the bound instrument
   * holds, which an instrument bound after {@link setArticulation} has replaced.
   */
  async articulation(trackId: string | number, channel: number): Promise<Articulation> {
    return this.realtimeNode.requestArticulation(
      this.stripContext.resolveTargetId(trackId),
      channel,
    );
  }

  /**
   * Reads from the worklet's live engine how many legato continuations the bound instrument
   * refused, so each note started a voice of its own; see `RealtimeEngine.legatoFallbackCount`.
   * The count belongs to the instrument, so binding another one starts it from zero.
   */
  async legatoFallbackCount(trackId: string | number): Promise<number> {
    return this.realtimeNode.requestLegatoFallbackCount(this.stripContext.resolveTargetId(trackId));
  }

  /**
   * Loads SoundFont 2 bytes into the offline mirror and the worklet so SF2 instruments can be bound with
   * {@link setSf2Instrument}. The bytes are copied for the call; a previously loaded SoundFont is replaced.
   */
  loadSoundFont(data: Uint8Array): void {
    strips.loadSoundFont(this.stripContext, data);
  }

  /** Binds a SoundFont player; see {@link setBuiltinInstrument} for swap timing. */
  setSf2Instrument(
    trackId: string | number,
    config: Omit<Sf2InstrumentConfig, 'destinationId'> = {},
  ): void {
    strips.setSf2Instrument(this.stripContext, trackId, config);
  }

  /**
   * Route a track's MIDI to the external output (drained via {@link onMidiOut})
   * instead of an internal instrument, so the track plays an external device.
   * Pass `false` to restore internal-synth playback. The change takes
   * effect at the next processed block; switching a destination's route first
   * releases its notes and resets its controllers through the old route, and
   * drops its pending MIDI-FX events.
   */
  setMidiDestinationExternal(trackId: string | number, midiDestinationExternal: boolean): void {
    strips.setMidiDestinationExternal(this.stripContext, trackId, midiDestinationExternal);
  }

  /**
   * Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
   * the external output so external gear tracks the transport tempo. The bytes
   * arrive through {@link onMidiOut} tagged with the transport destination.
   */
  setExternalMidiClockEnabled(externalMidiClockEnabled: boolean): void {
    strips.setExternalMidiClockEnabled(this.stripContext, externalMidiClockEnabled);
  }

  /**
   * Install or replace a live, non-destructive MIDI-FX insert for one
   * destination. The insert transforms the destination's MIDI before
   * synthesis (transpose, quantize, velocity shaping, humanize, harmonize,
   * arpeggiate) without rewriting any stored notes, so it can be bypassed by
   * {@link clearMidiFx}. `config` is a {@link MidiFxConfig}, the same object
   * the offline `Project.bakeMidiFx` takes.
   */
  setMidiFx(trackId: string | number, config: MidiFxConfig): void {
    strips.setMidiFx(this.stripContext, trackId, config);
  }

  /** Remove the live MIDI-FX insert from one destination (a no-op when none). */
  clearMidiFx(trackId: string | number): void {
    strips.clearMidiFx(this.stripContext, trackId);
  }

  /**
   * Queues a live MIDI note-on to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `group` and `channel` are 0 to 15; `note` and `velocity` are 0 to 127.
   */
  pushMidiNoteOn(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiNoteOn(
      this.stripContext,
      trackId,
      group,
      channel,
      note,
      velocity,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI note-off to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `group` and `channel` are 0 to 15; `note` is 0 to 127; `velocity` is 0 to 127 (omitted: 0).
   */
  pushMidiNoteOff(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiNoteOff(
      this.stripContext,
      trackId,
      group,
      channel,
      note,
      velocity,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI control change to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `group` and `channel` are 0 to 15; `controller` and `value` are 0 to 127.
   */
  pushMidiCc(
    trackId: string | number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiCc(
      this.stripContext,
      trackId,
      group,
      channel,
      controller,
      value,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI pitch bend to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `bend14` is unsigned 14-bit with centre 8192 (0 to 16383).
   */
  pushMidiPitchBend(
    trackId: string | number,
    group: number,
    channel: number,
    bend14: number,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiPitchBend(
      this.stripContext,
      trackId,
      group,
      channel,
      bend14,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI channel pressure to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `pressure` is 0 to 127.
   */
  pushMidiChannelPressure(
    trackId: string | number,
    group: number,
    channel: number,
    pressure: number,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiChannelPressure(
      this.stripContext,
      trackId,
      group,
      channel,
      pressure,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI polyphonic key pressure to the track's MIDI destination. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   * `note` and `pressure` are 0 to 127.
   */
  pushMidiPolyPressure(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiPolyPressure(
      this.stripContext,
      trackId,
      group,
      channel,
      note,
      pressure,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live raw UMP message to the track's MIDI destination. `words` is 1 to 4 words, most
   * significant first, with a length matching the message type of `words[0]`. SysEx7 and data messages
   * (MT 0x3 / 0x5) are refused; use {@link pushMidiSysex}. `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   */
  pushMidiUmp(
    trackId: string | number,
    words: UmpWords,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiUmp(this.stripContext, trackId, words, renderFrame, options);
  }

  /**
   * Queues a live MIDI 2.0 Program Change, with an optional bank, to a track's
   * MIDI destination; arguments follow {@link RealtimeEngine.pushMidiProgram}, and `options.contextTimeSec`
   * places it on the AudioContext clock instead of `renderFrame`. Which receivers select programs: the SoundFont player
   * (`setSf2Instrument`) selects bank and program under its own RX PROGRAM
   * CHANGE / RX BANK SELECT gates; a NativeSynth bound with
   * `useGmPrograms: true` (`setSynthInstrument`) resolves each channel's voice
   * from the bank and program; a NativeSynth whose patch plays the GM kit (the
   * `drumKit` preset, or a percussion patch with `gmKit` on) takes the program
   * as the GS drum-set select on every channel. Every other receiver accepts the
   * message and ignores it: the built-in synth, any other NativeSynth patch, a
   * part with RX PROGRAM CHANGE off, and an MPE member channel.
   */
  pushMidiProgram(
    trackId: string | number,
    group: number,
    channel: number,
    program: number,
    bank?: MidiBank,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiProgram(
      this.stripContext,
      trackId,
      group,
      channel,
      program,
      bank,
      renderFrame,
      options,
    );
  }

  /**
   * Queues a live MIDI SysEx frame to the track's MIDI destination. `data` is the full message including
   * the leading 0xF0 and trailing 0xF7 (1 to 512 bytes). `renderFrame` is the absolute render frame to fire at, or `options.contextTimeSec` the AudioContext time (not both); omitted: immediate.
   */
  pushMidiSysex(
    trackId: string | number,
    data: Uint8Array,
    renderFrame?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    strips.pushMidiSysex(this.stripContext, trackId, data, renderFrame, options);
  }

  /** Maps a MIDI controller to an engine parameter, on both engines. */
  bindMidiCc(binding: ProjectMidiCcBinding): void {
    this.offlineEngine.bindMidiCc(binding);
    this.postSync({ type: 'syncMidiCcBinding', binding: { ...binding } });
  }

  /** Enables the engine-owned live MIDI input source for a destination on both engines; `destinationId` is the MIDI destination (omitted: 0). */
  setMidiInputSource(destinationId = 0): void {
    commitSync(
      this.commitContext,
      (offline) => offline.setMidiInputSource(destinationId),
      () => this.postSync({ type: 'syncMidiInputSource', destinationId }),
    );
  }

  /** Disables the engine-owned live MIDI input source on both engines. */
  clearMidiInputSource(): void {
    commitSync(
      this.commitContext,
      (offline) => offline.clearMidiInputSource(),
      () => this.postSync({ type: 'syncClearMidiInputSource' }),
    );
  }

  /** Pushes a live MIDI input event into the offline engine, then mirrors it to the worklet. */
  private mirrorMidiInput(
    pushOffline: (offsetInBlockSamples: number) => void,
    type:
      | 'syncMidiInputNoteOn'
      | 'syncMidiInputNoteOff'
      | 'syncMidiInputCc'
      | 'syncMidiInputPitchBend'
      | 'syncMidiInputChannelPressure'
      | 'syncMidiInputPolyPressure',
    group: number,
    channel: number,
    data0: number,
    data1: number,
    offsetInBlockSamples: number | undefined,
    options: SonareEngineMidiTimingOptions | undefined,
  ): void {
    const timing = resolveMidiInputTiming(
      `SonareEngine.pushMidiInput${type.slice('syncMidiInput'.length)}` as MidiInputPushName,
      offsetInBlockSamples,
      options,
    );
    // The offline mirror has no AudioContext clock; a timed event reaches it at the block head.
    pushOffline(timing.offsetInBlockSamples);
    this.postSync({ type, group, channel, data0, data1, ...timing });
  }

  /**
   * Push a live MIDI note-on to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputNoteOn(
    group: number,
    channel: number,
    note: number,
    velocity: number,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) => this.offlineEngine.pushMidiInputNoteOn(group, channel, note, velocity, offset),
      'syncMidiInputNoteOn',
      group,
      channel,
      note,
      velocity,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI note-off to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) => this.offlineEngine.pushMidiInputNoteOff(group, channel, note, velocity, offset),
      'syncMidiInputNoteOff',
      group,
      channel,
      note,
      velocity,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI control change to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) => this.offlineEngine.pushMidiInputCc(group, channel, controller, value, offset),
      'syncMidiInputCc',
      group,
      channel,
      controller,
      value,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI pitch bend to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputPitchBend(
    group: number,
    channel: number,
    bend14: number,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) => this.offlineEngine.pushMidiInputPitchBend(group, channel, bend14, offset),
      'syncMidiInputPitchBend',
      group,
      channel,
      bend14,
      0,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI channel pressure to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputChannelPressure(
    group: number,
    channel: number,
    pressure: number,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) => this.offlineEngine.pushMidiInputChannelPressure(group, channel, pressure, offset),
      'syncMidiInputChannelPressure',
      group,
      channel,
      pressure,
      0,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI polyphonic key pressure to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputPolyPressure(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    this.mirrorMidiInput(
      (offset) =>
        this.offlineEngine.pushMidiInputPolyPressure(group, channel, note, pressure, offset),
      'syncMidiInputPolyPressure',
      group,
      channel,
      note,
      pressure,
      offsetInBlockSamples,
      options,
    );
  }

  /**
   * Push a live MIDI 2.0 Program Change, with an optional bank, to the engine-owned MIDI input
   * source. The arguments and refusals match {@link pushMidiProgram}; which receivers select
   * programs is described there.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `bank` or `options` holds an undeclared key, a `contextTimeSec` that is
   *   not a number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `group`, `channel`, `program` or a bank byte is out of range, or `contextTimeSec` is negative
   *   or not finite.
   */
  pushMidiInputProgram(
    group: number,
    channel: number,
    program: number,
    bank?: MidiBank,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    const timing = resolveMidiInputTiming(
      'SonareEngine.pushMidiInputProgram',
      offsetInBlockSamples,
      options,
    );
    const words = packMidi2ProgramRequest('pushMidiInputProgram', group, channel, program, bank);
    this.offlineEngine.pushMidiInputUmp(words, timing.offsetInBlockSamples);
    this.postSync({ type: 'syncMidiInputUmp', words, ...timing });
  }

  /**
   * Push a live MIDI raw UMP message to the engine-owned MIDI input source.
   *
   * @param offsetInBlockSamples Samples into the next block drained, clamped to its last frame;
   *   negative reads as 0. Not an absolute clock position. Defaults to 0.
   * @param options `contextTimeSec` places the event on the AudioContext clock instead;
   *   exclusive with `offsetInBlockSamples`.
   * @throws TypeError when `options` holds an undeclared key, a `contextTimeSec` that is not a
   *   number, or a `contextTimeSec` beside an `offsetInBlockSamples`; RangeError when
   *   `contextTimeSec` is negative or not finite.
   */
  pushMidiInputUmp(
    words: UmpWords,
    offsetInBlockSamples?: number,
    options?: SonareEngineMidiTimingOptions,
  ): void {
    const timing = resolveMidiInputTiming(
      'SonareEngine.pushMidiInputUmp',
      offsetInBlockSamples,
      options,
    );
    const copiedWords =
      words instanceof Uint32Array
        ? new Uint32Array(words)
        : (Array.isArray(words) ? boundedUmpWords(words) : words).slice();
    this.offlineEngine.pushMidiInputUmp(copiedWords, timing.offsetInBlockSamples);
    this.postSync({ type: 'syncMidiInputUmp', words: copiedWords, ...timing });
  }

  /**
   * Reads from the worklet's live engine how many timed MIDI events, live input and
   * track-addressed alike, arrived after the render frame their `contextTimeSec` named had been
   * rendered, so they played at the start of the next block instead. Cumulative since the
   * engine was created.
   *
   * @throws SonareError (as a rejection) with `InvalidState` once the engine is destroyed.
   */
  async lateMidiEventCount(): Promise<number> {
    return this.realtimeNode.requestLateMidiEventCount();
  }

  /** Queues a MIDI panic (all-notes-off) releasing every sounding note on both engines. `renderFrame` is the absolute render frame to fire at; omitted: immediate. */
  pushMidiPanic(renderFrame?: number): void {
    strips.pushMidiPanic(this.stripContext, renderFrame);
  }

  /**
   * Sets up capture in one call: buffer, source, record offset, input monitor
   * and punch window, on both engines. The call states the whole setup, so an
   * omitted field returns to its default and an omitted `punch` clears the
   * punch window. `numChannels` defaults to the offline plane count.
   */
  configureCapture(options: CaptureOptions): void {
    capture.configureCapture(this.captureContext, options);
  }

  /** Arms or disarms the engine-global capture path; per-track capture is not implemented. */
  armCapture(state: CaptureArmState): void {
    capture.armCapture(this.captureContext, state);
  }

  /** Reads the capture state from the worklet; the answer arrives asynchronously. */
  captureStatus(): Promise<EngineCaptureStatus> {
    return capture.captureStatus(this.captureContext);
  }

  /** Reads the audio captured so far from the worklet, one `Float32Array` per channel; the answer arrives asynchronously. */
  capturedAudio(): Promise<Float32Array[]> {
    return capture.capturedAudio(this.captureContext);
  }

  /** Discards the captured audio on both engines; resolves once the worklet has done so. */
  async resetCapture(): Promise<void> {
    return capture.resetCapture(this.captureContext);
  }

  /** Configures the metronome on both engines; if the command ring is full neither changes. */
  setMetronome(opts: EngineMetronomeConfig): void {
    // The full config (beatGain/accentGain/clickSamples/clickSec) cannot fit
    // the fixed-size SAB command record, so it is delivered out-of-band; the
    // SetMetronome command then toggles enabled state on the audio thread.
    commitCommand(
      this.commitContext,
      { type: SonareEngineCommandType.SetMetronome, renderFrame: -1, argInt: opts.enabled ? 1 : 0 },
      (offline) => offline.setMetronome(opts),
      () => this.postSync({ type: 'syncMetronome', config: opts }),
    );
  }

  /**
   * Adds a marker at `ppq` (quarter notes, in [0, 1e12]) and returns its id.
   *
   * @param name Marker label (omitted: empty).
   */
  addMarker(ppq: number, name = ''): number {
    return markerOps.addMarker(this.markerContext, ppq, name);
  }

  /**
   * Replaces the whole marker set in one call. Entries without an `id` are
   * assigned fresh ids; entries carrying an `id` keep it. Returns the stored
   * markers in the order given.
   */
  setMarkers(entries: ReadonlyArray<EngineMarkerInput>): EngineMarker[] {
    return markerOps.setMarkers(this.markerContext, entries);
  }

  /** The stored markers in timeline order, read from the offline mirror. */
  markers(): EngineMarker[] {
    return markerOps.markers(this.markerContext);
  }

  /** One stored marker by id; an unknown id is a `RangeError`. */
  marker(markerId: number): EngineMarker {
    return markerOps.marker(this.markerContext, markerId);
  }

  /** Seeks to a marker; an unknown id is a `RangeError`. */
  seekMarker(markerId: number): void {
    markerOps.seekMarker(this.markerContext, markerId);
  }

  /** Loops between two markers. */
  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    markerOps.setLoopFromMarkers(this.markerContext, startMarkerId, endMarkerId);
  }

  /**
   * Renders the timeline offline through this thread's mirror engine, from the
   * current transport position, and returns one planar buffer per offline
   * channel. Primes the mirror first, so the result equals a bounce of the same
   * span. Runs synchronously on the calling thread: the thread is blocked for
   * the whole render.
   *
   * The request takes `totalFrames`, `blockSize` (default and ceiling: the
   * offline block size; a larger value throws `RangeError`), `finalize` and
   * With
   * `finalize: false` a chunk leaves held notes and delay tails running into
   * the next call, which does not prime again; end the series with
   * {@link finishOfflineRender}. The mirror's transport position is restored
   * when the timeline ends.
   */
  async renderOffline(request: SonareEngineRenderOfflineRequest): Promise<Float32Array[]> {
    return exporter.renderOffline(this.exportContext, request);
  }

  /**
   * Ends a chunked {@link renderOffline} series: releases every held note,
   * flushes the delay lines and restores the mirror's transport position.
   */
  finishOfflineRender(): void {
    exporter.finishOfflineRender(this.exportContext);
  }

  /**
   * Bounces the timeline to planar channels with the options, defaults
   * and result of {@link RealtimeEngine.bounceOffline}; `blockSize` defaults to
   * the offline block size and must not exceed it (`RangeError`). Runs
   * synchronously on the calling thread. The mirror's transport position is
   * restored afterwards. OPFS-streamed clips are paged in for the span first.
   */
  async bounceOffline(options: EngineBounceOptions): Promise<EngineBounceResult> {
    return exporter.bounceOffline(this.exportContext, options);
  }

  /**
   * Freezes the source layer with the options, defaults and result of
   * {@link RealtimeEngine.freezeOffline}: master, monitor, graph, capture and
   * metronome stay live. As there, the frozen clip replaces the clip set and the
   * MIDI clips are withdrawn: the facade, the mirror and the worklet all end up
   * holding only that clip. `blockSize` defaults to the offline block size and must not
   * exceed it (`RangeError`). Runs synchronously on the calling thread.
   */
  async freezeOffline(options: EngineFreezeOptions): Promise<EngineFreezeResult> {
    return exporter.freezeOffline(this.exportContext, options);
  }

  /**
   * Subscribe to batches of external-MIDI events (already lowered to MIDI 1.0
   * bytes) for delivery to Web MIDI output ports. Each event carries its
   * absolute `renderFrame`; a batch is not a render block. With postMessage
   * transport a batch holds one block's events; with the SAB ring it holds what
   * accumulated since the previous poll, which may span many blocks. Returns an
   * unsubscribe function.
   */
  onMidiOut(callback: (events: ExternalMidiEvent[]) => void): () => void {
    return this.realtimeNode.onMidiOut(callback);
  }

  /** Subscribes to meter snapshots from the worklet. Returns a function that removes the subscription. */
  onMeter(callback: (meter: WorkletMeterSnapshot) => void): () => void {
    return this.realtimeNode.onMeter(callback);
  }

  /** Subscribes to scope snapshots (spectrum and goniometer); produced only when `scopeIntervalFrames` is above 0 in SAB mode. Returns a function that removes the subscription. */
  onScope(callback: (scope: WorkletScopeSnapshot) => void): () => void {
    return this.realtimeNode.onScope(callback);
  }

  /** Subscribes to engine telemetry records from the worklet. Returns a function that removes the subscription. */
  onTelemetry(callback: (telemetry: EngineTelemetry) => void): () => void {
    return this.realtimeNode.onTelemetry(callback);
  }

  /** Subscribes to syncs the worklet refused after the offline mirror accepted them. Returns a function that removes the subscription. */
  onSyncError(callback: (message: EngineSyncErrorMessage) => void): () => void {
    return this.realtimeNode.onSyncError(callback);
  }

  /** Drains telemetry published since the last poll (SAB mode); empty otherwise. See {@link SonareRealtimeEngineNode.pollTelemetry}. */
  pollTelemetry(): EngineTelemetry[] {
    return this.realtimeNode.pollTelemetry();
  }

  /** Drains meter snapshots published since the last poll (SAB mode); empty otherwise. See {@link SonareRealtimeEngineNode.pollMeters}. */
  pollMeters(): WorkletMeterSnapshot[] {
    return this.realtimeNode.pollMeters();
  }

  /** Drains scope snapshots published since the last poll (SAB mode with `scopeIntervalFrames` above 0); empty otherwise. */
  pollScope(): WorkletScopeSnapshot[] {
    return this.realtimeNode.pollScope();
  }

  /** Per-insert gain reduction for a meter target; see {@link SonareRealtimeEngineNode.pollInsertGainReduction}. */
  pollInsertGainReduction(targetId: number): Promise<number[]> {
    return this.realtimeNode.pollInsertGainReduction(targetId);
  }

  /** Stops the engine and releases the worklet node and the offline mirror. Safe to call more than once. */
  destroy(): void {
    if (this.epoch.closed) {
      return;
    }
    this.unsubscribeWorkletClipRequests?.();
    this.unsubscribeWorkletClipRequests = undefined;
    this.workletClipStreamer?.destroy();
    this.workletClipStreamer = undefined;
    if (this.workletClipPagePollTimer !== undefined) {
      clearInterval(this.workletClipPagePollTimer);
      this.workletClipPagePollTimer = undefined;
    }
    this.workletClipPageRequests.clear();
    // Reclaims bindings still priming in attachOpfsClipStream while the
    // engines their providers live on still exist.
    this.epoch.close();
    this.realtimeNode.stop();
    this.realtimeNode.pollTelemetry();
    void this.realtimeNode.destroy();
    this.offlineEngine.destroy();
  }

  /** Same as {@link destroy}, for `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }

  private ensureWorkletClipStreamer(): ClipPageStreamer {
    if (!this.workletClipStreamer) {
      this.workletClipStreamer = new ClipPageStreamer({
        engine: { popClipPageRequest: () => this.popWorkletClipPageRequest() },
      });
    }
    return this.workletClipStreamer;
  }

  /**
   * SAB requests have no postMessage wake-up by design. Polling on the main
   * thread is therefore intentionally outside the audio callback; 8 ms keeps
   * the bounded OPFS prefetch frontier responsive without adding worklet GC.
   */
  private startWorkletClipPagePolling(): void {
    if (this.workletClipPagePollTimer !== undefined) {
      return;
    }
    const poll = () => {
      if (!this.epoch.closed) {
        this.realtimeNode.pollClipPageRequests();
      }
    };
    poll();
    this.workletClipPagePollTimer = setInterval(poll, 8);
  }

  private pumpWorkletClipPages(): void {
    const streamer = this.workletClipStreamer;
    if (!streamer || this.workletClipPump) {
      return;
    }
    this.workletClipPump = streamer
      .pump()
      .catch((error: unknown) => {
        // The worklet keeps rendering silence for an unavailable page. Surface
        // I/O failures without converting them into an AudioWorklet exception.
        // biome-ignore lint/suspicious/noConsole: asynchronous OPFS diagnostic.
        console.error('Sonare OPFS clip-page supply failed:', error);
      })
      .finally(() => {
        this.workletClipPump = undefined;
        if (this.workletClipPageRequests.size > 0) {
          this.pumpWorkletClipPages();
        }
      });
  }

  private enqueueWorkletClipPageRequest(request: ClipPagePageRequest): void {
    if (
      !Number.isInteger(request.clipId) ||
      request.clipId < 0 ||
      !Number.isInteger(request.pageIndex) ||
      request.pageIndex < 0
    ) {
      return;
    }
    // Reinsert an existing id so iteration order represents newest frontiers.
    this.workletClipPageRequests.delete(request.clipId);
    if (this.workletClipPageRequests.size >= MAX_PENDING_WORKLET_CLIP_PAGE_REQUESTS) {
      const oldest = this.workletClipPageRequests.keys().next().value;
      if (oldest !== undefined) {
        this.workletClipPageRequests.delete(oldest);
      }
    }
    this.workletClipPageRequests.set(request.clipId, request);
  }

  private popWorkletClipPageRequest(): ClipPagePageRequest | null {
    const entry = this.workletClipPageRequests.entries().next().value;
    if (!entry) {
      return null;
    }
    const [clipId, request] = entry;
    this.workletClipPageRequests.delete(clipId);
    return request;
  }

  /**
   * The attached OPFS stream `clip` schedules, or undefined when its page
   * provider is not one; throws when the clip id differs from the stream's.
   */
  private workletClipStream(clip: EngineClip): { clipId: number; streamKey: number } | undefined {
    if (clip.pageProvider === undefined) {
      return undefined;
    }
    const stream = this.workletPageProviderClipIds.get(clip.pageProvider);
    if (stream !== undefined && clip.id !== stream.clipId) {
      throw refuse(
        `OPFS stream clipId ${stream.clipId} must match addClip(..., { id: ${stream.clipId} }).`,
      );
    }
    return stream;
  }

  private mixerLanes(): EngineTrackLane[] {
    return mixer.mixerLanes(this.mixerContext);
  }

  private syncMixer(
    insertBaseResets: mixer.StripJsonTarget[] = [],
    oneShotInsertParamOverrides: mixer.InsertParamOverride[] = [],
  ): void {
    mixer.syncMixer(this.mixerContext, false, insertBaseResets, oneShotInsertParamOverrides);
  }

  private insertAutomationTargetKey(target: mixer.StripJsonTarget): string {
    return target.kind === 'track'
      ? `track:${target.trackId}`
      : target.kind === 'bus'
        ? `bus:${target.busId}`
        : 'master';
  }

  private rememberInsertAutomationId(target: mixer.StripJsonTarget, id: number): void {
    const key = this.insertAutomationTargetKey(target);
    const ids = this.insertAutomationIdsByTarget.get(key) ?? new Set<number>();
    ids.add(id);
    this.insertAutomationIdsByTarget.set(key, ids);
  }

  private clearInsertAutomationLanes(
    target: mixer.StripJsonTarget,
    forgetResolvedIds = false,
  ): void {
    const key = this.insertAutomationTargetKey(target);
    const ids = this.insertAutomationIdsByTarget.get(key);
    if (!ids) {
      return;
    }
    for (const id of ids) {
      if (this.automationLanes.has(id)) {
        this.setAutomationLane(id, []);
      }
    }
    if (forgetResolvedIds) {
      this.insertAutomationIdsByTarget.delete(key);
    }
  }

  // Posts an out-of-band control-sync message to the worklet engine processor.
  // Sync messages use a string `type` so the worklet's message handler routes
  // them through receiveMessage() (numeric `type` is reserved for SonareEngineCommandRecord).
  private postSync(message: SonareEngineSyncMessage, transfer?: Transferable[]): void {
    // Teardown callbacks (a closing clip stream) still post while destroy runs; they are moot.
    if (this.epoch.closed) {
      return;
    }
    this.realtimeNode.postSync(message, transfer);
  }

  // The control-only mirror applies due commands immediately while preserving
  // scheduled ones for its next render block.
  private flushOfflineMirror(): void {
    this.offlineEngine.applyCommandsDueNowPreservingFuture();
  }

  private sendMirroredCommand(
    command: Parameters<SonareRealtimeEngineNode['sendCommand']>[0],
  ): boolean {
    const accepted = this.realtimeNode.sendCommand(command);
    this.flushOfflineMirror();
    return accepted;
  }

  // Collaborator surface handed to the mixer/routing free functions so they can
  // mutate the routing stores (held by reference), mirror into the offline
  // engine, post mixer-sync messages, and declare lanes/buses without a
  // back-reference to the whole engine.
  private get mixerContext(): EngineMixerContext {
    return {
      offlineEngine: this.offlineEngine,
      trackLaneIds: this.trackLaneIds,
      trackSends: this.trackSends,
      trackOutputBus: this.trackOutputBus,
      trackSourceChannelLayout: this.trackSourceChannelLayout,
      laneSidechains: this.laneSidechains,
      busSidechains: this.busSidechains,
      masterSidechains: this.masterSidechains,
      buses: this.buses,
      trackStripJson: this.trackStripJson,
      busStripJson: this.busStripJson,
      insertParamOverrides: this.insertParamOverrides,
      flushOfflineMirror: () => this.flushOfflineMirror(),
      postSync: (message) => this.postSync(message),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      ensureBus: (busId) => this.ensureBus(busId),
      mixerLanes: () => this.mixerLanes(),
      syncMixer: (insertBaseResets, oneShotInsertParamOverrides) =>
        this.syncMixer(insertBaseResets, oneShotInsertParamOverrides),
      clearInsertAutomationLanes: (target, forgetResolvedIds) =>
        this.clearInsertAutomationLanes(target, forgetResolvedIds),
      sendSmoothedParam: (paramId, value, commitCache) =>
        this.sendSmoothedParam(paramId, value, commitCache),
      getMasterStripJson: () => this.masterStripJson,
      cacheMasterStripJson: (sceneJson) => {
        this.masterStripJson = sceneJson;
      },
    };
  }

  // Collaborator surface handed to the strip/pan/EQ/insert/MIDI free functions
  // so they can mirror into the offline engine, post sync messages, and resolve
  // lanes without each holding a back-reference to the whole engine.
  private get stripContext(): EngineStripContext {
    return {
      offlineEngine: this.offlineEngine,
      trackLaneIds: this.trackLaneIds,
      postSync: (message) => this.postSync(message),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      resolveTargetId: (target) => this.resolveTargetId(target),
      readStripJson: (target) => mixer.cachedStripJson(this.mixerContext, target),
      writeStripJson: (target, sceneJson) =>
        mixer.cacheStripJson(this.mixerContext, target, sceneJson),
      insertParamOverrides: this.insertParamOverrides,
      clearInsertAutomationLanes: (target) => this.clearInsertAutomationLanes(target),
    };
  }

  // Collaborator surface handed to the automation-lane free functions so they
  // can mutate the lane store, mirror into the offline engine, and post
  // automation-sync messages without holding a back-reference.
  private get automationContext(): EngineAutomationContext {
    return {
      offlineEngine: this.offlineEngine,
      automationLanes: this.automationLanes,
      postSync: (message) => this.postSync(message),
      resolveParamId: (nodeId, param) => this.resolveParamId(nodeId, param),
    };
  }

  // Collaborator surface handed to the capture/record/punch free functions so
  // they can mirror into and query the offline engine, command the realtime
  // node, and read/write the capture config without a back-reference.
  private get captureContext(): EngineCaptureContext {
    return {
      offlineEngine: this.offlineEngine,
      realtimeNode: this.realtimeNode,
      sendCommand: (command) => this.sendMirroredCommand(command),
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      sampleRate: this.sampleRate,
      offlineNumChannels: this.offlineNumChannels,
      postSync: (message) => this.postSync(message),
      getCaptureConfig: () => this.captureConfig,
      setCaptureConfig: (config) => {
        this.captureConfig = config;
      },
    };
  }

  // Collaborator surface handed to the parameter / automation-id resolution
  // free functions so they can mirror into and query the offline engine,
  // command the realtime node, and declare lanes/buses without holding a
  // back-reference to the whole engine.
  private get parameterContext(): EngineParameterContext {
    return {
      offlineEngine: this.offlineEngine,
      sendCommand: (command) => this.sendMirroredCommand(command),
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      postSync: (message) => this.postSync(message),
      automationLanes: this.automationLanes,
      trackLaneIds: this.trackLaneIds,
      resolveParamId: (nodeId, param) => this.resolveParamId(nodeId, param),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      ensureBus: (busId) => this.ensureBus(busId),
    };
  }

  // Collaborator surface handed to the tempo / time-signature free functions so
  // they can mirror into the offline engine, command the realtime node, post
  // tempo-sync messages, and mutate the engine's tempo-map state by reference.
  private get tempoContext(): EngineTempoContext {
    return {
      offlineEngine: this.offlineEngine,
      realtimeNode: this.realtimeNode,
      sendCommand: (command) => this.sendMirroredCommand(command),
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      postSync: (message) => this.postSync(message),
      getTempoBpm: () => this.tempoBpm,
      setTempoBpm: (bpm) => {
        this.tempoBpm = bpm;
      },
      getTimeSignature: () => this.timeSignature,
      setTimeSignature: (signature) => {
        this.timeSignature = signature;
      },
      getTempoSegments: () => this.tempoSegments,
      setTempoSegments: (segments) => {
        this.tempoSegments = segments;
      },
      getTimeSignatureSegments: () => this.timeSignatureSegments,
      setTimeSignatureSegments: (segments) => {
        this.timeSignatureSegments = segments;
      },
      setLatestTransportState: (state) => {
        this.latestTransportState = state;
      },
      getLatestTransportState: () => this.latestTransportState,
    };
  }

  private get exportContext(): EngineExportContext {
    return {
      offlineEngine: this.offlineEngine,
      offlineBlockSize: this.offlineBlockSize,
      offlineNumChannels: this.offlineNumChannels,
      clips: this.clips,
      opfsSources: this.opfsExportSources,
      session: this.exportSession,
      epoch: this.epoch,
      flushOfflineMirror: () => this.flushOfflineMirror(),
      // The frozen clip stands for the MIDI it baked, so the MIDI clips leave with the old set.
      commitFrozenClip: (clip, previousClipIds) => {
        clips.replaceClips(this.clipContext, clip, previousClipIds);
        clips.setMidiClips(this.clipContext, []);
      },
    };
  }

  // Collaborator surface handed to the audio/MIDI clip scheduling free
  // functions so they can mutate the clip stores, mirror into the offline
  // engine, and post clip-sync messages without holding a back-reference.
  private get clipContext(): EngineClipContext {
    return {
      offlineEngine: this.offlineEngine,
      clips: this.clips,
      midiClips: this.midiClips,
      allocateClipId: () => this.nextClipId++,
      postSync: (message, transfer) => this.postSync(message, transfer),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      resolveTargetId: (target) => this.resolveTargetId(target),
      workletClipStream: (clip) => this.workletClipStream(clip),
      hasClipStream: (clipId) =>
        [...this.workletPageProviderClipIds.values()].some((entry) => entry.clipId === clipId),
    };
  }

  // Collaborator surface handed to the marker free functions so they can mutate
  // the marker store and id counter, mirror into the offline engine, post
  // marker-sync messages, and drive transport without a back-reference.
  private get markerContext(): EngineMarkerContext {
    return {
      offlineEngine: this.offlineEngine,
      markers: this.markerStore,
      getNextMarkerId: () => this.nextMarkerId,
      setNextMarkerId: (value) => {
        this.nextMarkerId = value;
      },
      postSync: (message) => this.postSync(message),
      sendCommand: (command) => this.sendMirroredCommand(command),
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      setLoop: (startPpq, endPpq, enabled) =>
        tempo.setLoop(this.tempoContext, { startPpq, endPpq, enabled }),
    };
  }

  // Mirrors a smoothed parameter into the offline engine and pushes a
  // sample-accurate smoothed-param command to the realtime runtime.
  private sendSmoothedParam(paramId: number, value: number, commitCache?: () => void): void {
    parameter.commitParameter(this.commitContext, paramId, value, true, undefined, commitCache);
  }

  // The transaction every control edit commits through (see engine-commit.ts).
  private get commitContext(): EngineCommitContext {
    return {
      offlineEngine: this.offlineEngine,
      canSend: (command) => this.realtimeNode.canSendCommand(command),
      sendCommand: (command) => this.sendMirroredCommand(command),
    };
  }

  private resolveParamId(nodeId: string, param: string | number): number {
    return resolveParamId(this.parameters(), nodeId, param);
  }

  private syncParameters(): void {
    const parameters = this.parameters();
    if (parameters.length > 0) {
      this.postSync({ type: 'syncParameters', parameters });
    }
  }

  private resolveTargetId(target: string | number): number {
    return resolveTargetId(target);
  }

  private ensureTrackLane(target: string | number): number {
    return mixer.ensureTrackLane(this.mixerContext, target);
  }

  private ensureBus(busId: number): number {
    // Tested on the raw id: truncating first leaves the integrality check
    // unreachable, and registers a bus under an id the caller never named.
    if (!Number.isInteger(busId) || busId <= 0) {
      throw refuse(`Invalid bus id for mixer bus: ${String(busId)}`);
    }
    const existing = this.buses.findIndex((bus) => bus.busId === busId);
    if (existing >= 0) {
      return existing;
    }
    this.buses.push({ busId });
    this.syncMixer();
    return this.buses.length - 1;
  }
}
