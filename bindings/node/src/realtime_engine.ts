import { ClipPageProvider, FileClipPageProvider } from './clip_page_provider.js';
import { addon } from './native.js';
import type { ProjectTimeline } from './project.js';
import type {
  Articulation,
  BuiltinSynthConfig,
  ClipPageRequest,
  ControllerBinding,
  EngineAutomationPoint,
  EngineBounceOptions,
  EngineBounceResult,
  EngineBus,
  EngineCapturePunchRequest,
  EngineCaptureSource,
  EngineCaptureStatus,
  EngineClip,
  EngineExternalMidiEvent,
  EngineFreezeOptions,
  EngineFreezeResult,
  EngineGraphSpec,
  EngineMarker,
  EngineMeterTelemetry,
  EngineMeterTelemetryWide,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineScopeTelemetry,
  EngineTelemetry,
  EngineTrackLane,
  EngineTrackMonitorMode,
  EngineTrackSend,
  EngineTransportState,
  EqBandInput,
  FileClipPageProviderOptions,
  MidiCcBindOptions,
  MpeDimension,
  NoteTracking,
  PanLawInput,
  PanMode,
  PartRigInsert,
  PartRigMode,
  PartRigRequest,
  ProjectMidiCcBinding,
  ProjectTempoSegment,
  ProjectTimeSignatureSegment,
  RenderOfflineRequest,
  Sf2InstrumentConfig,
  SidechainCheck,
  SidechainRefusal,
  SidechainSourceKind,
  SurroundPan,
  SynthPatch,
  UmpWords,
} from './types.js';
import {
  assertNonNegativeSafeInteger,
  resolveRenderFrame,
  resolveSampleBound,
} from './validation.js';
import {
  engineAutomationPointValue,
  normalizePartRig,
  normalizeSynthInstrument,
  panLawValue,
  panModeValue,
  sendTimingValue,
  sidechainSourceKindValue,
  trackMonitorModeValue,
} from './value_coercion.js';

export * from './clip_page_provider.js';

/**
 * Resolves each send's `sendTiming` spelling to its C ABI ordinal, shared by
 * {@link RealtimeEngine.setTrackLanes} and {@link RealtimeEngine.setTrackBuses}
 * so a track lane's sends and a bus's sends normalize identically.
 */
// The addon reads a page provider by its numeric handle.
function toNativeClip(clip: EngineClip): unknown {
  return {
    ...clip,
    pageProvider:
      typeof clip.pageProvider === 'object' && clip.pageProvider !== null
        ? clip.pageProvider.id
        : clip.pageProvider,
  };
}

function normalizeSends(sends: EngineTrackSend[]): EngineTrackSend[] {
  return sends.map((send) =>
    send.sendTiming === undefined
      ? send
      : { ...send, sendTiming: sendTimingValue(send.sendTiming) },
  );
}

/**
 * One normalizer for both {@link RealtimeEngine.renderOffline} call forms, so
 * the request object and the positional overload cannot drift in their defaults.
 */
function normalizeRenderOfflineRequest(
  channelsOrRequest: Float32Array[] | RenderOfflineRequest,
  blockSize: number,
): { channels: Float32Array[]; blockSize: number; finalize: boolean } {
  const request = Array.isArray(channelsOrRequest)
    ? { channels: channelsOrRequest, blockSize }
    : channelsOrRequest;
  return {
    channels: request.channels,
    blockSize: request.blockSize ?? 128,
    finalize: request.finalize ?? true,
  };
}

const SIDECHAIN_REFUSALS: readonly SidechainRefusal[] = [
  'invalidTarget',
  'insertOutOfRange',
  'undeclaredSource',
  'invalidSourceKind',
  'selfKey',
  'cycle',
  'tableFull',
  'planRefused',
];

/** Maps the C ABI `SonareSidechainRefusal` value (0 = accepted) to the facade shape. */
function sidechainCheck(code: number): SidechainCheck {
  if (code === 0) {
    return { ok: true, reason: null };
  }
  const reason = SIDECHAIN_REFUSALS[code - 1];
  if (reason === undefined) {
    throw new RangeError(`Unknown sidechain refusal: ${code}`);
  }
  return { ok: false, reason };
}

export class RealtimeEngine {
  private native: InstanceType<typeof addon.RealtimeEngine>;
  private disposed = false;
  private sampleRate: number;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
    maxChannels = 64,
  ) {
    this.sampleRate = sampleRate;
    this.native = new addon.RealtimeEngine(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
      maxChannels,
    );
  }

  /**
   * Size the engine's queues and scratch for a sample rate and block size.
   *
   * `commandCapacity` must not exceed 65536 and `telemetryCapacity` must not
   * exceed 16384; a larger value throws and leaves the engine untouched. The
   * telemetry number is not a queue depth paid for one-for-one: the engine
   * reserves that many meter records per metered lane, so its memory cost is
   * far larger than the number given here.
   */
  prepare(
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
    maxChannels = 64,
  ): void {
    this.native.prepare(sampleRate, maxBlockSize, commandCapacity, telemetryCapacity, maxChannels);
    this.sampleRate = sampleRate;
  }

  /** Starts transport playback, at `renderFrame` when given. */
  play(renderFrame?: number): void {
    this.native.play(resolveRenderFrame('play', renderFrame));
  }

  /**
   * Stops transport playback, at `renderFrame` when given.
   *
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset, so
   * controller values set before a loop region are not restored at the wrap.
   */
  stop(renderFrame?: number): void {
    this.native.stop(resolveRenderFrame('stop', renderFrame));
  }

  /** Moves the transport to `timelineSample`, a position in timeline samples. */
  seekSample(timelineSample: number, renderFrame?: number): void {
    this.native.seekSample(timelineSample, resolveRenderFrame('seekSample', renderFrame));
  }

  /**
   * Snaps every in-flight parameter ramp (engine-level smoothed params, mixer
   * lane fader/pan/gate, bus gains) to its target value. It is one step of
   * {@link primeOfflineParameters}; called alone it only snaps the smoothers
   * and does not touch automation or processing state. Control-thread only:
   * must not be called concurrently with {@link process}.
   */
  settleParameters(): void {
    this.native.settleParameters();
  }

  /**
   * Runs the offline pre-roll that bounce and freeze run before rendering,
   * without rendering anything: applies every queued command, resets the mixer
   * and effect processors to their prepared state, adopts the published
   * snapshots, resolves automation and lane gates at the transport position
   * and snaps every smoother. A host that drives the engine block by block
   * calls it once before the first chunk so a repeat render starts from the
   * same state. Instruments are not reset. Control-thread only: must not be
   * called concurrently with {@link process}.
   *
   * @param numChannels - Channel count of the blocks that will be rendered
   * @param blockSize - Frames per block that will be rendered (clamped to the prepared block size)
   * @throws If a count is not positive, `numChannels` exceeds the prepared
   *   channel count, or the engine was never prepared
   */
  primeOfflineParameters(numChannels: number, blockSize: number): void {
    this.native.primeOfflineParameters(numChannels, blockSize);
  }

  /**
   * Queues a reset of every mixer and effect processor to its prepared state.
   * At `renderFrame` (omitted: the next block head) the lane, bus, master,
   * monitor and graph processors drop their tails, delay lines and envelopes,
   * automation is applied at the transport position and every smoother is
   * snapped to its target, so playback queued after it starts from the state an
   * offline bounce starts from. Strips the host bound to the engine, including
   * host-owned ones, are reset too. Instruments are not reset. Called during
   * playback it cuts running insert tails and delay lines mid-sound, like a
   * seek.
   *
   * @param renderFrame - Block-relative frame to apply at; omit for the next block head (negative is refused)
   * @throws If the command queue is full
   */
  resetProcessorState(renderFrame?: number): void {
    this.native.resetProcessorState(resolveRenderFrame('resetProcessorState', renderFrame));
  }

  /**
   * Longest audible tail after the last input, in samples: an upper bound made
   * of the longest instrument tail plus the longest route through lane strips
   * (channel delay included), buses, sends, the graph and the master strip.
   * `2147483647` means the tail is unbounded. Control-thread only: must not be
   * called concurrently with {@link process}.
   */
  tailSamples(): number {
    return this.native.tailSamples();
  }

  /** Processing latency of the engine in 1/256 samples, as telemetry reports it. */
  graphLatencySamplesQ8(): number {
    return this.native.graphLatencySamplesQ8();
  }

  /**
   * Applies commands queued on an offline/control-only engine immediately.
   * A host that never calls {@link process} still queues onto a bounded
   * realtime command ring, so those commands have to be drained explicitly
   * rather than by the next process() call. Not safe to call concurrently with
   * a running process().
   */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  /**
   * Snaps only the in-flight strip insert parameter ramps (track, bus and
   * master inserts) to their targets, leaving fader/pan ramps untouched. For
   * use after a structural replay (strip replacement followed by restored
   * insert values).
   */
  settleInsertParameters(): void {
    this.native.settleInsertParameters();
  }

  /**
   * Applies the queued commands already due at the current render frame,
   * keeping future-dated commands queued in their original order.
   * Control-only mirror counterpart of {@link flushControlCommands}, which
   * drains future commands too.
   */
  applyCommandsDueNowPreservingFuture(): void {
    this.native.applyCommandsDueNowPreservingFuture();
  }

  /** Moves the transport to `ppq`, a position in quarter notes. */
  seekPpq(ppq: number, renderFrame?: number): void {
    this.native.seekPpq(ppq, resolveRenderFrame('seekPpq', renderFrame));
  }

  /** Set a finite tempo in the range (0, 100000] BPM. */
  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  /**
   * Installs a tempo map from ramp segments. Each segment needs a finite
   * non-negative `startPpq` and a `bpm` in (0, 100000]; a non-zero `endBpm`
   * uses the same range. An empty array clears the map to the single tempo.
   */
  setTempoSegments(segments: ReadonlyArray<ProjectTempoSegment>): void {
    this.native.setTempoSegments(segments);
  }

  /**
   * Installs a time-signature map. Each segment needs a finite non-negative
   * `startPpq` and a positive `numerator` / `denominator`.
   */
  setTimeSignatureSegments(segments: ReadonlyArray<ProjectTimeSignatureSegment>): void {
    this.native.setTimeSignatureSegments(segments);
  }

  sampleAtPpq(ppq: number): number {
    return this.native.sampleAtPpq(ppq);
  }

  /** Sets the loop region between two quarter-note positions and enables or disables it. */
  setLoop(startPpq: number, endPpq: number, enabled = true): void {
    this.native.setLoop(startPpq, endPpq, enabled);
  }

  addParameter(info: EngineParameterInfo): void {
    this.native.addParameter(info);
  }

  parameterCount(): number {
    return this.native.parameterCount();
  }

  parameterInfoByIndex(index: number): Required<EngineParameterInfo> {
    return this.native.parameterInfoByIndex(index);
  }

  parameterInfo(id: number): Required<EngineParameterInfo> {
    return this.native.parameterInfo(id);
  }

  /**
   * Replaces the automation lane driving `paramId`. An empty `points` array
   * leaves the target undriven rather than snapping it to 0 or a default:
   * once adopted, the target reverts to the last value explicitly sent
   * through {@link setParameter} / {@link setParameterSmoothed}, or is left
   * unchanged if no such value was ever sent for `paramId`. This holds
   * regardless of whether the manual value or the lane clear reaches the
   * audio thread first.
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws `InvalidParameter`; resolve per-strip ids with the `resolve*AutomationId`
   * methods.
   */
  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(paramId, points.map(engineAutomationPointValue));
  }

  automationLaneCount(): number {
    return this.native.automationLaneCount();
  }

  setMarkers(markers: EngineMarker[]): void {
    this.native.setMarkers(markers);
  }

  markerCount(): number {
    return this.native.markerCount();
  }

  markerByIndex(index: number): EngineMarker {
    return this.native.markerByIndex(index);
  }

  marker(id: number): EngineMarker {
    return this.native.marker(id);
  }

  seekMarker(markerId: number, renderFrame?: number): void {
    this.native.seekMarker(markerId, resolveRenderFrame('seekMarker', renderFrame));
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  /** Set a metronome config; click lengths are limited to one second. */
  setMetronome(config: EngineMetronomeConfig): void {
    this.native.setMetronome(config);
  }

  metronome(): Required<EngineMetronomeConfig> {
    return this.native.metronome();
  }

  countInEndSample(startSample: number, bars: number): number {
    return this.native.countInEndSample(startSample, bars);
  }

  createClipPageProvider(
    numChannels: number,
    numSamples: number,
    pageFrames: number,
  ): ClipPageProvider {
    const id = this.native.createClipPageProvider(numChannels, numSamples, pageFrames);
    return new ClipPageProvider(this, id);
  }

  createFileClipPageProvider(
    path: string,
    options: FileClipPageProviderOptions,
  ): FileClipPageProvider {
    const id = this.native.createClipPageProvider(
      options.numChannels,
      options.numSamples,
      options.pageFrames,
    );
    try {
      return new FileClipPageProvider(this, id, path, options);
    } catch (err) {
      this.native.destroyClipPageProvider(id);
      throw err;
    }
  }

  setClips(clips: EngineClip[]): void {
    this.native.setClips(clips.map(toNativeClip));
  }

  /**
   * Replaces the clip whose `id` matches `clip.id`, or adds it when no clip has
   * that id. Every other clip stays as published. The clip is validated like a
   * `setClips` entry; a refused clip leaves the published set unchanged.
   *
   * @example
   * engine.upsertClip({ id: 3, channels: [samples], startPpq: 4, gain: 0.5 });
   */
  upsertClip(clip: EngineClip): void {
    this.native.upsertClip(toNativeClip(clip));
  }

  /**
   * Removes the clip `clipId`, leaving every other clip as published.
   * Throws when no clip has that id.
   */
  removeClip(clipId: number): void {
    this.native.removeClip(clipId);
  }

  clipCount(): number {
    return this.native.clipCount();
  }

  setTrackLanes(lanes: Array<number | EngineTrackLane>): void {
    this.native.setTrackLanes(
      lanes.map((lane) => {
        if (typeof lane === 'number') {
          return { trackId: lane };
        }
        if (lane.sends === undefined) {
          return lane;
        }
        return { ...lane, sends: normalizeSends(lane.sends) };
      }),
    );
  }

  /**
   * Replaces one lane's sends, leaving its output bus, layout and every other
   * lane as published. An empty array clears the lane's sends.
   *
   * The edited lane list goes through {@link setTrackLanes}, so it is validated
   * the same way (a send naming an undeclared bus, a duplicate send bus or a
   * level outside the allowed range is refused), automation ids keep naming
   * their track, and an unchanged send list keeps the sends' in-flight ramps; a
   * changed list restarts every send ramp on the lane. Lane membership is
   * caller-owned here: `trackId` must already be in the published lane list
   * (set through {@link setTrackLanes}), otherwise this throws. The worklet
   * facade's `setTrackSends` shares the name but creates lanes as needed.
   * Control-thread only; do not call concurrently with `process`.
   *
   * @throws SonareError `InvalidParameter` for an unknown track (the message
   *   names the id) or a send list `setTrackLanes` would refuse; `TypeError`
   *   when `sends` is not an array
   */
  setTrackSends(trackId: number, sends: EngineTrackSend[]): void {
    this.native.setTrackSends(trackId, Array.isArray(sends) ? normalizeSends(sends) : sends);
  }

  /**
   * Sets one lane's output bus, leaving its sends, layout and every other lane
   * as published. `busId` 0 returns the lane to the master mix; any other value
   * must name a declared bus. Same validation, membership and threading
   * contract as {@link setTrackSends}.
   */
  setTrackOutputBus(trackId: number, busId: number): void {
    this.native.setTrackOutputBus(trackId, busId);
  }

  /**
   * Configure realtime engine buses: layout, fader, output and sends. Replaces
   * the whole bus list. An `outputBusId` or send naming an undeclared bus, the
   * bus itself, or forming a cycle (through outputs, sends, or bus-sourced
   * sidechain keys) is rejected and leaves the previous buses unchanged.
   */
  setTrackBuses(buses: EngineBus[]): void {
    this.native.setTrackBuses(
      buses.map((bus) =>
        bus.sends === undefined ? bus : { ...bus, sends: normalizeSends(bus.sends) },
      ),
    );
  }

  /**
   * Keys one insert of a lane strip from another lane's post-strip audio
   * (ducking/sidechainRouter inserts). sourceTrackId 0 removes the binding.
   *
   * A key naming the track itself, or a binding that would close a cycle
   * across the track ids' bindings as a whole, throws and leaves the existing
   * bindings unchanged. The source and destination tracks need not exist yet,
   * and the insert index is not range-checked: a binding outlives lane
   * re-publication. The key is taken after the source's lane strip, with the
   * delay to the keyed insert planned so the rendered result does not depend
   * on lane order or block size. Control-thread only: must not be called
   * concurrently with {@link process}.
   */
  setLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): void {
    this.native.setLaneSidechain(trackId, insertIndex, sourceTrackId);
  }

  /**
   * Install a timeline compiled by {@link Project.compileTimeline}, all or
   * nothing. Stopped transport only: a playing engine throws and is left
   * unchanged, as does any validation failure. The timeline replaces tempo and
   * time-signature segments, markers, track lanes, automation lanes, clips and
   * the project mixer scene's track strips; a value set on one of these through
   * a low-level setter is overwritten by the next apply. Instruments, buses,
   * the master strip, metronome, loop and capture are untouched, so bind
   * instruments with {@link setBuiltinInstrument} (or a sibling) using each
   * MIDI track's destination id.
   *
   * The engine keeps its own reference, so the timeline may be disposed right
   * after this call. A disposed timeline throws.
   *
   * Also throws on an unprepared engine and on a timeline whose sample rate
   * differs from the engine's. Once a timeline is applied, a later prepare at
   * another rate is refused until the timeline's clips are cleared.
   */
  applyProjectTimeline(timeline: ProjectTimeline): void {
    this.native.applyProjectTimeline((timeline as unknown as { native: unknown }).native);
  }

  /**
   * Keys one insert of a bus strip from a track lane or another bus. The key
   * is taken before the source's lane fader or bus `gainDb`. `sourceId` 0
   * removes the binding. Control-thread only: must not be called concurrently
   * with {@link process}.
   */
  setBusSidechain(
    busId: number,
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): void {
    this.native.setBusSidechain(busId, insertIndex, sidechainSourceKindValue(sourceKind), sourceId);
  }

  /**
   * Keys one insert of the master strip from a track lane or a bus.
   * `insertIndex` counts the master strip's pre-fader inserts first, then its
   * post-fader inserts, as the other master insert setters do. Same source
   * rules and threading contract as {@link setBusSidechain}.
   */
  setMasterSidechain(
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): void {
    this.native.setMasterSidechain(insertIndex, sidechainSourceKindValue(sourceKind), sourceId);
  }

  /**
   * Reports whether {@link setLaneSidechain} would accept the binding, without
   * changing anything. `reason` names the first check the setter would fail.
   * Control-thread only: must not be called concurrently with {@link process}.
   */
  canSetLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): SidechainCheck {
    return sidechainCheck(this.native.canSetLaneSidechain(trackId, insertIndex, sourceTrackId));
  }

  /** Reports whether {@link setBusSidechain} would accept the binding, without changing anything. */
  canSetBusSidechain(
    busId: number,
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): SidechainCheck {
    return sidechainCheck(
      this.native.canSetBusSidechain(
        busId,
        insertIndex,
        sidechainSourceKindValue(sourceKind),
        sourceId,
      ),
    );
  }

  /** Reports whether {@link setMasterSidechain} would accept the binding, without changing anything. */
  canSetMasterSidechain(
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): SidechainCheck {
    return sidechainCheck(
      this.native.canSetMasterSidechain(
        insertIndex,
        sidechainSourceKindValue(sourceKind),
        sourceId,
      ),
    );
  }

  setBusStripJson(busId: number, sceneJson: string): void {
    this.native.setBusStripJson(busId, sceneJson);
  }

  /** Bus-strip counterpart of {@link setTrackStripEqBand}. */
  setBusStripEqBand(busId: number, bandIndex: number, band: EqBandInput | string): void {
    this.native.setBusStripEqBandJson(
      busId,
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  /** Bus-strip counterpart of {@link setTrackStripEqBandJson}. */
  setBusStripEqBandJson(busId: number, bandIndex: number, bandJson: string): void {
    this.native.setBusStripEqBandJson(busId, bandIndex, bandJson);
  }

  setTrackStripJson(trackId: number, sceneJson: string): void {
    this.native.setTrackStripJson(trackId, sceneJson);
  }

  setTrackStripEqBand(trackId: number, bandIndex: number, band: EqBandInput | string): void {
    this.native.setTrackStripEqBandJson(
      trackId,
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  setTrackStripEqBandJson(trackId: number, bandIndex: number, bandJson: string): void {
    this.native.setTrackStripEqBandJson(trackId, bandIndex, bandJson);
  }

  setTrackStripInsertBypassed(
    trackId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setTrackStripInsertBypassed(trackId, insertIndex, bypassed, resetOnBypass);
  }

  setMasterStripJson(sceneJson: string): void {
    this.native.setMasterStripJson(sceneJson);
  }

  setMasterStripEqBand(bandIndex: number, band: EqBandInput | string): void {
    this.native.setMasterStripEqBandJson(
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  setMasterStripEqBandJson(bandIndex: number, bandJson: string): void {
    this.native.setMasterStripEqBandJson(bandIndex, bandJson);
  }

  setMasterStripInsertBypassed(
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setMasterStripInsertBypassed(insertIndex, bypassed, resetOnBypass);
  }

  /**
   * Changes one track-strip insert parameter in realtime, addressed by the
   * processor's JSON-key parameter name — an entry in {@link
   * masteringInsertParamInfo} whose `id` is non-null. Applied at the next
   * block head via the engine command queue; safe during playback. Throws if
   * the track, insert, or name is unknown, the param is not realtime-safe, or
   * the command queue is full.
   */
  setTrackStripInsertParamByName(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  }

  /** Master-strip counterpart of {@link setTrackStripInsertParamByName}. */
  setMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.native.setMasterStripInsertParamByName(insertIndex, paramName, value);
  }

  /**
   * Bus-strip counterpart of {@link setTrackStripInsertParamByName}. The bus
   * must already exist via {@link setTrackBuses} and carry a strip configured
   * with {@link setBusStripJson}.
   */
  setBusStripInsertParamByName(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setBusStripInsertParamByName(busId, insertIndex, paramName, value);
  }

  /**
   * Applies a live track-strip insert edit on this engine's owning thread,
   * bypassing the command queue. Returns `false` rather than throwing when
   * the track, insert, or name is unknown, or the param is not realtime-safe.
   * Not safe to call concurrently with {@link process}.
   */
  applyTrackStripInsertParamByNameNow(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyTrackStripInsertParamByNameNow(trackId, insertIndex, paramName, value);
  }

  /**
   * Restores a retained track-strip insert value exactly, without a ramp.
   * Used to replay retained edits after {@link setTrackStripJson} replaced
   * the strip. Throws if the track, insert, or name is unknown.
   */
  restoreTrackStripInsertParamByName(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.restoreTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  }

  /** Master-strip counterpart of {@link applyTrackStripInsertParamByNameNow}. */
  applyMasterStripInsertParamByNameNow(
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyMasterStripInsertParamByNameNow(insertIndex, paramName, value);
  }

  /** Master-strip counterpart of {@link restoreTrackStripInsertParamByName}. */
  restoreMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.native.restoreMasterStripInsertParamByName(insertIndex, paramName, value);
  }

  /** Bus-strip counterpart of {@link applyTrackStripInsertParamByNameNow}. */
  applyBusStripInsertParamByNameNow(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyBusStripInsertParamByNameNow(busId, insertIndex, paramName, value);
  }

  /** Bus-strip counterpart of {@link restoreTrackStripInsertParamByName}. */
  restoreBusStripInsertParamByName(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.restoreBusStripInsertParamByName(busId, insertIndex, paramName, value);
  }

  /**
   * Forgets the remembered manual insert-parameter values of one track strip
   * and discards its queued insert edits. Call before {@link setTrackStripJson}
   * replaces the strip when its old values must not carry over; the setter
   * never does this itself, since a queued edit may already target the new chain.
   */
  clearTrackInsertParameterBases(trackId: number): void {
    this.native.clearTrackInsertParameterBases(trackId);
  }

  /** Master-strip counterpart of {@link clearTrackInsertParameterBases}. */
  clearMasterInsertParameterBases(): void {
    this.native.clearMasterInsertParameterBases();
  }

  /**
   * Bus-strip counterpart of {@link clearTrackInsertParameterBases}. Also
   * accepts a bus identity that was already removed, covering every selector
   * the identity held across remove/re-add.
   */
  clearBusInsertParameterBases(busId: number): void {
    this.native.clearBusInsertParameterBases(busId);
  }

  /**
   * Bus-strip counterpart of {@link setTrackStripInsertBypassed}. The bus must
   * already exist via {@link setTrackBuses} and carry a strip configured with
   * {@link setBusStripJson}.
   */
  setBusStripInsertBypassed(
    busId: number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    this.native.setBusStripInsertBypassed(busId, insertIndex, bypassed, resetOnBypass);
  }

  /**
   * Resolves a track-lane insert parameter (by its JSON-key name) to the
   * reserved automation id that can then be driven with
   * {@link setAutomationLane}, {@link setParameter}, or
   * {@link setParameterSmoothed}, exactly like a fader/pan id. Returns `-1`
   * when the track, insert, or name is unknown. (The Python binding raises a
   * `SonareError` for an unknown id where Node/WASM return the `-1` sentinel.)
   *
   * This trio is how a mastering processor gets time-varying automation: the
   * `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`,
   * `maximizer.*` and `multiband.*` processors are all available as strip
   * inserts, so placing one on a strip and resolving its parameter here drives
   * it at audio-block precision, live and offline alike. The whole-signal
   * stages of the offline mastering chain (`repair.*`, `loudness`, and the
   * match stages) have no insert form and no automation id: they buffer the
   * entire signal by construction and do not run on the realtime path.
   *
   * Track, bus and master insert ids share one lifetime rule: an id names its
   * strip by identity and the kind of processor in its slot (for
   * `effects.gsEfx`, its EFX type too), so it survives lane and bus reorders
   * and the removal of other strips, and stays valid until its track or bus is
   * removed or its slot comes to hold another kind of processor. After that it
   * applies nothing, its queued edits are dropped, and it is never reissued;
   * resolve again for the new processor. A change that would take the engine
   * past its 8192 insert-id entries is refused.
   */
  resolveTrackInsertAutomationId(trackId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveTrackInsertAutomationId(trackId, insertIndex, paramName);
  }

  /** Master-strip counterpart of {@link resolveTrackInsertAutomationId}. */
  resolveMasterInsertAutomationId(insertIndex: number, paramName: string): number {
    return this.native.resolveMasterInsertAutomationId(insertIndex, paramName);
  }

  /** Bus-strip counterpart of {@link resolveTrackInsertAutomationId}. */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveBusInsertAutomationId(busId, insertIndex, paramName);
  }

  /**
   * Resolve a track lane's fader (`'faderDb'`) or pan (`'pan'`) to its reserved
   * automation id. Same lifetime rule as {@link resolveTrackInsertAutomationId}:
   * the id names the track, not its lane position, so it keeps driving that
   * track across {@link setTrackLanes} reorders and applies nothing once the
   * track is removed. Returns `-1` when the track has no lane or the name is
   * neither.
   */
  resolveTrackLaneAutomationId(trackId: number, paramName: 'faderDb' | 'pan'): number {
    return this.native.resolveTrackLaneAutomationId(trackId, paramName);
  }

  /**
   * Resolve a bus's fader (`'faderDb'`) to its reserved automation id. Same
   * lifetime rule as {@link resolveTrackInsertAutomationId}: the id names the
   * bus, not its position in the bus list, so it keeps driving that bus across
   * {@link setTrackBuses} reorders and the removal of other buses, and applies
   * nothing once the bus is removed. Returns `-1` when the bus is not
   * configured or the name is not `'faderDb'`.
   */
  resolveBusAutomationId(busId: number, paramName: 'faderDb'): number {
    return this.native.resolveBusAutomationId(busId, paramName);
  }

  /** Return the immutable construction value for a resolved insert parameter. */
  insertParameterConstructedValue(paramId: number): number {
    return this.native.insertParameterConstructedValue(paramId);
  }

  /**
   * Resolves a hosted instrument's continuous parameter (by its JSON-key name)
   * to the reserved automation id, so an instrument parameter is driven at
   * audio-block precision exactly like a strip insert. Returns `-1` when the
   * destination has no bound instrument, the instrument exposes no automatable
   * parameters, or the name is unknown.
   *
   * For the NativeSynth ({@link setSynthInstrument}) the names are the
   * continuous `SynthPatch` fields: `gain`, `busDrive`, `cutoffHz`,
   * `resonanceQ`, `drive`, `keyTrack`, `envToCutoffCents`, `velToCutoffCents`,
   * `ampAttackMs`, `ampDecayMs`, `ampSustain`, `ampReleaseMs`,
   * `filterAttackMs`, `filterDecayMs`, `filterSustain`, `filterReleaseMs`,
   * `lfoRateHz`, `lfoToPitchCents`, `lfo2RateHz`, `glideMs`, `bodyMix`,
   * `stereoSpread`, `detuneCents`, `driftCents`, `pitchOffsetCents`,
   * `hpCutoffHz`, `sampleHoldHz`, `bitDepth`.
   *
   * Structural fields (`preset`, `engineMode`, `waveform`, `filterModel`,
   * `unison`, `polyphony`, `body`, `modRoutings`) are not automatable and
   * return `-1`; rebind the instrument with a new patch instead.
   *
   * All automatable parameters of a NativeSynth reach already-sounding voices
   * from the next processed block. Voices using GM program patches are
   * unaffected; parameters apply to NativeSynth's own patch only.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return this.native.resolveInstrumentAutomationId(destinationId, paramName);
  }

  /**
   * Sets a track lane strip's pan position (-1..1) in realtime. Applied at the
   * next block head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param pan Pan position from -1 (hard left) to 1 (hard right).
   */
  setTrackStripPan(trackId: number, pan: number): void {
    this.native.setTrackStripPan(trackId, pan);
  }

  /**
   * Sets a track lane strip's pan law in realtime. Applied at the next block
   * head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param panLaw Pan law as an enum name, the enum, or the raw int.
   */
  setTrackStripPanLaw(trackId: number, panLaw: PanLawInput): void {
    this.native.setTrackStripPanLaw(trackId, panLawValue(panLaw));
  }

  /**
   * Sets a track lane strip's pan mode in realtime. Applied at the next block
   * head via the engine command queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param panMode Pan mode as an enum name, the enum, or the raw int.
   */
  setTrackStripPanMode(trackId: number, panMode: PanMode): void {
    this.native.setTrackStripPanMode(trackId, panModeValue(panMode));
  }

  /**
   * Sets a track lane strip's independent left/right pan positions (dual-pan
   * mode) in realtime. Applied at the next block head via the engine command
   * queue; safe during playback.
   *
   * @param trackId Lane the strip belongs to.
   * @param leftPan Left-channel pan position from -1 to 1.
   * @param rightPan Right-channel pan position from -1 to 1.
   */
  setTrackStripDualPan(trackId: number, leftPan: number, rightPan: number): void {
    this.native.setTrackStripDualPan(trackId, leftPan, rightPan);
  }

  /**
   * Sets a track lane strip's surround pan in realtime. Applied at the next
   * block head with a short 5 ms placement glide, so a live move is glitch-free; it
   * takes effect when the lane feeds a destination wider than stereo (a 5.1/7.1
   * group bus or master). Requires a lane with a bound strip.
   *
   * @param trackId Lane the strip belongs to.
   * @param pan Surround position; omitted fields default to a centered point source.
   */
  setTrackStripSurroundPan(trackId: number, pan: SurroundPan): void {
    this.native.setTrackStripSurroundPan(trackId, pan);
  }

  /**
   * Sets a bus strip's pan position (-1..1) in realtime. Applied at the next
   * block head via the engine command queue; safe during playback. Rejected on
   * a surround (>2ch) bus, where pan is not defined.
   *
   * @param busId Bus the strip belongs to.
   * @param pan Pan position from -1 (hard left) to 1 (hard right).
   */
  setBusStripPan(busId: number, pan: number): void {
    this.native.setBusStripPan(busId, pan);
  }

  /**
   * Sets a bus strip's pan law in realtime. Applied at the next block head via
   * the engine command queue; safe during playback. Rejected on a surround
   * (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param panLaw Pan law as an enum name, the enum, or the raw int.
   */
  setBusStripPanLaw(busId: number, panLaw: PanLawInput): void {
    this.native.setBusStripPanLaw(busId, panLawValue(panLaw));
  }

  /**
   * Sets a bus strip's pan mode in realtime. Applied at the next block head via
   * the engine command queue; safe during playback. Rejected on a surround
   * (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param panMode Pan mode as an enum name, the enum, or the raw int.
   */
  setBusStripPanMode(busId: number, panMode: PanMode): void {
    this.native.setBusStripPanMode(busId, panModeValue(panMode));
  }

  /**
   * Sets a bus strip's independent left/right pan positions (dual-pan mode) in
   * realtime. Applied at the next block head via the engine command queue;
   * safe during playback. Rejected on a surround (>2ch) bus.
   *
   * @param busId Bus the strip belongs to.
   * @param leftPan Left-channel pan position from -1 to 1.
   * @param rightPan Right-channel pan position from -1 to 1.
   */
  setBusStripDualPan(busId: number, leftPan: number, rightPan: number): void {
    this.native.setBusStripDualPan(busId, leftPan, rightPan);
  }

  /**
   * Sets a track lane strip's alignment delay in samples.
   * `delaySamples` is a non-negative whole-sample delay that moves this lane later
   * relative to every other lane. It is not latency: PDC does not compensate it
   * and the reported graph latency is unchanged. It reallocates the strip's delay
   * line, so treat it as a structural change: do NOT call it concurrently with
   * {@link process}. Stop
   * playback (or otherwise quiesce the audio callback) before calling.
   *
   * @param trackId Lane the strip belongs to.
   * @param delaySamples Non-negative channel delay in samples.
   */
  setTrackStripChannelDelaySamples(trackId: number, delaySamples: number): void {
    this.native.setTrackStripChannelDelaySamples(trackId, delaySamples);
  }

  supplyClipPage(providerId: number, pageIndex: number, channels: Float32Array[]): void {
    this.native.supplyClipPage(providerId, pageIndex, channels);
  }

  clearClipPage(providerId: number, pageIndex: number): void {
    this.native.clearClipPage(providerId, pageIndex);
  }

  destroyClipPageProvider(providerId: number): void {
    this.native.destroyClipPageProvider(providerId);
  }

  popClipPageRequest(): ClipPageRequest | null {
    return this.native.popClipPageRequest();
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames.
   *
   * The player reports the pages it is *about to* read that are not resident
   * yet, so a streaming host can service them before the audio thread reaches
   * them. Without look-ahead a page miss is only reported after the read
   * already produced silence, which costs one block of silence at every page
   * boundary the host has not primed.
   *
   * Look-ahead requests drain through the same {@link popClipPageRequest}
   * queue and are queued *after* the block's genuine misses, so a host that
   * keeps only the newest request per clip tracks the look-ahead frontier.
   *
   * `prepare` defaults this to half a second at the engine's sample rate; `0`
   * disables it. A clip whose pages are all resident produces no requests
   * either way. Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    this.native.setClipPagePrefetchFrames(frames);
  }

  /** Current clip-page look-ahead window in timeline frames. */
  clipPagePrefetchFrames(): number {
    return this.native.clipPagePrefetchFrames();
  }

  /**
   * Allocate an addon-owned capture buffer with the requested channel count
   * and capacity in frames. This is the canonical cross-binding form.
   *
   * The addon sizes the planes itself, so a long capture costs exactly one
   * allocation of `numChannels * capacityFrames` samples.
   */
  setCaptureBuffer(numChannels: number, capacityFrames: number): void;
  /**
   * Install caller-owned capture planes. The addon copies them immediately, so
   * their ArrayBuffers may be transferred after this call.
   *
   * @deprecated Prefer `(numChannels, capacityFrames)` for cross-binding parity.
   */
  setCaptureBuffer(channels: Float32Array[]): void;
  setCaptureBuffer(numChannelsOrChannels: number | Float32Array[], capacityFrames?: number): void {
    if (Array.isArray(numChannelsOrChannels)) {
      this.native.setCaptureBuffer(numChannelsOrChannels);
      return;
    }
    const fnName = 'RealtimeEngine.setCaptureBuffer';
    if (capacityFrames === undefined) {
      throw new TypeError(`${fnName}: capacityFrames is required when numChannels is a number`);
    }
    // Two clauses, two owners: that each is a count a JS number still denotes is
    // the shared check's, that neither may be zero is this buffer's. Each names
    // its own argument, so a caller who passed one of the two is told which.
    assertNonNegativeSafeInteger(fnName, numChannelsOrChannels, 'numChannels');
    assertNonNegativeSafeInteger(fnName, capacityFrames, 'capacityFrames');
    if (numChannelsOrChannels <= 0) {
      throw new RangeError(`${fnName}: numChannels must be greater than zero`);
    }
    if (capacityFrames <= 0) {
      throw new RangeError(`${fnName}: capacityFrames must be greater than zero`);
    }
    this.native.setCaptureBuffer(numChannelsOrChannels, capacityFrames);
  }

  armCapture(armed = true): void {
    this.native.armCapture(armed);
  }

  /**
   * Set the punch-in window, half-open `[start, end)` in timeline samples.
   *
   * Each bound is samples or seconds (rounded to the nearest sample at the
   * engine's sample rate), one spelling each; both bounds are required and a
   * negative one is refused.
   */
  setCapturePunch(request: EngineCapturePunchRequest): void;
  setCapturePunch(startSample: number, endSample: number, enabled?: boolean): void;
  setCapturePunch(
    first: EngineCapturePunchRequest | number,
    endSample?: number,
    enabled = true,
  ): void {
    const request: EngineCapturePunchRequest =
      typeof first === 'object' && first !== null
        ? first
        : { startSample: first, endSample, enabled };
    const start = resolveSampleBound(
      'setCapturePunch',
      request.startSample,
      request.startSec,
      this.sampleRate,
      'startSample',
      'startSec',
    );
    const end = resolveSampleBound(
      'setCapturePunch',
      request.endSample,
      request.endSec,
      this.sampleRate,
      'endSample',
      'endSec',
    );
    if (start === undefined || end === undefined) {
      throw new RangeError(
        'setCapturePunch: both bounds are required (startSample or startSec, endSample or endSec)',
      );
    }
    this.native.setCapturePunch(start, end, request.enabled ?? true);
  }

  setCaptureSource(source: EngineCaptureSource): void {
    this.native.setCaptureSource(source);
  }

  /** Positive values delay capture relative to the punch window. */
  setRecordOffsetSamples(offsetSamples: number): void {
    this.native.setRecordOffsetSamples(offsetSamples);
  }

  setInputMonitor(enabled: boolean, gain = 1): void {
    this.native.setInputMonitor(enabled, gain);
  }

  resetCapture(): void {
    this.native.resetCapture();
  }

  captureStatus(): EngineCaptureStatus {
    return this.native.captureStatus();
  }

  /**
   * Read the recorded samples from the addon-owned capture buffer.
   *
   * Returns one `Float32Array` per capture channel, each sliced to the number
   * of frames recorded so far (see {@link captureStatus}). Call after capture
   * to retrieve the audio written after {@link setCaptureBuffer}. The addon
   * copies that method's inputs, so transferring or detaching the original
   * ArrayBuffers cannot invalidate an active capture.
   */
  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  setGraph(spec: EngineGraphSpec): void {
    this.native.setGraph(spec);
  }

  graphNodeCount(): number {
    return this.native.graphNodeCount();
  }

  graphConnectionCount(): number {
    return this.native.graphConnectionCount();
  }

  /**
   * Renders one block and returns processed channel copies. Input channel
   * buffers are never mutated: the addon copies each plane before adding engine
   * output. Pass zero-filled planes when the engine is the only audio source.
   */
  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  processWithMonitor(channels: Float32Array[]): {
    output: Float32Array[];
    monitor: Float32Array[];
  } {
    return this.native.processWithMonitor(channels);
  }

  /**
   * Renders `channels` offline from the current transport position.
   *
   * Set `finalize: false` to render one chunk of a longer timeline; see
   * {@link RenderOfflineRequest.finalize} and {@link finishOfflineRender}.
   */
  renderOffline(request: RenderOfflineRequest): Float32Array[];
  renderOffline(channels: Float32Array[], blockSize?: number): Float32Array[];
  renderOffline(
    channelsOrRequest: Float32Array[] | RenderOfflineRequest,
    blockSize = 128,
  ): Float32Array[] {
    const request = normalizeRenderOfflineRequest(channelsOrRequest, blockSize);
    return this.native.renderOffline(request.channels, request.blockSize, request.finalize);
  }

  /**
   * Ends a chunked offline render: releases every note the sequencer still
   * holds and flushes the PDC / alignment delay lines. Required after
   * `renderOffline({ finalize: false })`; the finalizing form does it itself.
   *
   * Skipping it leaves every note still sounding at the last chunk held. On an
   * engine-internal instrument the tail simply never releases; on a destination
   * marked external ({@link RealtimeEngine.setMidiDestinationExternal}) the
   * note-ons already left through the external MIDI queue, so the note-offs
   * emitted here are the only ones the receiving device will get and the notes
   * otherwise hang outside the engine.
   */
  finishOfflineRender(): void {
    this.native.finishOfflineRender();
  }

  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    return this.native.bounceOffline(options);
  }

  /**
   * Render the source layer over the span and install it as the only clip.
   * Bakes every audio clip and hosted MIDI instrument through the lane strips,
   * lane inserts and group buses; the master strip, monitor bus, engine graph,
   * capture and metronome stay live and process the frozen clip once on
   * playback. The frozen clip replaces the whole clip set and every MIDI clip is
   * withdrawn in the same call, so nothing plays twice; live MIDI input still
   * reaches the instruments. `numChannels` above the prepared channel count
   * throws.
   */
  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    return this.native.freezeOffline(options);
  }

  drainTelemetry(maxRecords = 1024): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords);
  }

  /**
   * Drain pending stereo meter telemetry records published by the engine's meter tap.
   * Compatibility drain over the queue {@link drainMeterTelemetryWide} reads; use that one alone.
   */
  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  /**
   * Drain pending meter telemetry as per-plane (wide) records. This is the one
   * meter drain: a stereo target's left and right are planes 0 and 1. Use it
   * alone; {@link drainMeterTelemetry} consumes the same queue (every target's
   * records) and stays for compatibility.
   */
  drainMeterTelemetryWide(maxRecords = 1024): EngineMeterTelemetryWide[] {
    return this.native.drainMeterTelemetryWide(maxRecords);
  }

  /**
   * Per-insert gain reduction (dB, <= 0) of a meter target's strip from the last
   * rendered block, in combined insert order (pre-fader inserts, then post-fader).
   * Bypassed and non-dynamics inserts read 0; the meter record's `gainReductionDb`
   * is the minimum of these entries.
   *
   * @param targetId - Meter record target id (0 master, 1..32 lanes, 33..40 buses); the input monitor target yields `[]`
   * @throws If `targetId` is outside the encoded range
   */
  meterTargetInsertGainReduction(targetId: number): number[] {
    return this.native.meterTargetInsertGainReduction(targetId);
  }

  /** Queue a reset of the master integrated-loudness meter. */
  resetMasterLoudnessMeter(renderFrame?: number): void {
    this.native.resetMasterLoudnessMeter(
      resolveRenderFrame('resetMasterLoudnessMeter', renderFrame),
    );
  }

  /**
   * Enable or configure per-target spectrum + vectorscope telemetry.
   *
   * @param intervalFrames - Minimum render-frame gap between published snapshots; `0` disables capture
   * @param bandCount - Requested FFT band resolution (1..64); changing it re-prepares the tap
   * @returns The band count actually applied
   */
  configureScopeTelemetry(intervalFrames: number, bandCount: number): number {
    return this.native.configureScopeTelemetry(intervalFrames, bandCount);
  }

  /** Drain pending spectrum + vectorscope telemetry records published by the engine's scope tap. */
  drainScopeTelemetry(maxRecords = 1024): EngineScopeTelemetry[] {
    return this.native.drainScopeTelemetry(maxRecords);
  }

  /**
   * Push a live parameter value to the engine (immediate jump).
   *
   * @param paramId - Target parameter id
   * @param value - New value
   * @param renderFrame - Render-frame time to apply; omit for immediate (negative is refused)
   *
   * This value also becomes `paramId`'s base value: if an automation lane
   * later starts (and stops) driving `paramId`, the target reverts to this
   * value once that lane empties — see {@link setAutomationLane}.
   * A reserved mixer id (`0x4d58xxxx`) other than a master id names no strip and
   * throws `InvalidParameter`; resolve per-strip ids with the `resolve*AutomationId`
   * methods.
   */
  setParameter(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameter(paramId, value, resolveRenderFrame('setParameter', renderFrame));
  }

  /**
   * Push a live parameter value to the engine using a smoothed ramp. The
   * ramp's target (not its in-flight position) becomes `paramId`'s base
   * value, with the same restore-on-lane-release behavior as
   * {@link setParameter}.
   */
  setParameterSmoothed(paramId: number, value: number, renderFrame?: number): void {
    this.native.setParameterSmoothed(
      paramId,
      value,
      resolveRenderFrame('setParameterSmoothed', renderFrame),
    );
  }

  /**
   * Set the ramp time (ms) of the engine-level smoother. It governs only
   * `setParameterSmoothed` on parameters bound through the automation engine.
   * Reserved engine targets — track and bus faders, pan and width, and insert and
   * instrument parameters — keep their own fixed smoothing whatever this is set
   * to, and MIDI-CC mappings apply their values directly. The default is 20 ms;
   * `0` makes the governed changes instant.
   */
  setParamSmoothingMs(smoothingMs: number): void {
    this.native.setParamSmoothingMs(smoothingMs);
  }

  setSoloMute(laneIndex: number, solo: boolean, mute: boolean, renderFrame?: number): void {
    this.native.setSoloMute(laneIndex, solo, mute, resolveRenderFrame('setSoloMute', renderFrame));
  }

  /** Schedule a per-track PFL/AFL monitor tap at a render-frame boundary. */
  setTrackMonitorMode(laneIndex: number, mode: EngineTrackMonitorMode, renderFrame?: number): void {
    this.native.setTrackMonitorMode(
      laneIndex,
      trackMonitorModeValue(mode),
      resolveRenderFrame('setTrackMonitorMode', renderFrame),
    );
  }

  /**
   * Remove all registered parameters and release their backing strings. Use
   * before re-registering a parameter id (add() rejects duplicate ids). Not
   * realtime-safe.
   */
  clearParameters(): void {
    this.native.clearParameters();
  }

  /**
   * Replace the realtime MIDI clip snapshot. Events are absolute render-frame
   * UMP events compiled for the engine timeline.
   */
  setMidiClips(clips: ReadonlyArray<EngineMidiClipSchedule>): void {
    this.native.setMidiClips(clips);
  }

  /**
   * Bind a built-in synth to a realtime MIDI destination. Live note/CC commands
   * and scheduled MIDI clips routed to that destination render through it.
   */
  setBuiltinInstrument(
    config: BuiltinSynthConfig = {},
    destinationId = config.destinationId ?? 0,
  ): void {
    this.native.setBuiltinInstrument(destinationId, config);
  }

  /**
   * Bind the patch-driven NativeSynth to a realtime MIDI destination. `patch`
   * is a {@link SynthPatch} or a preset-name string (`'saw-lead'` /
   * `'va:saw-lead'`; see {@link synthPresetNames}), resolving exactly like
   * {@link Project.bounceWithSynthInstrument}. Live note/CC commands and
   * scheduled MIDI clips routed to that destination render through the synth.
   * Unknown preset names throw.
   *
   * An `engineMode: 'sample'` patch also carries the {@link SampleBank} its
   * keymap names. The engine takes a share of the bank, so it may be destroyed
   * right after this call; a sample patch bound without one renders silence.
   *
   * `useGmPrograms` follows incoming GM bank/program changes and routes
   * channel 10 through the GM drum map, exactly like the same-named bounce
   * binding key; the patch stays the fallback for any unsupported mapping.
   */
  setSynthInstrument(
    patch: SynthPatch | string = {},
    destinationId = (typeof patch === 'object' ? patch.destinationId : undefined) ?? 0,
  ): void {
    this.native.setSynthInstrument(destinationId, normalizeSynthInstrument(patch));
  }

  /**
   * Load (parse) SoundFont 2 bytes into the engine so SF2 instruments can be
   * bound with {@link setSf2Instrument}. Replaces any previously loaded
   * SoundFont (already-bound SF2 instruments keep the SoundFont they were
   * created with); the input buffer is not referenced after the call.
   */
  loadSoundFont(data: Uint8Array): void {
    this.native.loadSoundFont(data);
  }

  /**
   * Bind a GS-compatible SoundFont player to a realtime MIDI destination, fed
   * by the engine's loaded SoundFont ({@link loadSoundFont}). Live note/CC
   * commands and scheduled MIDI clips routed to that destination render
   * through the player (16 MIDI channels, channel 10 drums, GS NRPN part
   * edits, GS/GM SysEx resets). Without a loaded SoundFont — or for programs
   * the SoundFont does not cover — notes play through the built-in
   * synthesizer GM fallback bank (the data-free floor).
   */
  setSf2Instrument(
    config: Sf2InstrumentConfig = {},
    destinationId = config.destinationId ?? 0,
  ): void {
    this.native.setSf2Instrument(destinationId, config);
  }

  clearMidiInstrument(destinationId = 0): void {
    this.native.clearMidiInstrument(destinationId);
  }

  midiInstrumentCount(): number {
    return this.native.midiInstrumentCount();
  }

  /**
   * Bind a live MIDI CC to an engine automation parameter. The MIDI event still
   * reaches the destination instrument; when bound, its 7-bit value is also
   * mapped into [minValue, maxValue] for `paramId`.
   */
  bindMidiCc(
    channel: number,
    controller: number,
    paramId: number,
    options: MidiCcBindOptions = {},
  ): void {
    this.native.bindMidiCc(
      channel,
      controller,
      paramId,
      options.minValue ?? 0,
      options.maxValue ?? 1,
    );
  }

  /** Bind a 7/14-bit CC, RPN, or NRPN descriptor to a live parameter. */
  bindMidiCcBinding(binding: ProjectMidiCcBinding): void {
    this.native.bindMidiCcBinding(binding);
  }

  clearMidiCcBindings(): void {
    this.native.clearMidiCcBindings();
  }

  midiCcBindingCount(): number {
    return this.native.midiCcBindingCount();
  }

  /**
   * Replace the destination instrument's controller profile with a named preset
   * (see {@link controllerProfileNames}). Unknown names throw rather than
   * resolving to a default, and installing a profile drops every channel's
   * accumulated axis value. An instrument with nowhere to put a profile throws.
   */
  setControllerProfile(destinationId: number, presetName: string): void {
    this.native.setControllerProfile(destinationId, presetName);
  }

  /**
   * Add one {@link ControllerBinding} on top of the destination instrument's
   * current profile. Throws when the table is full, when the axis is `'none'`,
   * when a `poly-pressure` binding names a channel-level axis, or when the
   * range or curve is not finite.
   */
  bindController(destinationId: number, binding: ControllerBinding): void {
    this.native.bindController(destinationId, binding);
  }

  /**
   * Drop every binding of the destination instrument's controller profile. The
   * instrument keeps a profile; it resolves nothing until something is bound.
   */
  clearControllerBindings(destinationId: number): void {
    this.native.clearControllerBindings(destinationId);
  }

  controllerBindingCount(destinationId: number): number {
    return this.native.controllerBindingCount(destinationId);
  }

  /**
   * Declare whether note-on velocity is expression for this instrument. When
   * off, every note is taken at full scale and the bound axes carry the
   * dynamics alone. There is no fixed default — each preset states it.
   */
  setControllerVelocityMeaningful(destinationId: number, meaningful: boolean): void {
    this.native.setControllerVelocityMeaningful(destinationId, meaningful);
  }

  controllerVelocityMeaningful(destinationId: number): boolean {
    return this.native.controllerVelocityMeaningful(destinationId);
  }

  /**
   * Say which note a value addressed to a whole MIDI channel belongs to when
   * several are sounding on it, for one per-note dimension
   * ({@link MPE_DIMENSIONS}, {@link NOTE_TRACKINGS}).
   *
   * Set per dimension because the useful answers differ: pressure following the
   * newest note while bend reaches every one is a real configuration, not a
   * mistake. MPE poses this question and declines to answer it, so this is a
   * choice rather than a rule — and it is read only inside an MPE zone, and
   * only while more than one note is sounding on the channel, which an MPE
   * sender avoids by giving each note its own member channel.
   *
   * Both arguments are required and are a name or its C ordinal; an unknown
   * spelling throws rather than resolving to a default. A destination with no
   * instrument bound throws `InvalidParameter`; one whose instrument holds no
   * controller profile throws `NotSupported`.
   */
  setControllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension | number,
    tracking: NoteTracking | number,
  ): void {
    this.native.setControllerNoteTracking(destinationId, dimension, tracking);
  }

  /**
   * Read back {@link RealtimeEngine.setControllerNoteTracking} for one
   * dimension. An ordinal this binding's name table does not cover comes back
   * as the number itself rather than as a wrong name.
   */
  controllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension | number,
  ): NoteTracking | number {
    return this.native.controllerNoteTracking(destinationId, dimension);
  }

  /**
   * Set how one MIDI channel of the destination's instrument treats a note-on
   * while another note on that channel is still held (see {@link Articulation}).
   * The mode is per channel, so slurring one part leaves the rest of the rack
   * polyphonic.
   *
   * The mode is a name from {@link ARTICULATIONS} or its C ordinal, and is
   * required: an omitted one would mean `'poly'`, which plays every note and
   * slurs none of them — indistinguishable from a request that took. A channel
   * above 15 and an out-of-range mode throw rather than being clamped. A
   * destination with no instrument bound throws `InvalidParameter`; one whose
   * instrument has no articulation of its own throws `NotSupported`, and the
   * two are deliberately different answers.
   */
  setArticulation(
    destinationId: number,
    channel: number,
    articulation: Articulation | number,
  ): void {
    this.native.setArticulation(destinationId, channel, articulation);
  }

  /**
   * Set one part rig of the destination's instrument (see
   * {@link Project.setPartRig} for the request). A destination with no
   * instrument bound throws `InvalidParameter`; an instrument without part rigs
   * (builtin, host callback) throws `NotSupported`.
   */
  setPartRig(request: PartRigRequest): void;
  setPartRig(
    destinationId: number,
    part: number,
    mode: PartRigMode | number,
    inserts?: PartRigInsert[],
  ): void;
  setPartRig(
    requestOrDestinationId: PartRigRequest | number,
    part?: number,
    mode?: PartRigMode | number,
    inserts?: PartRigInsert[],
  ): void {
    const rig = normalizePartRig('setPartRig', requestOrDestinationId, part, mode, inserts);
    this.native.setPartRig(rig.destinationId, rig.part, rig.mode as PartRigMode, rig.insertsJson);
  }

  /**
   * Read back {@link RealtimeEngine.setArticulation} for one channel. An
   * ordinal this binding's name table does not cover comes back as the number
   * itself rather than as a wrong name.
   */
  articulation(destinationId: number, channel: number): Articulation | number {
    return this.native.articulation(destinationId, channel);
  }

  /**
   * How many times a `'mono-legato'` continuation was asked for and refused, so
   * the note started a voice of its own instead. Counted rather than inferred:
   * a refusal sounds like an ordinary note, so nothing in the audio separates
   * "this engine declines legato" from "the mode was never set". Saturates at
   * 2^32 - 1 rather than wrapping.
   *
   * Refuses on the same terms as {@link RealtimeEngine.setArticulation} rather
   * than answering zero: a destination with no instrument bound throws
   * `InvalidParameter`, and one whose instrument has no articulation of its own
   * throws `NotSupported`. An instrument that never had an articulation has
   * refused nothing, and a zero here would read as "every slur took" — the
   * reading this counter exists to prevent.
   */
  legatoFallbackCount(destinationId: number): number {
    return this.native.legatoFallbackCount(destinationId);
  }

  /** Install/replace a live non-destructive MIDI-FX insert for one destination. */
  setMidiFx(destinationId: number, configJson: string): void {
    this.native.setMidiFx(destinationId, configJson);
  }

  clearMidiFx(destinationId = 0): void {
    this.native.clearMidiFx(destinationId);
  }

  /** Enable the engine-owned live MIDI input source for a destination. */
  setMidiInputSource(destinationId = 0): void {
    this.native.setMidiInputSource(destinationId);
  }

  clearMidiInputSource(): void {
    this.native.clearMidiInputSource();
  }

  midiInputPendingCount(): number {
    return this.native.midiInputPendingCount();
  }

  pushMidiInputNoteOn(
    group: number,
    channel: number,
    note: number,
    velocity: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOn(group, channel, note, velocity, portTimeSamples);
  }

  pushMidiInputNoteOff(
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputNoteOff(group, channel, note, velocity, portTimeSamples);
  }

  pushMidiInputCc(
    group: number,
    channel: number,
    controller: number,
    value: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputCc(group, channel, controller, value, portTimeSamples);
  }

  /**
   * Push a live MIDI pitch bend into the engine-owned MIDI input source.
   *
   * Requires {@link RealtimeEngine.setMidiInputSource}; without it the call is
   * refused. The engine drains the queue at block start, so the event lands on
   * the block containing `portTimeSamples`.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383). A value outside
   *   that range is refused rather than wrapped into it.
   * @param portTimeSamples Port-clock timestamp in samples; defaults to 0.
   * @example
   * ```ts
   * engine.setMidiInputSource(destinationId);
   * engine.pushMidiInputPitchBend(0, 0, 16383); // a full bend up
   * ```
   */
  pushMidiInputPitchBend(
    group: number,
    channel: number,
    bend14: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputPitchBend(group, channel, bend14, portTimeSamples);
  }

  /**
   * Push a live MIDI channel pressure (aftertouch) into the engine-owned MIDI
   * input source. Requires {@link RealtimeEngine.setMidiInputSource}.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param pressure 7-bit channel pressure (0..127), applying to every sounding
   *   note on the channel.
   * @param portTimeSamples Port-clock timestamp in samples; defaults to 0.
   */
  pushMidiInputChannelPressure(
    group: number,
    channel: number,
    pressure: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputChannelPressure(group, channel, pressure, portTimeSamples);
  }

  /**
   * Push a live MIDI polyphonic key pressure into the engine-owned MIDI input
   * source. Requires {@link RealtimeEngine.setMidiInputSource}.
   *
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param note Key the pressure belongs to (0..127), so it reaches that voice
   *   alone rather than the whole channel.
   * @param pressure 7-bit key pressure (0..127).
   * @param portTimeSamples Port-clock timestamp in samples; defaults to 0.
   */
  pushMidiInputPolyPressure(
    group: number,
    channel: number,
    note: number,
    pressure: number,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputPolyPressure(group, channel, note, pressure, portTimeSamples);
  }

  pushMidiNoteOn(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiNoteOn(
      destinationId,
      group,
      channel,
      note,
      velocity,
      resolveRenderFrame('pushMidiNoteOn', renderFrame),
    );
  }

  pushMidiNoteOff(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame?: number,
  ): void {
    this.native.pushMidiNoteOff(
      destinationId,
      group,
      channel,
      note,
      velocity,
      resolveRenderFrame('pushMidiNoteOff', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI control change to a MIDI destination. Values
   * are 7-bit; channel 0..15, group 0..15. `renderFrame` is the render-frame
   * time to apply; omit for immediate (negative is refused).
   */
  pushMidiCc(
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiCc(
      destinationId,
      group,
      channel,
      controller,
      value,
      resolveRenderFrame('pushMidiCc', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI pitch bend to a MIDI destination.
   *
   * The bend travels as a single-word MIDI 1.0 UMP rather than as a 7-bit
   * scalar command, which is what carries it at its own width: no 7-bit
   * controller value can spell a 14-bit bend.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param bend14 Unsigned 14-bit bend, centre 8192 (0..16383). A value outside
   *   that range is refused rather than wrapped into it.
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   * @example
   * ```ts
   * engine.pushMidiNoteOn(destinationId, 0, 0, 60, 100);
   * engine.pushMidiPitchBend(destinationId, 0, 0, 16383); // a full bend up
   * ```
   */
  pushMidiPitchBend(
    destinationId: number,
    group: number,
    channel: number,
    bend14: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiPitchBend(
      destinationId,
      group,
      channel,
      bend14,
      resolveRenderFrame('pushMidiPitchBend', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI channel pressure (aftertouch) to a MIDI
   * destination.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param pressure 7-bit channel pressure (0..127), applying to every sounding
   *   note on the channel.
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiChannelPressure(
    destinationId: number,
    group: number,
    channel: number,
    pressure: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiChannelPressure(
      destinationId,
      group,
      channel,
      pressure,
      resolveRenderFrame('pushMidiChannelPressure', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) MIDI polyphonic key pressure to a MIDI
   * destination.
   *
   * @param destinationId MIDI destination id (clip/instrument destination).
   * @param group UMP group (0..15).
   * @param channel MIDI channel (0..15).
   * @param note Key the pressure belongs to (0..127), so it reaches that voice
   *   alone rather than the whole channel.
   * @param pressure 7-bit key pressure (0..127).
   * @param renderFrame Render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiPolyPressure(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
    renderFrame?: number,
  ): void {
    this.native.pushMidiPolyPressure(
      destinationId,
      group,
      channel,
      note,
      pressure,
      resolveRenderFrame('pushMidiPolyPressure', renderFrame),
    );
  }

  /**
   * Queue a MIDI panic (all-notes-off) releasing every sounding note.
   * `renderFrame` is the render-frame time to apply; omit for immediate (negative is refused).
   */
  pushMidiPanic(renderFrame?: number): void {
    this.native.pushMidiPanic(resolveRenderFrame('pushMidiPanic', renderFrame));
  }

  /**
   * Queue an immediate (live) MIDI SysEx message to a MIDI destination. `data`
   * is the full SysEx frame including the leading 0xF0 and trailing 0xF7, and
   * must be 1..512 bytes. `renderFrame` is the render-frame time to apply
   * (omit for immediate). Throws `InvalidParameter` when the destination
   * instrument cannot prepare the SysEx (retrying cannot help), and
   * `OutOfMemory` when the payload slots or the command queue are full
   * (retry after a processed block).
   */
  pushMidiSysex(destinationId: number, data: Uint8Array, renderFrame?: number): void {
    this.native.pushMidiSysex(
      destinationId,
      data,
      resolveRenderFrame('pushMidiSysex', renderFrame),
    );
  }

  /**
   * Queue an immediate (live) raw UMP message to a MIDI destination. `words` is
   * 1 to 4 words, most significant first, and its length must match the message
   * type of `words[0]`. MIDI 2.0 channel-voice messages (MT 0x4) arrive at full
   * width; SysEx7 / data messages (MT 0x3 / 0x5) are refused, use
   * {@link pushMidiSysex}. Throws when the slot ring or command queue is full
   * (retry after a process block). `renderFrame` is the render-frame time to
   * apply (omit for immediate).
   */
  pushMidiUmp(destinationId: number, words: UmpWords, renderFrame?: number): void {
    this.native.pushMidiUmp(destinationId, words, resolveRenderFrame('pushMidiUmp', renderFrame));
  }

  /**
   * Queue a live MIDI 2.0 Program Change to a MIDI destination. The bank
   * travels in the same message and is applied only when `bankValid` is true;
   * a non-zero `bankMsb` or `bankLsb` with `bankValid` false is refused.
   * Program, bank MSB and LSB are 7-bit; group and channel 0..15. The message
   * takes the receive path a wire Program Change takes, so a receiver that does
   * not select programs (the built-in synth, a native synth without GM
   * programs, a part with RX PROGRAM CHANGE off, an MPE member channel)
   * accepts and ignores it. Throws when the slot ring or command queue is full
   * (retry after a process block). `renderFrame` is the render-frame time to
   * apply (omit for immediate); a future-dated op can be evicted by the
   * pending-command bank like any future command.
   */
  pushMidiProgram(
    destinationId: number,
    group: number,
    channel: number,
    program: number,
    bankValid = false,
    bankMsb = 0,
    bankLsb = 0,
    renderFrame?: number,
  ): void {
    this.native.pushMidiProgram(
      destinationId,
      group,
      channel,
      program,
      bankValid,
      bankMsb,
      bankLsb,
      resolveRenderFrame('pushMidiProgram', renderFrame),
    );
  }

  /**
   * Push a live MIDI 2.0 Program Change to the engine-owned MIDI input source.
   * The rules match {@link pushMidiProgram}. `portTimeSamples` is the port
   * timestamp in samples.
   */
  pushMidiInputProgram(
    group: number,
    channel: number,
    program: number,
    bankValid = false,
    bankMsb = 0,
    bankLsb = 0,
    portTimeSamples = 0,
  ): void {
    this.native.pushMidiInputProgram(
      group,
      channel,
      program,
      bankValid,
      bankMsb,
      bankLsb,
      portTimeSamples,
    );
  }

  /**
   * Push one raw UMP message (1 to 4 words) to the engine-owned MIDI input
   * source. The message rules match {@link pushMidiUmp}. `portTimeSamples` is
   * the port timestamp in samples.
   */
  pushMidiInputUmp(words: UmpWords, portTimeSamples = 0): void {
    this.native.pushMidiInputUmp(words, portTimeSamples);
  }

  /**
   * Routes a MIDI destination (a track lane) to the external-MIDI output queue
   * instead of the internal instrument rack, so the track drives an external
   * device. Its sequenced events are buffered for {@link drainExternalMidi}.
   * Clearing it restores internal-synth playback. Control-thread only. The
   * change takes effect at the next processed block; switching a destination's
   * route first releases its notes and resets its controllers through the old
   * route, and drops its pending MIDI-FX events. Single writer.
   */
  setMidiDestinationExternal(destinationId: number, external: boolean): void {
    this.native.setMidiDestinationExternal(destinationId, external);
  }

  /**
   * Enables forwarding MIDI clock (0xF8) and transport (start/continue/stop)
   * bytes to the external output queue, tagged with destination `0xFFFFFFFF`,
   * so external gear stays tempo-synced. Off by default; control-thread only.
   */
  setExternalMidiClockEnabled(enabled: boolean): void {
    this.native.setExternalMidiClockEnabled(enabled);
  }

  /** Number of external-MIDI events dropped because the output queue was full. */
  externalMidiDroppedCount(): number {
    return this.native.externalMidiDroppedCount();
  }

  /** Cumulative page misses dropped because the native bounded request queue was full. */
  clipPageRequestOverflowCount(): number {
    return this.native.clipPageRequestOverflowCount();
  }

  /** Cumulative warp-stretch requests dropped because the native queue was full. */
  warpStretchOverflowCount(): number {
    return this.native.warpStretchOverflowCount();
  }

  /**
   * Sets the number of concurrent time-stretch voices. `voices` must be an
   * integer in `[0, 64]`; a non-integer, negative, or larger value throws and
   * leaves the capacity unchanged. Default is 8. Capacity 0 disables
   * time-stretch, so every warped clip plays resampled instead and none of
   * that counts toward {@link warpStretchOverflowCount}. A change applied
   * while the engine is running restarts the splice state of any clip
   * stretching through a voice at that moment. Control-thread only.
   */
  setWarpVoiceCapacity(voices: number): void {
    this.native.setWarpVoiceCapacity(voices);
  }

  /** Reads the current time-stretch voice capacity (default 8). */
  warpVoiceCapacity(): number {
    return this.native.warpVoiceCapacity();
  }

  /**
   * Drains queued external-MIDI events, already lowered to MIDI 1.0 byte
   * messages so the host can write them straight to an output port. Returns one
   * entry per lowered message; transport/clock bytes carry
   * `destinationId === 0xFFFFFFFF`. `maxRecords` caps the number of output
   * events returned — the shared unit across every surface. Events past the cap
   * stay queued for the next call (lossless); call again to drain the rest.
   *
   * `maxRecords` must be 0 (drain nothing) or at least 4, the most MIDI 1.0
   * messages a single queued event can lower to (a MIDI 2.0 registered or
   * assignable controller becomes CC 101/100 or 99/98 plus Data Entry 6/38):
   * a smaller budget could never
   * consume a record, so it is rejected with a `RangeError` rather than
   * returning an empty array while the queue keeps growing. A negative or
   * fractional value is likewise a `RangeError`, a non-number a `TypeError`.
   */
  drainExternalMidi(maxRecords = 1024): EngineExternalMidiEvent[] {
    return this.native.drainExternalMidi(maxRecords);
  }

  /** Read the current engine transport state (playing/position/ppq/tempo). */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.native.destroy();
  }

  /**

   * Releases the native handle; lets `using` free it automatically (needs TypeScript 5.2+

   * or a runtime with native explicit resource management; Node 22 does not parse `using`).

   */
  [Symbol.dispose](): void {
    this.destroy();
  }
}

export function engineAbiVersion(): number {
  return addon.engineAbiVersion();
}

export function voiceChangerAbiVersion(): number {
  return addon.voiceChangerAbiVersion();
}
