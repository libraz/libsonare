import { ErrorCode, SonareError } from './errors';
import { getSonareModule } from './module_state';
import type { SynthPatch } from './project';
import type { EqBand } from './public_types';
import type {
  WasmClipPageRequest,
  WasmEngineAutomationPoint,
  WasmEngineBounceOptions,
  WasmEngineBounceResult,
  WasmEngineCaptureStatus,
  WasmEngineClip,
  WasmEngineFreezeOptions,
  WasmEngineFreezeResult,
  WasmEngineGraphSpec,
  WasmEngineMarker,
  WasmEngineMeterTelemetry,
  WasmEngineMetronomeConfig,
  WasmEngineParameterInfo,
  WasmEngineProcessWithMonitorResult,
  WasmEngineTelemetry,
  WasmEngineTempoSegment,
  WasmEngineTimeSignatureSegment,
  WasmEngineTransportState,
  WasmRealtimeEngine,
} from './sonare.js';

export type EngineClip = WasmEngineClip;
export type ClipPageRequest = WasmClipPageRequest;
export type EngineParameterInfo = WasmEngineParameterInfo;
export type EngineAutomationPoint = WasmEngineAutomationPoint;
export type EngineMarker = WasmEngineMarker;
export type EngineMetronomeConfig = WasmEngineMetronomeConfig;
export type EngineGraphSpec = WasmEngineGraphSpec;
export type EngineCaptureStatus = WasmEngineCaptureStatus;
export type EngineBounceOptions = WasmEngineBounceOptions;
export type EngineBounceResult = WasmEngineBounceResult;
export type EngineFreezeOptions = WasmEngineFreezeOptions;
export type EngineFreezeResult = WasmEngineFreezeResult;
export type EngineTelemetry = WasmEngineTelemetry;
export type EngineMeterTelemetry = WasmEngineMeterTelemetry;
export type EngineTransportState = WasmEngineTransportState;
export type EngineTempoSegment = WasmEngineTempoSegment;
export type EngineTimeSignatureSegment = WasmEngineTimeSignatureSegment;

export interface EngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
}

export interface EngineTrackLane {
  trackId: number;
  sends?: EngineTrackSend[];
}

export interface EngineBus {
  busId: number;
  gainDb?: number;
}

export interface EngineMidiEvent {
  renderFrame: number;
  word0?: number;
  word1?: number;
  word2?: number;
  word3?: number;
  wordCount?: number;
  group?: number;
  sysexHandle?: number;
  data0?: number;
  data1?: number;
}

export interface EngineMidiClipSchedule {
  id?: number;
  trackId?: number;
  destinationId?: number;
  startSample?: number;
  startPpq?: number;
  lengthSamples?: number;
  loop?: boolean;
  loopLengthSamples?: number;
  events: EngineMidiEvent[];
}

export const EXPECTED_ENGINE_ABI_VERSION = 3;

/** Options for {@link RealtimeEngine.bindMidiCc}. All fields are optional. */
export interface MidiCcBindOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
}

export interface EngineCapabilities {
  engineAbiVersion: number;
  expectedEngineAbiVersion: number;
  abiCompatible: boolean;
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
  mode: 'sab' | 'postMessage';
}

export function engineCapabilities(): EngineCapabilities {
  const abiVersion = getSonareModule().engineAbiVersion();
  const sharedArrayBuffer = typeof globalThis.SharedArrayBuffer === 'function';
  const atomics = typeof globalThis.Atomics === 'object';
  const audioWorklet =
    typeof AudioWorkletNode !== 'undefined' ||
    typeof (globalThis as typeof globalThis & { AudioWorkletProcessor?: unknown })
      .AudioWorkletProcessor !== 'undefined';
  return {
    engineAbiVersion: abiVersion,
    expectedEngineAbiVersion: EXPECTED_ENGINE_ABI_VERSION,
    abiCompatible: abiVersion === EXPECTED_ENGINE_ABI_VERSION,
    sharedArrayBuffer,
    atomics,
    audioWorklet,
    mode: sharedArrayBuffer && atomics ? 'sab' : 'postMessage',
  };
}

export class RealtimeEngine {
  private native: WasmRealtimeEngine;

  constructor(
    sampleRate = 48000,
    maxBlockSize = 128,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ) {
    const module = getSonareModule();
    const capabilities = engineCapabilities();
    if (!capabilities.abiCompatible) {
      throw new Error(
        `Engine ABI mismatch: wasm=${capabilities.engineAbiVersion}, expected=${capabilities.expectedEngineAbiVersion}`,
      );
    }
    this.native = new module.RealtimeEngine(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
    );
  }

  prepare(
    sampleRate: number,
    maxBlockSize: number,
    commandCapacity = 1024,
    telemetryCapacity = 1024,
  ): void {
    this.native.prepare(sampleRate, maxBlockSize, commandCapacity, telemetryCapacity);
  }

  /** Queue a sample-accurate parameter change (engine kSetParam). */
  setParameter(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameter(paramId, value, renderFrame);
  }

  /** Queue a smoothed parameter change (engine kSetParamSmoothed). */
  setParameterSmoothed(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameterSmoothed(paramId, value, renderFrame);
  }

  setSoloMute(laneIndex: number, solo: boolean, mute: boolean, renderFrame = -1): void {
    this.native.setSoloMute(laneIndex, solo, mute, renderFrame);
  }

  setMidiClips(clips: readonly EngineMidiClipSchedule[]): void {
    this.native.setMidiClips(clips);
  }

  setBuiltinInstrument(
    config: { destinationId?: number } & Record<string, unknown> = {},
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
   * Unknown preset names throw. An object patch's `destinationId` is a JS
   * binding convenience, not part of the NativeSynth patch itself.
   */
  setSynthInstrument(
    patch: SynthPatch | string = {},
    destinationId = (typeof patch === 'object' ? patch.destinationId : undefined) ?? 0,
  ): void {
    this.native.setSynthInstrument(destinationId, patch);
  }

  /**
   * Load (parse) SoundFont 2 bytes into the engine so SF2 instruments can be
   * bound with {@link setSf2Instrument}. The host fetches the `.sf2` and
   * passes the raw bytes; they are copied into linear memory for the call and
   * not referenced afterwards. Replaces any previously loaded SoundFont.
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
    config: { destinationId?: number; gain?: number; polyphony?: number } = {},
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

  clearMidiCcBindings(): void {
    this.native.clearMidiCcBindings();
  }

  midiCcBindingCount(): number {
    return this.native.midiCcBindingCount();
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

  pushMidiNoteOn(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiNoteOn(destinationId, group, channel, note, velocity, renderFrame);
  }

  pushMidiNoteOff(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    velocity = 0,
    renderFrame = -1,
  ): void {
    this.native.pushMidiNoteOff(destinationId, group, channel, note, velocity, renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI control change to a MIDI destination
   * (engine kMidiCcImmediate). `group`/`channel` are 0..15; `controller`/`value`
   * are 7-bit (0..127). `renderFrame` is the frame to fire at, or -1 for
   * immediate. Mirrors the Node/Python/C-ABI `pushMidiCc`.
   */
  pushMidiCc(
    destinationId: number,
    group: number,
    channel: number,
    controller: number,
    value: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiCc(destinationId, group, channel, controller, value, renderFrame);
  }

  /**
   * Queue a MIDI panic (all-notes-off) releasing every sounding note at
   * `renderFrame` (-1 = immediate). Mirrors the C-ABI `pushMidiPanic`.
   */
  pushMidiPanic(renderFrame = -1): void {
    this.native.pushMidiPanic(renderFrame);
  }

  /**
   * Remove all registered parameters (and their automation lanes). Control-thread
   * only; not realtime-safe. Mirrors the C-ABI `clearParameters`.
   */
  clearParameters(): void {
    this.native.clearParameters();
  }

  /** Read back the current transport state snapshot. */
  getTransportState(): EngineTransportState {
    return this.native.getTransportState();
  }

  play(renderFrame = -1): void {
    this.native.play(renderFrame);
  }

  stop(renderFrame = -1): void {
    this.native.stop(renderFrame);
  }

  seekSample(timelineSample: number, renderFrame = -1): void {
    this.native.seekSample(timelineSample, renderFrame);
  }

  /**
   * Snaps every in-flight parameter ramp (engine-level smoothed params, mixer
   * lane fader/pan/gate, bus gains) to its target value. Offline renders call
   * this after a priming process() block so the first audible block renders at
   * settled values instead of ramping in from defaults.
   */
  settleParameters(): void {
    this.native.settleParameters();
  }

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  setTempo(bpm: number): void {
    this.native.setTempo(bpm);
  }

  setTempoSegments(segments: readonly EngineTempoSegment[]): void {
    this.native.setTempoSegments([...segments]);
  }

  setTimeSignature(numerator: number, denominator: number): void {
    this.native.setTimeSignature(numerator, denominator);
  }

  setTimeSignatureSegments(segments: readonly EngineTimeSignatureSegment[]): void {
    this.native.setTimeSignatureSegments([...segments]);
  }

  sampleAtPpq(ppq: number): number {
    return Number(this.native.sampleAtPpq(ppq));
  }

  setLoop(startPpq: number, endPpq: number, enabled = true): void {
    this.native.setLoop(startPpq, endPpq, enabled);
  }

  addParameter(info: EngineParameterInfo): void {
    this.native.addParameter(info);
  }

  parameterCount(): number {
    return this.native.parameterCount();
  }

  parameterInfoByIndex(index: number): EngineParameterInfo {
    return this.native.parameterInfoByIndex(index);
  }

  parameterInfo(id: number): EngineParameterInfo {
    return this.native.parameterInfo(id);
  }

  setAutomationLane(paramId: number, points: EngineAutomationPoint[]): void {
    this.native.setAutomationLane(paramId, points);
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

  seekMarker(markerId: number, renderFrame = -1): void {
    this.native.seekMarker(markerId, renderFrame);
  }

  setLoopFromMarkers(startMarkerId: number, endMarkerId: number): void {
    this.native.setLoopFromMarkers(startMarkerId, endMarkerId);
  }

  setMetronome(config: EngineMetronomeConfig): void {
    this.native.setMetronome(config);
  }

  metronome(): Required<EngineMetronomeConfig> {
    return this.native.metronome();
  }

  countInEndSample(startSample: number, bars: number): number {
    return Number(this.native.countInEndSample(startSample, bars));
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

  setClips(clips: EngineClip[]): void {
    this.native.setClips(
      clips.map((clip) => ({
        ...clip,
        pageProvider:
          typeof clip.pageProvider === 'object' && clip.pageProvider !== null
            ? clip.pageProvider.id
            : clip.pageProvider,
      })),
    );
  }

  clipCount(): number {
    return this.native.clipCount();
  }

  setTrackLanes(lanes: Array<number | EngineTrackLane>): void {
    this.native.setTrackLanes(
      lanes.map((lane) => (typeof lane === 'number' ? { trackId: lane } : lane)),
    );
  }

  setTrackBuses(buses: EngineBus[]): void {
    this.native.setTrackBuses(buses);
  }

  setBusStripJson(busId: number, sceneJson: string): void {
    try {
      JSON.parse(sceneJson);
    } catch (error) {
      const message = error instanceof Error ? error.message : 'invalid bus strip JSON';
      throw new SonareError(ErrorCode.InvalidFormat, 'InvalidFormat', message);
    }
    this.native.setBusStripJson(busId, sceneJson);
  }

  setTrackStripJson(trackId: number, sceneJson: string): void {
    try {
      JSON.parse(sceneJson);
    } catch (error) {
      const message = error instanceof Error ? error.message : 'invalid track strip JSON';
      throw new SonareError(ErrorCode.InvalidFormat, 'InvalidFormat', message);
    }
    this.native.setTrackStripJson(trackId, sceneJson);
  }

  setTrackStripEqBand(trackId: number, bandIndex: number, band: EqBand | string): void {
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
    try {
      JSON.parse(sceneJson);
    } catch (error) {
      const message = error instanceof Error ? error.message : 'invalid master strip JSON';
      throw new SonareError(ErrorCode.InvalidFormat, 'InvalidFormat', message);
    }
    this.native.setMasterStripJson(sceneJson);
  }

  setMasterStripEqBand(bandIndex: number, band: EqBand | string): void {
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

  createClipPageProvider(
    numChannels: number,
    numSamples: number,
    pageFrames: number,
  ): ClipPageProvider {
    const id = this.native.createClipPageProvider(numChannels, numSamples, pageFrames);
    return new ClipPageProvider(this, id);
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

  setCaptureBuffer(numChannels: number, capacityFrames: number): void {
    this.native.setCaptureBuffer(numChannels, capacityFrames);
  }

  armCapture(armed = true): void {
    this.native.armCapture(armed);
  }

  setCapturePunch(startSample: number, endSample: number, enabled = true): void {
    this.native.setCapturePunch(startSample, endSample, enabled);
  }

  setCaptureSource(source: EngineCaptureStatus['source']): void {
    this.native.setCaptureSource(source);
  }

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

  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  process(channels: Float32Array[]): Float32Array[] {
    return this.native.process(channels);
  }

  /**
   * Allocates persistent per-channel WASM-heap scratch for the zero-copy
   * `getChannelBuffer` / `processPrepared` realtime path. Call once (off the
   * audio thread) before driving `processPrepared` from an AudioWorklet so the
   * render callback never allocates on the C++/JS heap.
   */
  prepareChannels(numChannels: number, maxFrames: number): void {
    this.native.prepareChannels(numChannels, maxFrames);
  }

  /**
   * Returns a Float32Array view onto the persistent WASM-heap scratch for one
   * channel (valid for up to `numFrames`). Fill it, call `processPrepared`, then
   * read the same view back. Re-acquire after WASM memory growth.
   */
  getChannelBuffer(channel: number, numFrames: number): Float32Array {
    return this.native.getChannelBuffer(channel, numFrames);
  }

  /**
   * Runs the engine in place over the prepared per-channel scratch buffers.
   * Allocation-free: safe to call on the AudioWorklet render thread after
   * `prepareChannels`.
   */
  processPrepared(numFrames: number): void {
    this.native.processPrepared(numFrames);
  }

  processWithMonitor(channels: Float32Array[]): WasmEngineProcessWithMonitorResult {
    return this.native.processWithMonitor(channels);
  }

  renderOffline(channels: Float32Array[], blockSize = 128): Float32Array[] {
    return this.native.renderOffline(channels, blockSize);
  }

  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    return this.native.bounceOffline(options);
  }

  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    return this.native.freezeOffline(options);
  }

  drainTelemetry(maxRecords = 1024): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords);
  }

  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  destroy(): void {
    this.native.delete();
  }
}

export class ClipPageProvider {
  private disposed = false;

  constructor(
    private readonly engine: RealtimeEngine,
    readonly id: number,
  ) {}

  supply(pageIndex: number, channels: Float32Array[]): void {
    if (this.disposed) {
      throw new Error('ClipPageProvider is destroyed');
    }
    this.engine.supplyClipPage(this.id, pageIndex, channels);
  }

  clear(pageIndex: number): void {
    if (this.disposed) {
      return;
    }
    this.engine.clearClipPage(this.id, pageIndex);
  }

  destroy(): void {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.engine.destroyClipPageProvider(this.id);
  }
}
