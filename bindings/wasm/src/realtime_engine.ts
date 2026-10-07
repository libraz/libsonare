import {
  panLawCode,
  panModeCode,
  sendTimingCode,
  sidechainCheckFromCode,
  sidechainSourceKindCode,
  trackMonitorModeCode,
} from './codes';
import { ErrorCode, SonareError } from './errors';
import { getSonareModule } from './module_state';
import type {
  Articulation,
  ControllerBinding,
  MpeDimension,
  NoteTracking,
  PartRigInsert,
  PartRigMode,
  PartRigRequest,
  ProjectMidiCcBinding,
  SynthPatch,
} from './project';
import type { ProjectTimeline } from './project_class';
import {
  normalizePartRig,
  normalizeSynthInstrument,
  projectTimelineNativeId,
} from './project_internal';
import type {
  EqBand,
  PanLawInput,
  PanMode,
  SendTiming,
  SidechainCheck,
  SidechainSourceKind,
  SurroundPan,
  UmpWords,
} from './public_types';
import type {
  WasmClipPageRequest,
  WasmEngineAutomationPoint,
  WasmEngineBounceOptions,
  WasmEngineBounceResult,
  WasmEngineBus,
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
  WasmEngineTrackSend,
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
   * 0 mono, 1 stereo, 2 5.1, 3 7.1). Absent defaults to stereo. Only stereo is
   * accepted; any other layout throws until multichannel lanes are implemented.
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
  /**
   * Bus this bus's output sums into instead of the master mix (bus-to-bus
   * routing); 0 or absent keeps it on the master mix.
   */
  outputBusId?: number;
  /**
   * Sends to other buses, in the same shape as a track lane's sends. A
   * pre-fader send taps before `gainDb`, a post-fader one after it.
   */
  sends?: EngineTrackSend[];
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
  /**
   * Linear gain applied to the destination instrument's rendered audio while
   * this clip is the most recently started active clip on it. Absent defaults
   * to `1` (unity).
   */
  gain?: number;
  /**
   * Linear fade lengths over the clip's full length (not per internal loop
   * repeat). Absent defaults to `0` (no fade). `fadeOutSamples` above `0` is
   * rejected when `lengthSamples` is absent or `<= 0` (open-ended): an
   * open-ended clip has no end to fade out towards.
   */
  fadeInSamples?: number;
  fadeOutSamples?: number;
}

export const EXPECTED_ENGINE_ABI_VERSION = 3;

/** Options for {@link RealtimeEngine.bindMidiCc}. All fields are optional. */
export interface MidiCcBindOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
}

/** Request form of {@link RealtimeEngine.renderOffline}. */
export interface RenderOfflineRequest {
  /** One buffer per output plane; their common length is the render span. */
  channels: Float32Array[];
  /** Render block size. Default `128`. */
  blockSize?: number;
  /**
   * Whether this call ends the timeline. `true` (the default, and what a
   * one-shot bounce wants) releases every sounding note and flushes the PDC /
   * alignment delay lines before returning. `false` renders one CHUNK of a
   * longer timeline: a note held across the chunk boundary keeps sounding into
   * the next call and the delay lines carry their history over, so consecutive
   * chunks concatenate to exactly what one continuous render of the same span
   * produces. Call {@link RealtimeEngine.finishOfflineRender} once after the
   * last chunk.
   *
   * Sample-exact concatenation requires every chunk to use the same `blockSize`
   * and a frame count that is a whole number of blocks: each call restarts the
   * block grid at its own frame 0 and renders a short final block for the
   * remainder, and the clip / automation / MIDI-clip snapshots are frozen once
   * per block, so a chunk that ends mid-block shifts every later block
   * boundary. Audio stays continuous either way; only bit-identity is lost.
   */
  finalize?: boolean;
}

const UMP_WORD_MIN = -0x80000000;
const UMP_WORD_MAX = 0xffffffff;

// A word may be spelled `(0x4 << 28) | …`, which is a signed int once bit 31 is
// set, so the signed 32-bit range is accepted alongside the unsigned one.
function assertUmpWords(fnName: string, words: UmpWords): UmpWords {
  if (!(words instanceof Uint32Array) && !Array.isArray(words)) {
    throw new TypeError(`${fnName}: words must be a Uint32Array or a number array`);
  }
  if (words.length < 1 || words.length > 4) {
    throw new RangeError(`${fnName}: words must hold 1 to 4 words`);
  }
  for (let i = 0; i < words.length; i++) {
    const word = words[i];
    if (
      typeof word !== 'number' ||
      !Number.isInteger(word) ||
      word < UMP_WORD_MIN ||
      word > UMP_WORD_MAX
    ) {
      throw new RangeError(`${fnName}: words[${i}] must be an integer 32-bit word`);
    }
  }
  return words;
}

/**
 * One normalizer for both call forms, so the request object and the positional
 * overload cannot drift in their defaults.
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
  private released = false;

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
      throw new SonareError(
        ErrorCode.AbiMismatch,
        'AbiMismatch',
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
   *
   * An `engineMode: 'sample'` patch also carries the {@link SampleBank} its
   * keymap names. The synth takes a share of the bank, so it may be released
   * right after this call; a sample patch bound without one renders silence.
   */
  setSynthInstrument(
    patch: SynthPatch | string = {},
    destinationId = (typeof patch === 'object' ? patch.destinationId : undefined) ?? 0,
  ): void {
    this.native.setSynthInstrument(destinationId, normalizeSynthInstrument(patch));
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
      clearBankRig?: boolean;
      gsEfxRealization?: 'modern' | 'classic';
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

  /**
   * Replace a destination instrument's controller profile with a named preset
   * (see {@link controllerProfileNames}). Installing a profile drops every
   * channel's accumulated axis values: the new bindings say nothing about what
   * the old ones had reached. An unknown name throws, and so does a destination
   * with no instrument or one whose instrument holds no profile.
   */
  setControllerProfile(destinationId: number, presetName: string): void {
    this.native.setControllerProfile(destinationId, presetName);
  }

  /** Add one {@link ControllerBinding} on top of the destination's current profile. */
  bindController(destinationId: number, binding: ControllerBinding): void {
    this.native.bindController(destinationId, binding);
  }

  /**
   * Drop every binding of the destination's controller profile. The instrument
   * keeps a profile; it resolves nothing until something is bound again.
   */
  clearControllerBindings(destinationId: number): void {
    this.native.clearControllerBindings(destinationId);
  }

  controllerBindingCount(destinationId: number): number {
    return this.native.controllerBindingCount(destinationId);
  }

  /**
   * Whether note-on velocity is expression for this instrument. No fixed
   * default is possible — a wind controller ships sending breath-derived
   * velocity on one model and a constant on the next — so each preset states it
   * and a host building its own profile sets it. When false the synth takes
   * every note at full scale and the bound axes carry the dynamics alone.
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
   * Both arguments are required and are a name or its ordinal; an unknown
   * spelling is refused rather than resolved to a default, as are a destination
   * with no instrument and one whose instrument holds no controller profile.
   */
  setControllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension | number,
    tracking: NoteTracking | number,
  ): void {
    this.native.setControllerNoteTracking(destinationId, dimension, tracking);
  }

  /**
   * Read back {@link setControllerNoteTracking} for one dimension, as the
   * canonical name.
   */
  controllerNoteTracking(
    destinationId: number,
    dimension: MpeDimension | number,
  ): NoteTracking | number {
    return this.native.controllerNoteTracking(destinationId, dimension);
  }

  /**
   * Set how one MIDI channel (0–15) of a destination's instrument treats a
   * note-on while another note on that channel is still held: `'poly'` takes a
   * new voice each time, `'mono-retrigger'` stops and restarts the note (what
   * GS MONO MODE and CC126 mean), `'mono-legato'` carries the sounding voice
   * and only moves its pitch — a wind player's slur, which no MIDI message can
   * reach by design.
   *
   * `'mono-legato'` is a request, not a guarantee: an engine whose exciter is
   * spent at the onset — anything struck or plucked — and a target pitch below
   * what the engine's delay line can hold both fall back to an ordinary note,
   * which {@link legatoFallbackCount} counts. A channel outside [0,15] and an
   * articulation outside the enum are refused rather than clamped, and so is a
   * destination with no instrument or one whose instrument has no articulation
   * of its own.
   */
  setArticulation(
    destinationId: number,
    channel: number,
    articulation: Articulation | number,
  ): void {
    this.native.setArticulation(destinationId, channel, articulation);
  }

  /**
   * Set the rig of one part (0-15), or of the destination's default when `part`
   * is `PART_RIG_ALL_PARTS`, on the instrument bound to `destinationId`. A
   * direct call, not a stored edit: a destination with no instrument is an
   * invalid parameter, and an instrument without part rigs (a builtin, a
   * host-callback instrument) is refused as not implemented rather than
   * succeeding quietly. `'chain'` takes 1-8 `inserts`; no other mode does.
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
   * Read back {@link setArticulation} as the canonical name. An ordinal this
   * build cannot spell is handed back as the number, the way every other enum
   * leaves this surface.
   */
  articulation(destinationId: number, channel: number): Articulation | number {
    return this.native.articulation(destinationId, channel);
  }

  /**
   * How many times a legato continuation was asked for on this destination and
   * refused, so the note started a voice of its own instead. Counted rather
   * than inferred: a refusal sounds like an ordinary note, so nothing in the
   * audio separates "this engine declines legato" from "the mode was never
   * set". Saturates at 4294967295 rather than wrapping — matching the C ABI, so
   * the same phrase reports the same number on every surface — after which it
   * reads as "at least this many".
   *
   * Throws on a destination with no instrument, and on one whose instrument has
   * no articulation of its own — the same two refusals
   * {@link setArticulation} keeps apart. Reporting 0 for the second would read
   * as "every slur took", which is the reading this counter exists to prevent.
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

  /**
   * Route a destination's (track lane's) MIDI to the external output queue
   * instead of the internal instrument rack, so the track plays an external
   * device. Clearing it restores internal-synth playback. Control-thread only.
   * The change takes effect at the next processed block; switching a
   * destination's route first releases its notes and resets its controllers
   * through the old route, and drops its pending MIDI-FX events. Single writer.
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
   * One queued record lowers to at most 4 MIDI 1.0 messages (a MIDI 2.0
   * registered or assignable controller becomes CC 101/100 or 99/98 plus Data
   * Entry 6/38), so a positive `maxRecords` below 4 could never consume a record
   * and is rejected with an `InvalidParameter` `SonareError` instead of
   * returning nothing forever.
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
    // embind marshals the int64 render frame as a BigInt; the declared `number`
    // has to be a real number or the first consumer that does arithmetic on it
    // dies with "Cannot mix BigInt". Same normalization as the telemetry, meter
    // and scope scratch frames.
    return Number(this.native.externalMidiScratchRenderFrame());
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

  /**
   * Push a live MIDI pitch bend to the engine-owned MIDI input source.
   *
   * `bend14` is unsigned 14-bit with centre 8192 (0..16383) — the dimension is
   * not 7-bit, so a value past 16383 is refused rather than narrowed. The input
   * source must be enabled with {@link setMidiInputSource} first.
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
   * Push a live MIDI channel pressure to the engine-owned MIDI input source.
   * `pressure` is 7-bit (0..127). Under MPE this is the member channel's
   * per-note pressure.
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
   * Push a live MIDI polyphonic key pressure to the engine-owned MIDI input
   * source. `note` and `pressure` are 7-bit (0..127).
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
   * Queue an immediate (live) MIDI pitch bend to a MIDI destination. `bend14`
   * is unsigned 14-bit with centre 8192 (0..16383); `renderFrame` is the frame
   * to fire at, or -1 for immediate. Mirrors the Node/Python/C-ABI
   * `pushMidiPitchBend`.
   */
  pushMidiPitchBend(
    destinationId: number,
    group: number,
    channel: number,
    bend14: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiPitchBend(destinationId, group, channel, bend14, renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI channel pressure to a MIDI destination.
   * `pressure` is 7-bit (0..127); `renderFrame` is the frame to fire at, or -1
   * for immediate. Mirrors the Node/Python/C-ABI `pushMidiChannelPressure`.
   */
  pushMidiChannelPressure(
    destinationId: number,
    group: number,
    channel: number,
    pressure: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiChannelPressure(destinationId, group, channel, pressure, renderFrame);
  }

  /**
   * Queue an immediate (live) MIDI polyphonic key pressure to a MIDI
   * destination. `note` and `pressure` are 7-bit (0..127); `renderFrame` is the
   * frame to fire at, or -1 for immediate. Mirrors the Node/Python/C-ABI
   * `pushMidiPolyPressure`.
   */
  pushMidiPolyPressure(
    destinationId: number,
    group: number,
    channel: number,
    note: number,
    pressure: number,
    renderFrame = -1,
  ): void {
    this.native.pushMidiPolyPressure(destinationId, group, channel, note, pressure, renderFrame);
  }

  /**
   * Queue an immediate (live) raw UMP message to a MIDI destination. `words` is
   * 1 to 4 words, most significant first, and its length must match the message
   * type of `words[0]`. MIDI 2.0 channel-voice messages (MT 0x4) arrive at full
   * width; SysEx7 / data messages (MT 0x3 / 0x5) are refused, use
   * {@link pushMidiSysex}. Throws when the slot ring or command queue is full
   * (retry after a process block). `renderFrame` is the render-frame time to
   * apply, or -1 for immediate. A bare number is accepted as a one-word
   * message.
   */
  pushMidiUmp(destinationId: number, words: UmpWords | number, renderFrame = -1): void {
    const list = typeof words === 'number' ? [words] : words;
    this.native.pushMidiUmp(destinationId, assertUmpWords('pushMidiUmp', list), renderFrame);
  }

  /**
   * Push one raw UMP message (1 to 4 words) to the engine-owned MIDI input
   * source. The message rules match {@link pushMidiUmp}. `portTimeSamples` is
   * the port timestamp in samples.
   */
  pushMidiInputUmp(words: UmpWords, portTimeSamples = 0): void {
    this.native.pushMidiInputUmp(assertUmpWords('pushMidiInputUmp', words), portTimeSamples);
  }

  /**
   * Queue an immediate (live) MIDI SysEx frame to a MIDI destination. `data` is
   * the full message including the leading 0xF0 and trailing 0xF7 (1..512
   * bytes). `renderFrame` is the frame to fire at, or -1 for immediate. Throws
   * `InvalidParameter` when the destination instrument cannot prepare
   * the SysEx (retrying cannot help), and `OutOfMemory` when the payload
   * slots or the command queue are full (retry after a processed block).
   * Mirrors the Node/Python/C-ABI `pushMidiSysex`.
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

  /** Queues an integrated-loudness reset; short-term and momentary windows are retained. */
  resetMasterLoudnessMeter(renderFrame = -1): void {
    this.native.resetMasterLoudnessMeter(renderFrame);
  }

  /** Reads the immutable factory value for a resolved insert parameter id. */
  insertParameterConstructedValue(paramId: number): number {
    return this.native.insertParameterConstructedValue(paramId);
  }

  play(renderFrame = -1): void {
    this.native.play(renderFrame);
  }

  /**
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset, so
   * controller values set before a loop region are not restored at the wrap.
   */
  stop(renderFrame = -1): void {
    this.native.stop(renderFrame);
  }

  /**
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset, so
   * controller values set before a loop region are not restored at the wrap.
   */
  seekSample(timelineSample: number, renderFrame = -1): void {
    this.native.seekSample(timelineSample, renderFrame);
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
   * At `renderFrame` (negative: the next block head) the lane, bus, master,
   * monitor and graph processors drop their tails, delay lines and envelopes,
   * automation is applied at the transport position and every smoother is
   * snapped to its target, so playback queued after it starts from the state an
   * offline bounce starts from. Strips the host bound to the engine, including
   * host-owned ones, are reset too. Instruments are not reset. Called during
   * playback it cuts running insert tails and delay lines mid-sound, like a
   * seek.
   *
   * @param renderFrame - Block-relative frame to apply at, or negative for the next block head
   * @throws If the command queue is full
   */
  resetProcessorState(renderFrame = -1): void {
    this.native.resetProcessorState(renderFrame);
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

  /** Snap only insert automation slots after structural replay. */
  settleInsertParameters(): void {
    this.native.settleInsertParameters();
  }

  /** Drains queued commands on an offline/control-only engine immediately. */
  flushControlCommands(): void {
    this.native.flushControlCommands();
  }

  /** Applies commands already due on a control-only mirror, retaining future commands. */
  applyCommandsDueNowPreservingFuture(): void {
    this.native.applyCommandsDueNowPreservingFuture();
  }

  /**
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset, so
   * controller values set before a loop region are not restored at the wrap.
   */
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

  /**
   * A loop wrap, seek or stop sends note-offs plus CC64=0, CC121, CC123 and a
   * centred pitch bend on every channel played since the last reset, so
   * controller values set before a loop region are not restored at the wrap.
   */
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

  /**
   * Normalizes each send's pre/post tap point to the integer the native layer
   * reads (defaults to post-fader when omitted). Shared by track lanes and
   * buses, which carry the same send shape.
   */
  private static normalizeSends(sends: EngineTrackSend[]): WasmEngineTrackSend[] {
    return sends.map((send) => ({
      ...send,
      // Post-fader (0) is the default for an omitted sendTiming.
      sendTiming: send.sendTiming === undefined ? 0 : sendTimingCode(send.sendTiming),
    }));
  }

  setTrackLanes(lanes: Array<number | EngineTrackLane>): void {
    this.native.setTrackLanes(
      lanes.map((lane) => {
        if (typeof lane === 'number') {
          return { trackId: lane };
        }
        return {
          ...lane,
          sends: lane.sends ? RealtimeEngine.normalizeSends(lane.sends) : undefined,
        };
      }),
    );
  }

  /**
   * Keys one insert of a lane strip from another lane's post-strip audio
   * (ducking/sidechainRouter inserts). sourceTrackId 0 removes the binding.
   * Lanes are processed in key order and the key is delay-compensated to the
   * destination strip input, so the result depends on neither the lane order
   * nor the block size. Throws, leaving the bindings unchanged, for a lane
   * keying itself, a binding that would close a cycle over the lane bindings,
   * a full binding table and an alignment past the delay ceiling. Not callable
   * while a block is being processed.
   */
  setLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): void {
    this.native.setLaneSidechain(trackId, insertIndex, sourceTrackId);
  }

  /**
   * Installs a compiled project timeline into this stopped engine, all or
   * nothing: tempo and time-signature segments, markers, track lanes, track
   * automation, audio and MIDI clips, and the per-track strips of the project's
   * mixer scene are replaced in full (a later low-level setter on those domains
   * is overwritten by the next apply). Instruments, buses, the master strip,
   * metronome, loop and capture are not touched; bind instruments separately.
   *
   * Throws while the transport is playing, for a timeline the engine cannot
   * hold, and for a scene strip routed from several tracks. A validation
   * failure leaves the engine unchanged. The engine keeps its own reference, so
   * {@link ProjectTimeline.dispose} may follow immediately.
   *
   * Also throws on an unprepared engine and on a timeline whose sample rate
   * differs from the engine's. Once a timeline is applied, a later prepare at
   * another rate is refused until the timeline's clips are cleared.
   */
  applyProjectTimeline(timeline: ProjectTimeline): void {
    // Resolve through the identity registry so no raw native handle crosses the
    // facade boundary and the realtime bundle needs no value class import.
    const timelineId = projectTimelineNativeId(timeline);
    if (timelineId === undefined) {
      throw new TypeError('timeline must be a ProjectTimeline instance');
    }
    this.native.applyProjectTimeline(timelineId);
  }

  setTrackBuses(buses: EngineBus[]): void {
    // Array.isArray guards a caller-fabricated array-like (e.g. `{ length }`)
    // meant to probe the native array-length read: passing it through
    // unmodified lets that guard see the real (missing) length rather than
    // failing here on a `.map` that array-likes do not implement.
    this.native.setTrackBuses(
      Array.isArray(buses)
        ? buses.map((bus) => ({
            ...bus,
            sends: bus.sends ? RealtimeEngine.normalizeSends(bus.sends) : undefined,
          }))
        : (buses as WasmEngineBus[]),
    );
  }

  /**
   * Keys one insert of a bus strip from a track lane or another bus
   * (ducking/sidechainRouter inserts). `sourceId` 0 removes the binding.
   */
  setBusSidechain(
    busId: number,
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): void {
    this.native.setBusSidechain(busId, insertIndex, sidechainSourceKindCode(sourceKind), sourceId);
  }

  /**
   * Keys one insert of the master strip from a track lane or a bus. Same
   * source rules as {@link setBusSidechain}.
   */
  setMasterSidechain(
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): void {
    this.native.setMasterSidechain(insertIndex, sidechainSourceKindCode(sourceKind), sourceId);
  }

  /**
   * Reports whether {@link setLaneSidechain} would accept the binding, without
   * changing anything. `reason` names the first check the setter would fail.
   * Control-thread only: must not be called concurrently with {@link process}.
   */
  canSetLaneSidechain(trackId: number, insertIndex: number, sourceTrackId: number): SidechainCheck {
    return sidechainCheckFromCode(
      this.native.canSetLaneSidechain(trackId, insertIndex, sourceTrackId),
    );
  }

  /** Reports whether {@link setBusSidechain} would accept the binding, without changing anything. */
  canSetBusSidechain(
    busId: number,
    insertIndex: number,
    sourceKind: SidechainSourceKind | number,
    sourceId: number,
  ): SidechainCheck {
    return sidechainCheckFromCode(
      this.native.canSetBusSidechain(
        busId,
        insertIndex,
        sidechainSourceKindCode(sourceKind),
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
    return sidechainCheckFromCode(
      this.native.canSetMasterSidechain(insertIndex, sidechainSourceKindCode(sourceKind), sourceId),
    );
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

  /** Bus-strip counterpart of {@link setTrackStripEqBand}. */
  setBusStripEqBand(busId: number, bandIndex: number, band: EqBand | string): void {
    this.native.setBusStripEqBandJson(
      busId,
      bandIndex,
      typeof band === 'string' ? band : JSON.stringify(band),
    );
  }

  setBusStripEqBandJson(busId: number, bandIndex: number, bandJson: string): void {
    this.native.setBusStripEqBandJson(busId, bandIndex, bandJson);
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
   * processor's JSON-key parameter name — one of the entries
   * {@link masteringInsertParamInfo} reports with a non-null `id`; a
   * construction-only entry (`id` null) takes effect only when the insert is
   * built. Applied at the next block head via the engine command queue; safe
   * during playback. Throws if the track, insert, or name is unknown, the
   * param is not realtime-safe, or the command queue is full.
   */
  setTrackStripInsertParamByName(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.setTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  }

  /** Apply a live insert edit on this engine's owning thread without draining its command queue. */
  applyTrackStripInsertParamByNameNow(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyTrackStripInsertParamByNameNow(trackId, insertIndex, paramName, value);
  }

  /** Restore a retained insert value exactly after a strip scene is replayed. */
  restoreTrackStripInsertParamByName(
    trackId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): void {
    this.native.restoreTrackStripInsertParamByName(trackId, insertIndex, paramName, value);
  }

  /** Master-strip counterpart of {@link setTrackStripInsertParamByName}. */
  setMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.native.setMasterStripInsertParamByName(insertIndex, paramName, value);
  }

  applyMasterStripInsertParamByNameNow(
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyMasterStripInsertParamByNameNow(insertIndex, paramName, value);
  }

  restoreMasterStripInsertParamByName(insertIndex: number, paramName: string, value: number): void {
    this.native.restoreMasterStripInsertParamByName(insertIndex, paramName, value);
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

  applyBusStripInsertParamByNameNow(
    busId: number,
    insertIndex: number,
    paramName: string,
    value: number,
  ): boolean {
    return this.native.applyBusStripInsertParamByNameNow(busId, insertIndex, paramName, value);
  }

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

  clearBusInsertParameterBases(busId: number): void {
    this.native.clearBusInsertParameterBases(busId);
  }

  clearMasterInsertParameterBases(): void {
    this.native.clearMasterInsertParameterBases();
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
   *
   * The returned id uses the track's current positional lane selector. When
   * `setTrackLanes` successfully changes lane order or membership, the engine
   * remaps already queued and published track automation by track id, but it
   * cannot update a numeric id retained by the caller. Re-resolve every track
   * insert id after such a topology change before passing it to
   * `setAutomationLane`, `setParameter`, or `setParameterSmoothed`. Use
   * `setTrackStripInsertParamByName` when the operation needs a stable track
   * identity. Master and bus insert ids are separate and are not invalidated by
   * track-lane changes.
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
   * `stereoSpread`, `detuneCents`, `driftCents`, `pitchOffsetCents`,
   * `hpCutoffHz`, `sampleHoldHz`, `bitDepth`.
   *
   * Structural fields (`preset`, `engineMode`, `waveform`, `filterModel`,
   * `unison`, `polyphony`, `body`, `modRoutings`) are not automatable and
   * return `-1`: they resize voice pools or swap DSP topology, which is not
   * audio-thread safe. Rebind the instrument with a new patch instead.
   *
   * All automatable parameters of a NativeSynth reach already-sounding voices
   * from the next processed block. Voices using GM program patches are
   * unaffected; parameters apply to NativeSynth's own patch only.
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
   * Sets a track lane strip's surround placement in realtime (glitch-free; a
   * constant-power 5 ms one-pole glide). Used when the destination has more than
   * two channels. Omitted fields default to a centered point source
   * (`distance` 1); a `distance` of 0 or less is treated as 1. Throws for an
   * unknown track, a track without a lane strip, or a non-finite field.
   */
  setTrackStripSurroundPan(trackId: number, pan: SurroundPan): void {
    this.native.setTrackStripSurroundPan(trackId, pan);
  }

  /**
   * Sets a bus strip's output pan position in realtime (glitch-free). Throws
   * for an unknown bus or one wider than stereo.
   */
  setBusStripPan(busId: number, pan: number): void {
    this.native.setBusStripPan(busId, pan);
  }

  /** Sets a bus strip's pan law in realtime. */
  setBusStripPanLaw(busId: number, panLaw: PanLawInput): void {
    this.native.setBusStripPanLaw(busId, panLawCode(panLaw));
  }

  /** Sets a bus strip's pan mode in realtime. */
  setBusStripPanMode(busId: number, panMode: PanMode | number): void {
    this.native.setBusStripPanMode(busId, panModeCode(panMode));
  }

  /** Sets a bus strip's dual-pan left/right positions in realtime. */
  setBusStripDualPan(busId: number, leftPan: number, rightPan: number): void {
    this.native.setBusStripDualPan(busId, leftPan, rightPan);
  }

  /**
   * Sets a track lane strip's alignment delay (whole samples), moving this lane
   * later relative to every other lane. It is not latency: PDC does not
   * compensate it and the reported graph latency is unchanged.
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
   * End a chunked offline render: release every note the sequencer still holds
   * and flush the PDC / alignment delay lines. Required after
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
  meterScratchInputPeakDbL(): number {
    return this.native.meterScratchInputPeakDbL();
  }
  meterScratchInputPeakDbR(): number {
    return this.native.meterScratchInputPeakDbR();
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
   * stays the stereo fast path. The two share one queue and each consumes every
   * target's records, so an engine uses only one of them. The live AudioWorklet
   * path owns the queue via the stereo drain, so this wide drain is for an
   * offline (non-worklet) engine instance; per-plane
   * surround meters are not delivered over the live worklet meter ring.
   */
  drainMeterTelemetryWide(maxRecords = 1024): EngineMeterTelemetryWide[] {
    return this.native.drainMeterTelemetryWide(maxRecords);
  }

  /**
   * Per-insert gain reduction in dB (each <= 0) of a meter target's strip from the
   * last rendered block, in combined pre-to-post insert order. `targetId` uses the
   * meter-record encoding (0 master, 1..32 lanes, 33..40 buses; 0xFFFF monitor
   * yields an empty array). Throws for an id outside that range.
   */
  meterTargetInsertGainReduction(targetId: number): number[] {
    return this.native.meterTargetInsertGainReduction(targetId);
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

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.native.delete();
  }

  /** Alias for {@link destroy}, matching embind's own release method name. */
  delete(): void {
    this.destroy();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
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
      throw new SonareError(
        ErrorCode.InvalidState,
        'InvalidState',
        'ClipPageProvider is destroyed',
      );
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

  /** Alias for {@link destroy}, provided for cross-binding compatibility. */
  delete(): void {
    this.destroy();
  }

  /** Releases the provider through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
