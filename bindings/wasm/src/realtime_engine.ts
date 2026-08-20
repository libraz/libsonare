import { panLawCode, panModeCode, sendTimingCode, trackMonitorModeCode } from './codes';
import { ErrorCode, SonareError } from './errors';
import { getSonareModule } from './module_state';
import type { ProjectMidiCcBinding, SynthPatch } from './project';
import type { EqBand, PanLawInput, PanMode, SendTiming } from './public_types';
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
  WasmEngineMeterTelemetryWide,
  WasmEngineMetronomeConfig,
  WasmEngineParameterInfo,
  WasmEngineProcessWithMonitorResult,
  WasmEngineScopeTelemetry,
  WasmEngineTelemetry,
  WasmEngineTempoSegment,
  WasmEngineTimeSignatureSegment,
  WasmEngineTransportState,
  WasmExternalMidiEvent,
  WasmRealtimeEngine,
} from './sonare.js';

export type ExternalMidiEvent = WasmExternalMidiEvent;

export type EngineClip = WasmEngineClip;
export type ClipPageRequest = WasmClipPageRequest;
export type EngineParameterInfo = WasmEngineParameterInfo;
export type EngineAutomationPoint = WasmEngineAutomationPoint;
export type EngineMarker = WasmEngineMarker;
export type EngineMetronomeConfig = WasmEngineMetronomeConfig;
export type EngineGraphSpec = WasmEngineGraphSpec;
export type EngineCaptureStatus = WasmEngineCaptureStatus;
export type EngineCaptureSource = EngineCaptureStatus['source'] | number;
export type EngineBounceOptions = WasmEngineBounceOptions;
export type EngineBounceResult = WasmEngineBounceResult;
export type EngineFreezeOptions = WasmEngineFreezeOptions;
export type EngineFreezeResult = WasmEngineFreezeResult;
export type EngineTelemetry = WasmEngineTelemetry;
export type EngineMeterTelemetry = WasmEngineMeterTelemetry;
export type EngineMeterTelemetryWide = WasmEngineMeterTelemetryWide;
export type EngineScopeTelemetry = WasmEngineScopeTelemetry;
export type EngineTransportState = WasmEngineTransportState;
export type EngineTempoSegment = WasmEngineTempoSegment;
export type EngineTimeSignatureSegment = WasmEngineTimeSignatureSegment;

export interface EngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
  /**
   * Pre/post-fader tap point. Defaults to post-fader when omitted, matching the
   * historical lane-send behavior and the scene-JSON default.
   */
  sendTiming?: SendTiming | number;
}

export interface EngineTrackLane {
  trackId: number;
  sends?: EngineTrackSend[];
  /**
   * Bus the lane's post-fader output sums into instead of the master mix
   * (group/folder routing); 0 or absent keeps the lane on the master mix.
   */
  outputBusId?: number;
  /**
   * Input channel layout of the source feeding this lane (`SonareChannelLayout`:
   * 0 mono, 1 stereo, 2 5.1, 3 7.1). Absent defaults to stereo. Stored but inert
   * until the surround DSP path lands.
   */
  sourceChannelLayout?: number;
}

/** Per-track cue/monitor tap mode: off, pre-fader listen, or after-fader listen. */
export type EngineTrackMonitorMode = 'off' | 'pfl' | 'afl' | 0 | 1 | 2;

/** Short alias for {@link EngineTrackMonitorMode}. */
export type TrackMonitorMode = EngineTrackMonitorMode;

export interface EngineBus {
  busId: number;
  gainDb?: number;
  /**
   * Channel layout of the bus (`SonareChannelLayout`: 0 mono, 1 stereo, 2 5.1,
   * 3 7.1). A surround layout makes this a surround group bus: lanes routed to
   * it are surround-panned and it sums into the master plane-by-plane. Defaults
   * to stereo.
   */
  channelLayout?: number;
}

export interface EngineMidiEvent {
  /** Absolute render frame for this event. Default `0`. */
  renderFrame?: number;
  word0?: number;
  word1?: number;
  word2?: number;
  word3?: number;
  wordCount?: number;
  /**
   * Redundant with `word0`, which already carries the UMP group in bits 24..27.
   * The engine reads the group from `word0` — the form that reaches a device or
   * a file — so packing it there is sufficient and a value here that contradicts
   * `word0` is ignored. Must still be in `[0, 15]`; anything else is rejected as
   * a malformed event. Default `0`.
   *
   * Utility (`word0` type nibble `0x0`) and UMP Stream (`0xF`) messages have no
   * group field — those bits are Reserved and `form`/`status` respectively — so
   * they always read as group `0` and packing a group into them has no effect.
   */
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
    maxChannels = 64,
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
    this.native.prepareWithChannels(
      sampleRate,
      maxBlockSize,
      commandCapacity,
      telemetryCapacity,
      maxChannels,
    );
  }

  /** Queue a sample-accurate parameter change (engine kSetParam). */
  setParameter(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameter(paramId, value, renderFrame);
  }

  /** Queue a smoothed parameter change (engine kSetParamSmoothed). */
  setParameterSmoothed(paramId: number, value: number, renderFrame = -1): void {
    this.native.setParameterSmoothed(paramId, value, renderFrame);
  }

  /**
   * Set the default ramp time (ms) for engine-level smoothed parameters —
   * fader/pan glides, insert-parameter automation, and MIDI-CC mappings. The
   * default is 20 ms; pass `0` for instant (un-ramped) changes.
   */
  setParamSmoothingMs(smoothingMs: number): void {
    this.native.setParamSmoothingMs(smoothingMs);
  }

  setSoloMute(laneIndex: number, solo: boolean, mute: boolean, renderFrame = -1): void {
    this.native.setSoloMute(laneIndex, solo, mute, renderFrame);
  }

  /** Queue a per-track PFL/AFL monitor tap mode change. */
  setTrackMonitorMode(laneIndex: number, mode: EngineTrackMonitorMode, renderFrame = -1): void {
    this.native.setTrackMonitorMode(laneIndex, trackMonitorModeCode(mode), renderFrame);
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
    config: {
      destinationId?: number;
      gain?: number;
      polyphony?: number;
      preferModelForModeledFamilies?: boolean;
    } = {},
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

  /**
   * Route a destination's (track lane's) MIDI to the external output queue
   * instead of the internal instrument rack, so the track plays an external
   * device. Clearing it restores internal-synth playback.
   */
  setMidiDestinationExternal(destinationId: number, external: boolean): void {
    this.native.setMidiDestinationExternal(destinationId, external);
  }

  /**
   * Enable/disable forwarding MIDI clock + transport (start/continue/stop) to
   * the external output queue so external gear tracks the transport tempo.
   */
  setExternalMidiClockEnabled(enabled: boolean): void {
    this.native.setExternalMidiClockEnabled(enabled);
  }

  /** Count of external-MIDI events dropped because the output queue was full. */
  externalMidiDroppedCount(): number {
    return this.native.externalMidiDroppedCount();
  }

  externalMidiPendingCount(): number {
    return this.native.externalMidiPendingCount();
  }

  /**
   * Drain queued external-MIDI events, already lowered to MIDI 1.0 byte
   * messages ready to write to a Web MIDI output port. Call once per audio
   * block / animation frame. `maxRecords` caps the number of output events
   * returned — the shared unit across every surface. Events past the cap stay
   * queued for the next call (lossless); call again to drain the rest.
   *
   * One queued record lowers to at most 3 MIDI 1.0 messages, so a positive
   * `maxRecords` below 3 could never consume a record and is rejected with an
   * `InvalidParameter` `SonareError` instead of returning nothing forever.
   */
  drainExternalMidi(maxRecords = 1024): WasmExternalMidiEvent[] {
    return this.native.drainExternalMidi(maxRecords);
  }

  /** Scalar, allocation-free external-MIDI drain for AudioWorklet SAB output. */
  popExternalMidiToScratch(): boolean {
    return this.native.popExternalMidiToScratch();
  }

  externalMidiScratchDestinationId(): number {
    return this.native.externalMidiScratchDestinationId();
  }

  externalMidiScratchRenderFrame(): number {
    return this.native.externalMidiScratchRenderFrame();
  }

  externalMidiScratchByteWord(): number {
    return this.native.externalMidiScratchByteWord();
  }

  externalMidiScratchByteCount(): number {
    return this.native.externalMidiScratchByteCount();
  }

  consumeExternalMidiScratch(): void {
    this.native.consumeExternalMidiScratch();
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

  /** Queue one immediate MIDI 1.0 channel-voice UMP word for a destination. */
  pushMidiUmp(destinationId: number, word0: number, renderFrame = -1): void {
    this.native.pushMidiUmp(destinationId, word0, renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI SysEx frame to a MIDI destination. `data` is
   * the full message including the leading 0xF0 and trailing 0xF7 (1..512
   * bytes). `renderFrame` is the frame to fire at, or -1 for immediate. Mirrors
   * the Node/Python/C-ABI `pushMidiSysex`.
   */
  pushMidiSysex(destinationId: number, data: Uint8Array, renderFrame = -1): void {
    this.native.pushMidiSysex(destinationId, data, renderFrame);
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

  /** Drains queued commands on an offline/control-only engine immediately. */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  seekPpq(ppq: number, renderFrame = -1): void {
    this.native.seekPpq(ppq, renderFrame);
  }

  /** Set a finite tempo in the range (0, 100000] BPM. */
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

  parameterInfoByIndex(index: number): Required<EngineParameterInfo> {
    return this.native.parameterInfoByIndex(index);
  }

  parameterInfo(id: number): Required<EngineParameterInfo> {
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

  /** Set a metronome config; click lengths are limited to one second. */
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

  /**
   * Returns the PCM generated for a tempo-sync clip by the control-thread
   * setter, or `null` when the clip did not require a tempo-sync bake.
   */
  prebakedClipChannels(clipId: number): Float32Array[] | null {
    return this.native.prebakedClipChannels(clipId);
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
        if (!lane.sends) {
          return lane;
        }
        // Normalize each send's pre/post tap point to the integer the native
        // layer reads (defaults to post-fader when omitted).
        return {
          ...lane,
          sends: lane.sends.map((send) => ({
            ...send,
            // Post-fader (0) is the default for an omitted sendTiming.
            sendTiming: send.sendTiming === undefined ? 0 : sendTimingCode(send.sendTiming),
          })),
        };
      }),
    );
  }

  /**
   * Keys one insert of a lane strip from another lane's post-strip audio
   * (ducking/sidechainRouter inserts). sourceTrackId 0 removes the binding.
   */
  setLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): void {
    this.native.setLaneSidechain(trackId, insertIndex, sourceTrackId);
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

  /**
   * Changes one track-strip insert parameter in realtime, addressed by the
   * processor's JSON-key parameter name (see {@link masteringInsertParamInfo}).
   * Applied at the next block head via the engine command queue; safe during
   * playback. Throws if the track, insert, or name is unknown, the param is not
   * realtime-safe, or the command queue is full.
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

  /** Bus-strip counterpart of {@link setTrackStripInsertParamByName}. */
  setBusStripInsertParamByName(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setBusStripInsertParamByName(busId, insertIndex, paramName, value);
  }

  /** Bus-strip counterpart of {@link setTrackStripInsertBypassed}. */
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
   * reserved automation id usable with `setAutomationLane` / `setParameter`.
   * Returns `-1` when the track, insert, or name is unknown. (The Python binding
   * raises a `SonareError` for an unknown id where Node/WASM return the `-1`
   * sentinel.)
   *
   * This trio is how a mastering processor gets time-varying automation: the
   * `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`,
   * `maximizer.*` and `multiband.*` processors are all available as strip
   * inserts, so placing one on a strip and resolving its parameter here drives
   * it at audio-block precision, live and offline alike. The whole-signal
   * stages of the offline mastering chain (`repair.*`, `loudness`, and the
   * match stages) have no insert form and no automation id: they buffer the
   * entire signal by construction and do not run on the realtime path.
   */
  resolveTrackInsertAutomationId(trackId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveTrackInsertAutomationId(trackId, insertIndex, paramName);
  }

  resolveMasterInsertAutomationId(insertIndex: number, paramName: string): number {
    return this.native.resolveMasterInsertAutomationId(insertIndex, paramName);
  }

  resolveBusInsertAutomationId(busId: number, insertIndex: number, paramName: string): number {
    return this.native.resolveBusInsertAutomationId(busId, insertIndex, paramName);
  }

  /**
   * Resolves a hosted instrument's continuous parameter (by its JSON-key name)
   * to the reserved automation id usable with `setAutomationLane` /
   * `setParameter`, so an instrument parameter is driven at audio-block
   * precision exactly like a strip insert. Returns `-1` when the destination
   * has no bound instrument, the instrument exposes no automatable parameters,
   * or the name is unknown.
   *
   * For the NativeSynth ({@link setSynthInstrument}) the names are the
   * continuous {@link SynthPatch} fields: `gain`, `busDrive`, `cutoffHz`,
   * `resonanceQ`, `drive`, `keyTrack`, `envToCutoffCents`, `velToCutoffCents`,
   * `ampAttackMs`, `ampDecayMs`, `ampSustain`, `ampReleaseMs`,
   * `filterAttackMs`, `filterDecayMs`, `filterSustain`, `filterReleaseMs`,
   * `lfoRateHz`, `lfoToPitchCents`, `lfo2RateHz`, `glideMs`, `bodyMix`,
   * `stereoSpread`, `detuneCents`, `driftCents`, `pitchOffsetCents`.
   *
   * Structural fields (`preset`, `engineMode`, `waveform`, `filterModel`,
   * `unison`, `polyphony`, `body`, `modRoutings`) are not automatable and
   * return `-1`: they resize voice pools or swap DSP topology, which is not
   * audio-thread safe. Rebind the instrument with a new patch instead.
   *
   * `gain`, `busDrive`, `cutoffHz`, `resonanceQ`, `envToCutoffCents`,
   * `lfoToPitchCents` and `pitchOffsetCents` reach voices that are already
   * sounding from the next block; the rest are cached into per-voice state at
   * note-on and take effect from the next note.
   *
   * The id survives an unbind/rebind of the same destination and applies
   * nothing while that destination is unbound.
   */
  resolveInstrumentAutomationId(destinationId: number, paramName: string): number {
    return this.native.resolveInstrumentAutomationId(destinationId, paramName);
  }

  /** Sets a track lane strip's pan position in realtime (glitch-free). */
  setTrackStripPan(trackId: number, pan: number): void {
    this.native.setTrackStripPan(trackId, pan);
  }

  /** Sets a track lane strip's pan law in realtime. */
  setTrackStripPanLaw(trackId: number, panLaw: PanLawInput): void {
    this.native.setTrackStripPanLaw(trackId, panLawCode(panLaw));
  }

  /** Sets a track lane strip's pan mode in realtime. */
  setTrackStripPanMode(trackId: number, panMode: PanMode | number): void {
    this.native.setTrackStripPanMode(trackId, panModeCode(panMode));
  }

  /** Sets a track lane strip's dual-pan left/right positions in realtime. */
  setTrackStripDualPan(trackId: number, leftPan: number, rightPan: number): void {
    this.native.setTrackStripDualPan(trackId, leftPan, rightPan);
  }

  /**
   * Sets a track lane strip's inter-channel alignment delay (whole samples).
   * Adjusts strip latency, so PDC and reported graph latency are refreshed.
   */
  setTrackStripChannelDelaySamples(trackId: number, delaySamples: number): void {
    this.native.setTrackStripChannelDelaySamples(trackId, delaySamples);
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

  /**
   * Moves one native request into the binding's persistent scalar scratch.
   * This avoids creating an embind JS object in AudioWorklet process().
   */
  popClipPageRequestToScratch(): boolean {
    return this.native.popClipPageRequestToScratch();
  }

  clipPageRequestScratchClipId(): number {
    return this.native.clipPageRequestScratchClipId();
  }

  clipPageRequestScratchSample(): number {
    return this.native.clipPageRequestScratchSample();
  }

  /** Cumulative page misses dropped because the native bounded request queue was full. */
  clipPageRequestOverflowCount(): number {
    return this.native.clipPageRequestOverflowCount();
  }

  /**
   * Sets the clip-page look-ahead window in timeline frames.
   *
   * The player reports the pages it is *about to* read that are not resident
   * yet, so a streaming host can service them before the audio thread reaches
   * them. Without look-ahead a page miss is only reported after the read
   * already produced silence, which costs one block of silence at every page
   * boundary the host has not primed — the reason a sliding-window streamer
   * cannot keep a live playhead fed from miss reports alone.
   *
   * Look-ahead requests drain through the same `popClipPageRequest` queue and
   * are queued *after* the block's genuine misses, so a host that keeps only
   * the newest request per clip (as {@link ClipPageStreamer} does) tracks the
   * look-ahead frontier.
   *
   * `prepare` defaults this to half a second at the engine's sample rate. `0`
   * disables the look-ahead. A clip whose pages are all resident produces no
   * requests at all, with or without look-ahead. Safe to call during playback.
   */
  setClipPagePrefetchFrames(frames: number): void {
    this.native.setClipPagePrefetchFrames(frames);
  }

  /** Current clip-page look-ahead window in timeline frames. */
  clipPagePrefetchFrames(): number {
    return this.native.clipPagePrefetchFrames();
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

  capturedAudio(): Float32Array[] {
    return this.native.capturedAudio();
  }

  /**
   * Renders in place, adding engine output to `channels`. Zero each plane first
   * when it contains no upstream input.
   */
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
   * Zero each active span first when it contains no upstream input.
   * Allocation-free: safe to call on the AudioWorklet render thread after
   * `prepareChannels`.
   */
  processPrepared(numFrames: number): void {
    this.native.processPrepared(numFrames);
  }

  /**
   * Allocates the cue-bus counterpart of {@link prepareChannels}. Needed only
   * when PFL/AFL monitoring must reach a separate output: `processPrepared`
   * folds the cue bus into the program output, while
   * {@link processPreparedWithMonitor} keeps the two apart. Call once, off the
   * audio thread, with at least as many channels as `prepareChannels` got.
   */
  prepareMonitorChannels(numChannels: number, maxFrames: number): void {
    this.native.prepareMonitorChannels(numChannels, maxFrames);
  }

  /**
   * Returns a Float32Array view onto the persistent cue-bus scratch for one
   * channel (valid for up to `numFrames`). Read it after
   * {@link processPreparedWithMonitor}. Re-acquire after WASM memory growth.
   */
  getMonitorChannelBuffer(channel: number, numFrames: number): Float32Array {
    return this.native.getMonitorChannelBuffer(channel, numFrames);
  }

  /**
   * Runs the engine in place over the prepared scratch, writing the cue bus to
   * the monitor scratch instead of folding it into the program output.
   * Allocation-free: safe on the AudioWorklet render thread after
   * `prepareChannels` and `prepareMonitorChannels`.
   */
  processPreparedWithMonitor(numFrames: number): void {
    this.native.processPreparedWithMonitor(numFrames);
  }

  processWithMonitor(channels: Float32Array[]): WasmEngineProcessWithMonitorResult {
    return this.native.processWithMonitor(channels);
  }

  /**
   * Render `channels` offline from the current transport position. Requesting
   * more planes than `prepare` reserved throws an `InvalidParameter`
   * `SonareError` rather than returning silence that reads as a finished render.
   */
  renderOffline(channels: Float32Array[], blockSize = 128): Float32Array[] {
    return this.native.renderOffline(channels, blockSize);
  }

  /**
   * Bounce the timeline to an interleaved buffer. `numChannels` above the
   * prepared channel count throws an `InvalidParameter` `SonareError`.
   */
  bounceOffline(options: EngineBounceOptions): EngineBounceResult {
    return this.native.bounceOffline(options);
  }

  /**
   * Freeze the current graph to audio. `numChannels` above the prepared channel
   * count throws an `InvalidParameter` `SonareError`.
   */
  freezeOffline(options: EngineFreezeOptions): EngineFreezeResult {
    return this.native.freezeOffline(options);
  }

  drainTelemetry(maxRecords = 1024): EngineTelemetry[] {
    return this.native.drainTelemetry(maxRecords);
  }

  popTelemetryToScratch(): boolean {
    return this.native.popTelemetryToScratch();
  }

  telemetryScratchType(): number {
    return this.native.telemetryScratchType();
  }

  telemetryScratchError(): number {
    return this.native.telemetryScratchError();
  }

  telemetryScratchRenderFrame(): number {
    return Number(this.native.telemetryScratchRenderFrame());
  }

  telemetryScratchTimelineSample(): number {
    return Number(this.native.telemetryScratchTimelineSample());
  }

  telemetryScratchAudibleTimelineSample(): number {
    return Number(this.native.telemetryScratchAudibleTimelineSample());
  }

  telemetryScratchGraphLatencySamplesQ8(): number {
    return this.native.telemetryScratchGraphLatencySamplesQ8();
  }

  telemetryScratchValue(): number {
    return this.native.telemetryScratchValue();
  }

  popMeterTelemetryToScratch(): boolean {
    return this.native.popMeterTelemetryToScratch();
  }
  meterScratchTargetId(): number {
    return this.native.meterScratchTargetId();
  }
  meterScratchRenderFrame(): number {
    return Number(this.native.meterScratchRenderFrame());
  }
  meterScratchValue(field: number): number {
    return this.native.meterScratchValue(field);
  }

  drainMeterTelemetry(maxRecords = 1024): EngineMeterTelemetry[] {
    return this.native.drainMeterTelemetry(maxRecords);
  }

  /**
   * Drains pending meter telemetry as per-plane (wide) records for a surround
   * target. Use this for a surround mix target; {@link drainMeterTelemetry}
   * stays the stereo fast path. The two share one queue — call only one per
   * target. The live AudioWorklet path owns the queue via the stereo drain, so
   * this wide drain is for an offline (non-worklet) engine instance; per-plane
   * surround meters are not delivered over the live worklet meter ring.
   */
  drainMeterTelemetryWide(maxRecords = 1024): EngineMeterTelemetryWide[] {
    return this.native.drainMeterTelemetryWide(maxRecords);
  }

  /**
   * Enables per-target spectrum + vectorscope capture. @param intervalFrames is
   * the minimum render-frame gap between snapshots (0 disables). @param bandCount
   * is the FFT band resolution (1..64); changing it re-prepares the tap. Returns
   * the band count actually applied.
   */
  configureScopeTelemetry(intervalFrames: number, bandCount: number): number {
    return this.native.configureScopeTelemetry(intervalFrames, bandCount);
  }

  /** Drains pending spectrum + vectorscope snapshots (per mix target). */
  drainScopeTelemetry(maxRecords = 1024): EngineScopeTelemetry[] {
    return this.native.drainScopeTelemetry(maxRecords);
  }

  popScopeTelemetryToScratch(): boolean {
    return this.native.popScopeTelemetryToScratch();
  }
  scopeScratchTargetId(): number {
    return this.native.scopeScratchTargetId();
  }
  scopeScratchRenderFrame(): number {
    return Number(this.native.scopeScratchRenderFrame());
  }
  scopeScratchBandCount(): number {
    return this.native.scopeScratchBandCount();
  }
  scopeScratchBand(index: number): number {
    return this.native.scopeScratchBand(index);
  }
  scopeScratchPointCount(): number {
    return this.native.scopeScratchPointCount();
  }
  scopeScratchPointLeft(index: number): number {
    return this.native.scopeScratchPointLeft(index);
  }
  scopeScratchPointRight(index: number): number {
    return this.native.scopeScratchPointRight(index);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  destroy(): void {
    this.native.delete();
  }

  /** Alias for {@link destroy}, matching embind's own release method name. */
  delete(): void {
    this.destroy();
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
