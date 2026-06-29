import type {
  EngineAutomationPoint,
  EngineBus,
  EngineCaptureStatus,
  EngineClip,
  EngineMarker,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineParameterInfo,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTrackSend,
  EngineTransportState,
  EqBand,
  PanLaw,
  PanMode,
} from '../index';
import { RealtimeEngine } from '../index';
import type { EngineAutomationContext } from './engine-automation';
import * as automation from './engine-automation';
import type { EngineClipContext } from './engine-clips';
import * as clips from './engine-clips';
import type { EngineMarkerContext } from './engine-markers';
import * as markers from './engine-markers';
import { SonareRealtimeEngineNode } from './engine-node';
import {
  buildCaptureConfig,
  buildTransportFacade,
  type CaptureOptions,
  normalizeTrackLanes,
} from './engine-offline';
import type { SonareEngineOptions, SuspendableAudioContext } from './engine-options';
import type { EngineStripContext } from './engine-strips';
import * as strips from './engine-strips';
import { buildMixerLanes, buildTempoSync, resolveParamId, resolveTargetId } from './engine-sync';
import type {
  SonareEngineInstrumentSyncMessage,
  SonareEngineSyncCaptureMessage,
  SonareEngineSyncMessage,
  SonareEngineTransportFacade,
  SonareRealtimeEngineNodeCapabilities,
  SonareWorkletExternalMidiEvent,
} from './messages';
import {
  ENGINE_MIXER_PARAM_FADER_DB,
  ENGINE_MIXER_PARAM_PAN,
  engineMixerBusTarget,
  engineMixerLaneTarget,
  engineMixerMasterTarget,
  SonareEngineCommandType,
  type SonareEngineTelemetryRecord,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletScopeSnapshot,
} from './protocol';

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
    this.transport = buildTransportFacade({
      sampleRate: this.sampleRate,
      realtimeNode: this.realtimeNode,
      offlineEngine: this.offlineEngine,
      setTransportPlaying: (playing) => {
        this.transportPlaying = playing;
      },
      flushPendingInstrumentSync: () => this.flushPendingInstrumentSync(),
      setTempo: (bpm) => this.setTempo(bpm),
      setTempoSegments: (segments) => this.setTempoSegments(segments),
      setLoop: (startPpq, endPpq, enabled) => this.setLoop(startPpq, endPpq, enabled),
    });
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
    automation.scheduleParam(this.automationContext, nodeId, param, ppq, value, curve);
  }

  addAutomationPoint(
    laneId: string | number,
    ppq: number,
    value: number,
    curve: number | 'linear' | 'exponential' = 'linear',
  ): void {
    automation.addAutomationPoint(this.automationContext, laneId, ppq, value, curve);
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
   * Resolves a track-lane insert parameter (JSON-key name) to the reserved
   * insert-automation id fed straight to setAutomationLane. Declares the track's
   * mixer lane first (like automationParamId) so the offline engine resolves the
   * same strip selector the realtime engine uses.
   *
   * @param target Track id (declares a mixer lane on first use).
   * @param insertIndex Index into the strip's combined insert sequence.
   * @param paramName Processor JSON-key parameter name.
   * @returns Reserved insert-automation id, or -1 when strip/insert/key unknown.
   */
  resolveTrackInsertAutomationId(
    target: string | number,
    insertIndex: number,
    paramName: string,
  ): number {
    const laneIndex = this.ensureTrackLane(target);
    return this.offlineEngine.resolveTrackInsertAutomationId(
      this.trackLaneIds[laneIndex],
      insertIndex,
      paramName,
    );
  }

  /**
   * Resolves a master-strip insert parameter to its reserved insert-automation
   * id.
   *
   * @param insertIndex Index into the master strip's insert sequence.
   * @param paramName Processor JSON-key parameter name.
   * @returns Reserved insert-automation id, or -1 when insert/key unknown.
   */
  resolveMasterInsertAutomationId(insertIndex: number, paramName: string): number {
    return this.offlineEngine.resolveMasterInsertAutomationId(insertIndex, paramName);
  }

  /**
   * Resolves a bus-strip insert parameter to its reserved insert-automation id.
   * Declares the mixer bus first so the offline engine resolves the same bus
   * selector.
   *
   * @param busId Bus id (declares the mixer bus on first use).
   * @param insertIndex Index into the bus strip's insert sequence.
   * @param paramName Processor JSON-key parameter name.
   * @returns Reserved insert-automation id, or -1 when bus/insert/key unknown.
   */
  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    this.ensureBus(busId);
    return this.offlineEngine.resolveBusInsertAutomationId(busId, insertIndex, paramName);
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
    return this.sendSmoothedParam(this.stripParamId(target, ENGINE_MIXER_PARAM_FADER_DB), db);
  }

  setStripPan(target: string | number, pan: number): boolean {
    return this.sendSmoothedParam(this.stripParamId(target, ENGINE_MIXER_PARAM_PAN), pan);
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
    const { entries, ids } = normalizeTrackLanes(this.trackLaneIds, lanes);
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
    return this.sendSmoothedParam(engineMixerBusTarget(busIndex, ENGINE_MIXER_PARAM_FADER_DB), db);
  }

  setTrackStripJson(target: string | number, sceneJson: string): void {
    const laneIndex = this.ensureTrackLane(target);
    const trackId = this.trackLaneIds[laneIndex];
    strips.setTrackStripJson(this.stripContext, trackId, sceneJson, this.trackStripJson);
    this.syncMixer();
  }

  setTrackStripEqBand(target: string | number, bandIndex: number, band: EqBand | string): void {
    strips.setTrackStripEqBand(this.stripContext, target, bandIndex, band);
  }

  setTrackStripInsertBypassed(
    target: string | number,
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    strips.setTrackStripInsertBypassed(
      this.stripContext,
      target,
      insertIndex,
      bypassed,
      resetOnBypass,
    );
  }

  setTrackStripInsertParamByName(
    target: string | number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    strips.setTrackStripInsertParamByName(this.stripContext, target, insertIndex, paramName, value);
  }

  setTrackStripPan(target: string | number, pan: number): void {
    strips.setTrackStripPan(this.stripContext, target, pan);
  }

  setTrackStripPanLaw(target: string | number, panLaw: PanLaw | number): void {
    strips.setTrackStripPanLaw(this.stripContext, target, panLaw);
  }

  setTrackStripPanMode(target: string | number, panMode: PanMode | number): void {
    strips.setTrackStripPanMode(this.stripContext, target, panMode);
  }

  setTrackStripDualPan(target: string | number, leftPan: number, rightPan: number): void {
    strips.setTrackStripDualPan(this.stripContext, target, leftPan, rightPan);
  }

  setTrackStripChannelDelaySamples(target: string | number, delaySamples: number): void {
    strips.setTrackStripChannelDelaySamples(this.stripContext, target, delaySamples);
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
    strips.setMasterStripEqBand(this.stripContext, bandIndex, band);
  }

  setMasterStripInsertBypassed(
    insertIndex: number,
    bypassed: boolean,
    resetOnBypass = false,
  ): void {
    strips.setMasterStripInsertBypassed(this.stripContext, insertIndex, bypassed, resetOnBypass);
  }

  setMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    strips.setMasterStripInsertParamByName(this.stripContext, insertIndex, paramName, value);
  }

  setBusStripInsertParamByName(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.ensureBus(busId);
    strips.setBusStripInsertParamByName(this.stripContext, busId, insertIndex, paramName, value);
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
    return clips.addClip(this.clipContext, trackId, buffer, startPpq, opts);
  }

  removeClip(clipId: number): void {
    clips.removeClip(this.clipContext, clipId);
  }

  setMidiClips(schedules: readonly EngineMidiClipSchedule[]): void {
    clips.setMidiClips(this.clipContext, schedules);
  }

  setBuiltinInstrument(
    trackId: string | number,
    config: { destinationId?: number } & Record<string, unknown> = {},
  ): void {
    strips.setBuiltinInstrument(this.stripContext, trackId, config);
  }

  setSynthInstrument(trackId: string | number, patch: Record<string, unknown> | string = {}): void {
    strips.setSynthInstrument(this.stripContext, trackId, patch);
  }

  loadSoundFont(data: Uint8Array): void {
    strips.loadSoundFont(this.stripContext, data);
  }

  setSf2Instrument(
    trackId: string | number,
    config: { destinationId?: number; gain?: number; polyphony?: number } = {},
  ): void {
    strips.setSf2Instrument(this.stripContext, trackId, config);
  }

  /**
   * Route a track's MIDI to the external output (drained via {@link onMidiOut})
   * instead of an internal instrument, so the track plays an external device.
   * Pass `external=false` to restore internal-synth playback.
   */
  setMidiDestinationExternal(trackId: string | number, external: boolean): void {
    strips.setMidiDestinationExternal(this.stripContext, trackId, external);
  }

  /**
   * Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
   * the external output so external gear tracks the transport tempo. The bytes
   * arrive through {@link onMidiOut} tagged with the transport destination.
   */
  setExternalMidiClockEnabled(enabled: boolean): void {
    strips.setExternalMidiClockEnabled(this.stripContext, enabled);
  }

  /**
   * Install or replace a live, non-destructive MIDI-FX insert for one
   * destination. The insert transforms the destination's MIDI before
   * synthesis (transpose, quantize, velocity shaping, humanize, harmonize,
   * arpeggiate) without rewriting any stored notes, so it can be bypassed by
   * {@link clearMidiFx}. The config JSON is the flat object the engine's
   * MIDI-FX accepts (the same schema as the offline `Project.bakeMidiFx`).
   */
  setMidiFx(trackId: string | number, configJson: string): void {
    strips.setMidiFx(this.stripContext, trackId, configJson);
  }

  /** Remove the live MIDI-FX insert from one destination (a no-op when none). */
  clearMidiFx(trackId: string | number): void {
    strips.clearMidiFx(this.stripContext, trackId);
  }

  pushMidiNoteOn(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame = -1,
  ): void {
    strips.pushMidiNoteOn(this.stripContext, trackId, group, channel, note, velocity, renderFrame);
  }

  pushMidiNoteOff(
    trackId: string | number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame = -1,
  ): void {
    strips.pushMidiNoteOff(this.stripContext, trackId, group, channel, note, velocity, renderFrame);
  }

  pushMidiCc(
    trackId: string | number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame = -1,
  ): void {
    strips.pushMidiCc(this.stripContext, trackId, group, channel, controller, value, renderFrame);
  }

  pushMidiPanic(renderFrame = -1): void {
    this.offlineEngine.pushMidiPanic(renderFrame);
    this.postSync({ type: 'syncMidiPanic', renderFrame });
  }

  configureCapture(options: CaptureOptions): void {
    const config = buildCaptureConfig(options, this.offlineChannelCount);
    this.offlineEngine.setCaptureBuffer(config.channels, config.bufferFrames);
    this.offlineEngine.setCaptureSource(config.source);
    this.offlineEngine.setRecordOffsetSamples(config.recordOffsetSamples);
    this.offlineEngine.setInputMonitor(config.inputMonitor.enabled, config.inputMonitor.gain);
    this.captureConfig = config;
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
    return markers.addMarker(this.markerContext, ppq, name);
  }

  /**
   * Replaces the whole marker set in one call. Entries without an `id` are
   * assigned fresh ids; entries carrying an `id` keep it. Returns the resolved
   * markers in the order given.
   */
  setMarkers(entries: ReadonlyArray<{ ppq: number; name?: string; id?: number }>): EngineMarker[] {
    return markers.setMarkers(this.markerContext, entries);
  }

  markerCount(): number {
    return markers.markerCount(this.markerContext);
  }

  markerByIndex(index: number): EngineMarker {
    return markers.markerByIndex(this.markerContext, index);
  }

  marker(markerId: number): EngineMarker {
    return markers.marker(this.markerContext, markerId);
  }

  seekMarker(markerId: number): boolean {
    return markers.seekMarker(this.markerContext, markerId);
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): boolean {
    return markers.setLoopFromMarkers(this.markerContext, startMarkerId, endMarkerId);
  }

  async renderOffline(totalFrames: number): Promise<Float32Array[]> {
    const frames = Math.max(0, Math.floor(totalFrames));
    const inputs: Float32Array[] = [];
    for (let ch = 0; ch < this.offlineChannelCount; ch++) {
      inputs.push(new Float32Array(frames));
    }
    return this.offlineEngine.renderOffline(inputs, this.offlineBlockSize);
  }

  /**
   * Subscribe to external-MIDI batches (already lowered to MIDI 1.0 bytes) for
   * delivery to Web MIDI output ports. Fires once per render block that
   * produced events. Returns an unsubscribe function.
   */
  onMidiOut(callback: (events: SonareWorkletExternalMidiEvent[]) => void): () => void {
    return this.realtimeNode.onMidiOut(callback);
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

  private mixerLanes(): EngineTrackLane[] {
    return buildMixerLanes(this.trackLaneIds, this.trackSends, this.trackOutputBus);
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
    this.postSync(
      buildTempoSync(
        this.tempoBpm,
        this.timeSignature,
        this.tempoSegments,
        this.timeSignatureSegments,
      ),
    );
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

  // Collaborator surface handed to the strip/pan/EQ/insert/MIDI free functions
  // so they can mirror into the offline engine, post sync messages, and resolve
  // lanes without each holding a back-reference to the whole engine.
  private get stripContext(): EngineStripContext {
    return {
      offlineEngine: this.offlineEngine,
      trackLaneIds: this.trackLaneIds,
      postSync: (message) => this.postSync(message),
      postInstrumentSync: (message) => this.postInstrumentSync(message),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      resolveTargetId: (target) => this.resolveTargetId(target),
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

  // Collaborator surface handed to the audio/MIDI clip scheduling free
  // functions so they can mutate the clip stores, mirror into the offline
  // engine, and post clip-sync messages without holding a back-reference.
  private get clipContext(): EngineClipContext {
    return {
      offlineEngine: this.offlineEngine,
      clips: this.clips,
      midiClips: this.midiClips,
      allocateClipId: () => this.nextClipId++,
      postSync: (message) => this.postSync(message),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      resolveTargetId: (target) => this.resolveTargetId(target),
    };
  }

  // Collaborator surface handed to the marker free functions so they can mutate
  // the marker store and id counter, mirror into the offline engine, post
  // marker-sync messages, and drive transport without a back-reference.
  private get markerContext(): EngineMarkerContext {
    return {
      offlineEngine: this.offlineEngine,
      markers: this.markers,
      getNextMarkerId: () => this.nextMarkerId,
      setNextMarkerId: (value) => {
        this.nextMarkerId = value;
      },
      postSync: (message) => this.postSync(message),
      sendCommand: (command) => this.realtimeNode.sendCommand(command),
      setLoop: (startPpq, endPpq, enabled) => this.setLoop(startPpq, endPpq, enabled),
    };
  }

  // Resolves the reserved mixer parameter id for a fader/pan target, declaring a
  // track lane on first use; 'master' addresses the master strip namespace.
  private stripParamId(target: string | number, paramKind: number): number {
    if (target === 'master') {
      return engineMixerMasterTarget(paramKind);
    }
    return engineMixerLaneTarget(this.ensureTrackLane(target), paramKind);
  }

  // Mirrors a smoothed parameter into the offline engine and pushes a
  // sample-accurate smoothed-param command to the realtime runtime.
  private sendSmoothedParam(paramId: number, value: number): boolean {
    this.offlineEngine.setParameter(paramId, value);
    return this.realtimeNode.sendCommand({
      type: SonareEngineCommandType.SetParamSmoothed,
      targetId: paramId,
      sampleTime: -1,
      argFloat: value,
    });
  }

  private resolveParamId(nodeId: string, param: string | number): number {
    return resolveParamId(this.listParameters(), nodeId, param);
  }

  private resolveTargetId(target: string | number): number {
    return resolveTargetId(target);
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
}
