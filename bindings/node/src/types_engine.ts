import type { TimeSignature } from './types_analysis.js';
import type { SendTiming } from './types_mastering.js';
import type { ProjectWarpAnchor, WarpMode } from './types_project.js';

export type EngineTelemetryType = 0 | 1;

export type EngineTelemetryError =
  | 0
  | 1
  | 2
  | 3
  | 4
  | 5
  | 6
  | 7
  | 8
  | 9
  | 10
  | 11
  | 12
  | 13
  | 14
  | 15
  | 16;

export interface EngineTelemetry {
  type: EngineTelemetryType;
  error: EngineTelemetryError;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  graphLatencySamplesQ8: number;
  value: number;
}

/** Meter telemetry record drained from {@link RealtimeEngine.drainMeterTelemetry}. */
export interface EngineMeterTelemetry {
  /** Meter tap target id (e.g. master/bus identifier). */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Left-channel peak level in dB. */
  peakDbL: number;
  /** Right-channel peak level in dB. */
  peakDbR: number;
  /** Left-channel RMS level in dB. */
  rmsDbL: number;
  /** Right-channel RMS level in dB. */
  rmsDbR: number;
  /** Left-channel true-peak level in dB. */
  truePeakDbL: number;
  /** Right-channel true-peak level in dB. */
  truePeakDbR: number;
  /** Maximum true-peak across channels in dB. */
  maxTruePeakDb: number;
  /** Stereo correlation in `[-1, 1]`. */
  correlation: number;
  /** Mono-compatibility width metric. */
  monoCompatWidth: number;
  /** Momentary loudness (LUFS). */
  momentaryLufs: number;
  /** Short-term loudness (LUFS). */
  shortTermLufs: number;
  /** Integrated loudness (LUFS). */
  integratedLufs: number;
  /** Gain reduction in dB. */
  gainReductionDb: number;
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
}

/**
 * One lowered MIDI 1.0 message drained from
 * {@link RealtimeEngine.drainExternalMidi}. A single queued channel-voice event
 * may lower to more than one message (e.g. a MIDI 2.0 program change with bank
 * select), so the drain returns one entry per lowered message.
 */
export interface EngineExternalMidiEvent {
  /**
   * Originating track MIDI destination id, or the transport sentinel
   * `0xFFFFFFFF` (4294967295) for clock / start / continue / stop bytes meant
   * for every external port.
   */
  destinationId: number;
  /**
   * Render-frame coordinate of the event. Channel-voice events carry the
   * timeline sample position; clock/transport bytes carry the device render
   * frame.
   */
  renderFrame: number;
  /** MIDI 1.0 status + data bytes (1..3 entries). */
  bytes: number[];
}

/**
 * Per-plane meter telemetry record drained from
 * {@link RealtimeEngine.drainMeterTelemetryWide} for a surround target. The
 * `peakDb`/`rmsDb`/`truePeakDb` arrays carry `channelCount` planes in canonical
 * WAVE order (5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Lss Rss Ls Rs).
 */
export interface EngineMeterTelemetryWide {
  /** Meter tap target id (e.g. master/bus identifier). */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Number of valid per-plane meters (1..8). */
  channelCount: number;
  /** Per-plane peak level in dB (length `channelCount`). */
  peakDb: number[];
  /** Per-plane RMS level in dB (length `channelCount`). */
  rmsDb: number[];
  /** Per-plane true-peak level in dB (length `channelCount`). */
  truePeakDb: number[];
  /** Maximum true-peak across channels in dB. */
  maxTruePeakDb: number;
  /** Stereo correlation in `[-1, 1]` (front pair). */
  correlation: number;
  /** Mono-compatibility width metric (front pair). */
  monoCompatWidth: number;
  /** Momentary loudness (LUFS). */
  momentaryLufs: number;
  /** Short-term loudness (LUFS). */
  shortTermLufs: number;
  /** Integrated loudness (LUFS). */
  integratedLufs: number;
  /** Gain reduction in dB. */
  gainReductionDb: number;
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
}

/** Scope telemetry record drained from {@link RealtimeEngine.drainScopeTelemetry}. */
export interface EngineScopeTelemetry {
  /** Scope tap target id (0 = master, `laneIndex + 1` for lanes, `33 + busIndex` for buses). */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
  /** FFT magnitude spectrum in dBFS, linear-spaced over `[0, Nyquist]`. */
  bands: number[];
  /**
   * Goniometer/vectorscope sample points as `{ left, right }` objects. The
   * Python surface exposes the same data as `(left, right)` tuples; this
   * representation difference is intentional, the values match.
   */
  points: { left: number; right: number }[];
}

/** Read-only engine transport snapshot from {@link RealtimeEngine.getTransportState}. */
export interface EngineTransportState {
  /** Whether the transport is currently playing. */
  playing: boolean;
  /** @deprecated Use `playing`; kept for one release for compatibility. */
  isPlaying?: boolean;
  /** Whether looping is enabled. */
  looping: boolean;
  /** Current render-frame counter. */
  renderFrame: number;
  /** Current timeline position in samples. */
  samplePosition: number;
  /** Current position in pulses-per-quarter-note. */
  ppq: number;
  /** Current tempo in beats per minute. */
  bpm: number;
  /** Loop start in PPQ. */
  loopStartPpq: number;
  /** Loop end in PPQ. */
  loopEndPpq: number;
  /** Engine sample rate in Hz. */
  sampleRate: number;
  /** PPQ of the current bar's downbeat (derived from the tempo map). */
  barStartPpq: number;
  /** Zero-based index of the current bar. */
  barCount: number;
  /** Time signature in effect at the current PPQ. */
  timeSignature: TimeSignature;
  /** One-based beat within the current bar (`barCount` is zero-based). */
  beat: number;
  /** Fractional position within the current beat, in [0, 1). */
  beatFraction: number;
}

/**
 * Engine automation breakpoint curve as an integer code.
 * Canonical ordinals (matches mixer `AutomationCurve`):
 *   0 = Linear (default), 1 = Exponential, 2 = Hold, 3 = SCurve.
 */
export type EngineAutomationPointCurve =
  | 0
  | 1
  | 2
  | 3
  | 'linear'
  | 'exponential'
  | 'hold'
  | 's-curve';

export interface EngineParameterInfo {
  id: number;
  name: string;
  unit: string;
  minValue: number;
  maxValue: number;
  defaultValue: number;
  rtSafe: boolean;
  defaultCurve: EngineAutomationPointCurve;
}

export interface EngineAutomationPoint {
  ppq: number;
  value: number;
  curveToNext?: EngineAutomationPointCurve;
}

/** Structured marker kind ordinals; mirror SonareMarkerKind / SmfMarkerKind. */
export enum MarkerKind {
  Marker = 0,
  Text = 1,
  Lyric = 2,
  CuePoint = 3,
  KeySignature = 4,
}

export interface EngineMarker {
  id: number;
  ppq: number;
  name?: string;
  /** Marker kind (MarkerKind ordinal); defaults to MarkerKind.Marker. */
  kind?: number;
  /** Key signature only: -7..7 (sharps positive). */
  keyFifths?: number;
  /** Key signature only: false = major, true = minor. */
  keyMinor?: boolean;
}

export interface ProjectMarker {
  id: number;
  ppq: number;
  name: string;
  /** Marker kind (MarkerKind ordinal). */
  kind: number;
  /** Key signature only: -7..7 (sharps positive). */
  keyFifths: number;
  /** Key signature only: false = major, true = minor. */
  keyMinor: boolean;
}

export interface EngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  clickSamples?: number;
  /** Click duration in seconds; used when clickSamples is 0 to derive the click length from the sample rate. */
  clickSeconds?: number;
}

export interface EngineClip {
  id: number;
  trackId?: number;
  channels?: Float32Array[];
  startPpq: number;
  lengthSamples?: number;
  clipOffsetSamples?: number;
  loop?: boolean;
  gain?: number;
  fadeInSamples?: number;
  fadeOutSamples?: number;
  warpMode?: WarpMode;
  warpAnchors?: ProjectWarpAnchor[];
  pageProvider?: number | { readonly id: number };
}

/**
 * Speaker bed layout for a bus or source, mirroring the C enum
 * `SonareChannelLayout`: `0` = mono, `1` = stereo, `2` = 5.1 (L R C LFE Ls Rs),
 * `3` = 7.1 (L R C LFE Lss Rss Ls Rs).
 */
export type ChannelLayout = 0 | 1 | 2 | 3;

export interface EngineTrackLane {
  trackId: number;
  sends?: EngineTrackSend[];
  /**
   * Bus the lane's post-fader output sums into instead of the master mix
   * (group/folder routing); 0 or absent keeps the lane on the master mix.
   */
  outputBusId?: number;
  /**
   * Input channel layout of the source feeding this lane. Absent defaults to
   * stereo. Stored but inert until the surround DSP path lands.
   */
  sourceChannelLayout?: ChannelLayout;
}

export interface EngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
  /**
   * Tap point for the send relative to the lane fader. Absent defaults to
   * post-fader, preserving the behavior before this field existed.
   */
  sendTiming?: SendTiming | number;
}

export interface EngineBus {
  busId: number;
  gainDb?: number;
  /**
   * Channel layout of this bus. Absent defaults to stereo. The master bus
   * carries the project output layout. Stored but inert until the surround DSP
   * path lands.
   */
  channelLayout?: ChannelLayout;
}

export interface ClipPageRequest {
  clipId: number;
  channel: number;
  sample: number;
}

export interface FileClipPageProviderOptions {
  numChannels: number;
  numSamples: number;
  pageFrames: number;
  dataOffsetBytes?: number;
}

export interface EngineCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: 'output' | 'input';
  recordOffsetSamples: number;
}

export type EngineCaptureSource = EngineCaptureStatus['source'] | number;

export interface EngineBounceOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  targetSampleRate?: number;
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
  targetLufs?: number;
  dither?: 0 | 1 | 2 | 3;
  ditherBits?: number;
  ditherSeed?: number;
}

export interface EngineBounceResult {
  interleaved: Float32Array;
  frames: number;
  numChannels: number;
  sampleRate: number;
  integratedLufs: number;
}

export interface EngineFreezeOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  clipId?: number;
  startPpq?: number;
  gain?: number;
}

export interface EngineFreezeResult {
  clipId: number;
  frames: number;
  numChannels: number;
}

export type EngineGraphNodeType = 0 | 1;

/**
 * Mixing intent for a graph edge (`0` = replace, `1` = add).
 *
 * NOTE: not currently honored — the compiled graph always sums edges into a
 * shared destination port in an order-independent way (the first edge into a
 * port overwrites, every later edge adds), regardless of this value. Retained
 * for API compatibility and to express intent; multiple edges into one port are
 * always summed.
 */
export type EngineGraphMix = 0 | 1;

export interface EngineGraphNode {
  id: string;
  type?: EngineGraphNodeType;
  gainDb?: number;
  numPorts?: number;
}

export interface EngineGraphConnection {
  sourceNode: string;
  sourcePort: number;
  destNode: string;
  destPort: number;
  mix?: EngineGraphMix;
}

export interface EngineGraphParameterBinding {
  paramId: number;
  nodeId: string;
}

export interface EngineGraphSpec {
  nodes: EngineGraphNode[];
  connections: EngineGraphConnection[];
  inputNode: string;
  outputNode: string;
  numChannels?: number;
  parameterBindings?: EngineGraphParameterBinding[];
}

/** Options for {@link RealtimeEngine.bindMidiCc}. All fields are optional. */
export interface MidiCcBindOptions {
  /** Lower end of the mapped parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the mapped parameter range. Default `1`. */
  maxValue?: number;
}

/** One absolute render-frame MIDI event accepted by {@link RealtimeEngine.setMidiClips}. */
export interface EngineMidiEvent {
  renderFrame: number;
  /** First UMP word. `data0` is accepted by native wrappers as an alias. */
  word0?: number;
  /** Second UMP word. `data1` is accepted by native wrappers as an alias. */
  word1?: number;
  word2?: number;
  word3?: number;
  wordCount?: number;
  /** UMP group, 0..15. */
  group?: number;
  /** Optional SysEx side-store handle for native hosts. */
  sysexHandle?: number;
  /** Project-style alias for one-word MIDI 1.0 events. */
  data0?: number;
  /** Project-style alias for the second UMP word. */
  data1?: number;
}

/** One compiled realtime MIDI clip schedule accepted by {@link RealtimeEngine.setMidiClips}. */
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
