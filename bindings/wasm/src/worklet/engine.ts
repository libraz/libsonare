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
import type { EngineCaptureContext } from './engine-capture-facade';
import * as capture from './engine-capture-facade';
import type { EngineClipContext } from './engine-clips';
import * as clips from './engine-clips';
import type { EngineMarkerContext } from './engine-markers';
import * as markers from './engine-markers';
import type { EngineMixerContext } from './engine-mixer-facade';
import * as mixer from './engine-mixer-facade';
import { SonareRealtimeEngineNode } from './engine-node';
import { buildTransportFacade, type CaptureOptions } from './engine-offline';
import type { SonareEngineOptions, SuspendableAudioContext } from './engine-options';
import type { EngineParameterContext } from './engine-parameter-facade';
import * as parameter from './engine-parameter-facade';
import type { EngineStripContext } from './engine-strips';
import * as strips from './engine-strips';
import { resolveParamId, resolveTargetId } from './engine-sync';
import type { EngineTempoContext } from './engine-tempo-facade';
import * as tempo from './engine-tempo-facade';
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
    tempo.setTempo(this.tempoContext, bpm);
  }

  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    tempo.setTempoSegments(this.tempoContext, segments);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    tempo.setTimeSignature(this.tempoContext, numerator, denominator);
  }

  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    tempo.setTimeSignatureSegments(this.tempoContext, segments);
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): boolean {
    return tempo.setLoop(this.tempoContext, startPpq, endPpq, enabled);
  }

  countInEndSample(startSample: number, bars: number): number {
    return tempo.countInEndSample(this.tempoContext, startSample, bars);
  }

  getTransportState(): Promise<EngineTransportState> {
    return tempo.getTransportState(this.tempoContext);
  }

  cachedTransportState(): EngineTransportState | undefined {
    return tempo.cachedTransportState(this.tempoContext);
  }

  setParam(nodeId: string, param: string | number, value: number): boolean {
    return parameter.setParam(this.parameterContext, nodeId, param, value);
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
    return parameter.automationParamId(this.parameterContext, target, kind);
  }

  /**
   * Returns the automation target id for a bus fader.
   *
   * @param busId Bus id (declares the mixer bus on first use).
   * @returns Reserved engine parameter id for the bus fader gain (dB).
   */
  busAutomationParamId(busId: number): number {
    return parameter.busAutomationParamId(this.parameterContext, busId);
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
    return parameter.resolveTrackInsertAutomationId(
      this.parameterContext,
      target,
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
    return parameter.resolveMasterInsertAutomationId(this.parameterContext, insertIndex, paramName);
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
    return parameter.resolveBusInsertAutomationId(
      this.parameterContext,
      busId,
      insertIndex,
      paramName,
    );
  }

  /**
   * Returns the number of automation lanes installed on the engine, including
   * lanes whose breakpoint list is currently empty.
   *
   * @returns Engine-side automation lane count.
   */
  automationLaneCount(): number {
    return parameter.automationLaneCount(this.parameterContext);
  }

  listParameters(): EngineParameterInfo[] {
    return parameter.listParameters(this.parameterContext);
  }

  setSoloMute(target: string | number, solo: boolean, mute: boolean): boolean {
    return parameter.setSoloMute(this.parameterContext, target, solo, mute);
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
    mixer.setTrackLanes(this.mixerContext, lanes);
  }

  /**
   * Routes a track lane's post-fader output into a declared bus instead of
   * the master mix (group/folder routing); busId 0 restores the master mix.
   */
  setTrackOutputBus(target: string | number, busId: number): void {
    mixer.setTrackOutputBus(this.mixerContext, target, busId);
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
    mixer.setLaneSidechain(this.mixerContext, target, insertIndex, sourceTarget);
  }

  setSends(target: string | number, sends: EngineTrackSend[]): void {
    mixer.setSends(this.mixerContext, target, sends);
  }

  setTrackBuses(buses: EngineBus[]): void {
    mixer.setTrackBuses(this.mixerContext, buses);
  }

  setBusGain(busId: number, db: number): boolean {
    return mixer.setBusGain(this.mixerContext, busId, db);
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
    mixer.setBusStripJson(this.mixerContext, busId, sceneJson);
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

  pushMidiSysex(trackId: string | number, data: Uint8Array, renderFrame = -1): void {
    strips.pushMidiSysex(this.stripContext, trackId, data, renderFrame);
  }

  pushMidiPanic(renderFrame = -1): void {
    this.offlineEngine.pushMidiPanic(renderFrame);
    this.postSync({ type: 'syncMidiPanic', renderFrame });
  }

  configureCapture(options: CaptureOptions): void {
    capture.configureCapture(this.captureContext, options);
  }

  armRecord(trackId: string | number, enabled: boolean): boolean {
    return capture.armRecord(this.captureContext, trackId, enabled);
  }

  punch(inPpq: number, outPpq: number): boolean {
    return capture.punch(this.captureContext, inPpq, outPpq);
  }

  captureStatus(): Promise<EngineCaptureStatus> {
    return capture.captureStatus(this.captureContext);
  }

  capturedAudio(): Promise<Float32Array[]> {
    return capture.capturedAudio(this.captureContext);
  }

  async resetCapture(): Promise<void> {
    return capture.resetCapture(this.captureContext);
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
    return mixer.mixerLanes(this.mixerContext);
  }

  private syncMixer(): void {
    mixer.syncMixer(this.mixerContext);
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

  // Posts an out-of-band control-sync message to the worklet engine processor.
  // Sync messages use a string `type` so the worklet's message handler routes
  // them to receiveSync() (numeric `type` is reserved for SonareEngineCommandRecord).
  private postSync(message: SonareEngineSyncMessage): void {
    if (this.destroyed) {
      return;
    }
    this.realtimeNode.node.port.postMessage(message);
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
      laneSidechains: this.laneSidechains,
      buses: this.buses,
      trackStripJson: this.trackStripJson,
      busStripJson: this.busStripJson,
      postSync: (message) => this.postSync(message),
      ensureTrackLane: (target) => this.ensureTrackLane(target),
      ensureBus: (busId) => this.ensureBus(busId),
      mixerLanes: () => this.mixerLanes(),
      syncMixer: () => this.syncMixer(),
      sendSmoothedParam: (paramId, value) => this.sendSmoothedParam(paramId, value),
      getMasterStripJson: () => this.masterStripJson,
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

  // Collaborator surface handed to the capture/record/punch free functions so
  // they can mirror into and query the offline engine, command the realtime
  // node, and read/write the capture config without a back-reference.
  private get captureContext(): EngineCaptureContext {
    return {
      offlineEngine: this.offlineEngine,
      realtimeNode: this.realtimeNode,
      offlineChannelCount: this.offlineChannelCount,
      postSync: (message) => this.postSync(message),
      getCaptureConfig: () => this.captureConfig,
      setCaptureConfig: (config) => {
        this.captureConfig = config;
      },
      resolveTargetId: (target) => this.resolveTargetId(target),
    };
  }

  // Collaborator surface handed to the parameter / automation-id resolution
  // free functions so they can mirror into and query the offline engine,
  // command the realtime node, and declare lanes/buses without holding a
  // back-reference to the whole engine.
  private get parameterContext(): EngineParameterContext {
    return {
      offlineEngine: this.offlineEngine,
      realtimeNode: this.realtimeNode,
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
