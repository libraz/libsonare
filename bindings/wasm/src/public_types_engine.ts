import type { ChannelLayout } from './audio.js';
import type {
  AutomationCurve,
  SendTiming,
  SidechainSourceKind,
  TimeSignature,
} from './public_types.js';
import type { ClipPageProvider } from './realtime_engine.js';

/** Construction and `prepare` settings of {@link RealtimeEngine}. */
export interface RealtimeEngineConfig {
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** Largest block the engine renders, in frames. Default `128`; `0` is refused. */
  maxBlockSize?: number;
  /** Command queue capacity, at most 65536. Default `1024`; `0` is refused. */
  commandCapacity?: number;
  /** Telemetry queue capacity, at most 16384. Default `1024`; `0` is refused. */
  telemetryCapacity?: number;
  /** Most channel planes the engine can render. Default `64`; `0` is refused. */
  maxNumChannels?: number;
}

/**
 * A strip address: a number is a track id, `'master'` is the master strip. Any
 * other string is refused with a `RangeError`, digit-only strings included.
 */
export type StripRef = number | string;

/** Marker kind spellings. */
export type MarkerKind = 'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature';

/** Warp behaviour of a clip. */
export type EngineWarpMode = 'off' | 'repitch' | 'tempoSync' | 'timeStretch';

/** Where {@link RealtimeEngine.captureStatus} says the capture is taken from. */
export type EngineCaptureSource = 'output' | 'input';

/** Dither applied by {@link RealtimeEngine.bounceOffline}; absent means none. */
export type EngineDither = 'rpdf' | 'tpdf' | 'noiseShaped';

/** Kind of an engine telemetry record. */
export type EngineTelemetryType = 'processBlock' | 'error';

/** Error carried by an `'error'` telemetry record. */
export type EngineTelemetryError =
  | 'none'
  | 'commandQueueOverflow'
  | 'pendingCommandOverflow'
  | 'boundaryOverflow'
  | 'telemetryOverflow'
  | 'captureOverflow'
  | 'maxBlockExceeded'
  | 'unknownTarget'
  | 'nonRealtimeSafeParameter'
  | 'notPrepared'
  | 'nonQueueableCommand'
  | 'automationBindTargetOverflow'
  | 'staleAutomationLanes'
  | 'smoothedParameterCapacity'
  | 'commandBacklogDeferred'
  | 'clipPageUnderrun'
  | 'insertAutomationOverflow'
  | 'midiClockOverflow'
  | 'metronomeOverflow'
  | 'invalidCommand'
  | 'maxChannelsExceeded'
  | 'parameterBaseOverflow';

/** Node type of an {@link EngineGraphSpec}. */
export type EngineGraphNodeType = 'passThrough' | 'gain';

/** How a graph connection combines with what its destination port already holds. */
export type EngineGraphMix = 'replace' | 'add';

/** One external-MIDI event drained from the engine, lowered to MIDI 1.0 bytes. */
export interface ExternalMidiEvent {
  /** Originating track destination. Absent on clock and transport bytes, which are meant for every port. */
  destinationId?: number;
  /** Absolute engine frame at which the event fires, never an offset within a block. */
  renderFrame: number;
  /** MIDI 1.0 status and data bytes (1 to 3). */
  bytes: Uint8Array;
}

/** Size and paging of a {@link ClipPageProvider}. */
export interface ClipPageProviderOptions {
  /** Channel count; must be positive. */
  numChannels: number;
  /** Source length in frames; must be positive. */
  numSamples: number;
  /** Frames per page; must be positive. */
  pageFrames: number;
}

/** A clip of audio placed on the engine timeline. */
export interface EngineClip {
  id?: number;
  trackId?: number;
  /** Source audio, one plane per channel. Exactly one of `channels` and `pageProvider`. */
  channels?: Float32Array[];
  startPpq: number;
  lengthSamples?: number;
  clipOffsetSamples?: number;
  loop?: boolean;
  gain?: number;
  fadeInSamples?: number;
  fadeOutSamples?: number;
  /** Default `'off'`. */
  warpMode?: EngineWarpMode;
  warpAnchors?: Array<{ warpSample: number; sourceSample: number }>;
  pageProvider?: ClipPageProvider;
}

/** A parameter registered with {@link RealtimeEngine.addParameter}. */
export interface EngineParameterInfo {
  id: number;
  name: string;
  /** Display unit; omit for an empty string. */
  unit?: string;
  /** Lower end of the parameter range. Default `0`. */
  minValue?: number;
  /** Upper end of the parameter range. Default `1`. */
  maxValue?: number;
  /** Value the parameter starts at. Default `0`. */
  defaultValue?: number;
  /** Whether the parameter is safe to change from the realtime thread. Default `true`. */
  rtSafe?: boolean;
  /** Curve used by automation points that omit `curveToNext`. Default `'exponential'`. */
  defaultCurve?: AutomationCurve;
}

/** One breakpoint of an automation lane. */
export interface EngineAutomationPoint {
  ppq: number;
  value: number;
  /** Curve towards the next point. Default `'linear'`. */
  curveToNext?: AutomationCurve;
}

/** A timeline marker. */
export interface EngineMarker {
  id: number;
  ppq: number;
  name?: string;
  /** Default `'marker'`. */
  kind?: MarkerKind;
  /** Key signature only: -7..7, sharps positive. */
  keyFifths?: number;
  /** Key signature only: `true` for minor. */
  keyMinor?: boolean;
}

/**
 * Metronome settings. The click length is `clickSamples` or `clickSec`, never
 * both; an explicit `0` is refused, so omit it for the 2 ms default.
 */
export interface EngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  /** Click length in samples (1..384000), capped to one second at the engine rate. */
  clickSamples?: number;
  /** Click length in seconds, in (0, 1]. */
  clickSec?: number;
}

/** A node of an {@link EngineGraphSpec}. */
export interface EngineGraphNode {
  id: string;
  /** Default `'passThrough'`. */
  type?: EngineGraphNodeType;
  gainDb?: number;
  numPorts?: number;
}

/** A connection between two graph node ports. */
export interface EngineGraphConnection {
  sourceNode: string;
  sourcePort: number;
  destNode: string;
  destPort: number;
  /** Default `'add'`. */
  mix?: EngineGraphMix;
}

/** Binds an automation parameter id to a graph node. */
export interface EngineGraphParameterBinding {
  paramId: number;
  nodeId: string;
}

/** The processing graph installed by {@link RealtimeEngine.setGraph}. */
export interface EngineGraphSpec {
  nodes: EngineGraphNode[];
  connections: EngineGraphConnection[];
  inputNode: string;
  outputNode: string;
  numChannels: number;
  parameterBindings?: EngineGraphParameterBinding[];
}

/** Punch window of {@link CaptureOptions}: one start spelling and one end spelling. */
export interface CapturePunch {
  /** First timeline quarter note captured. Not with `startSample` or `startSec`. */
  startPpq?: number;
  /** First timeline sample captured. Not with `startPpq` or `startSec`. */
  startSample?: number;
  /** First timeline second captured, rounded to the nearest sample. Not with `startPpq` or `startSample`. */
  startSec?: number;
  /** End quarter note, exclusive. Not with `endSample` or `endSec`. */
  endPpq?: number;
  /** One past the last timeline sample captured. Not with `endPpq` or `endSec`. */
  endSample?: number;
  /** End in timeline seconds, rounded to the nearest sample. Not with `endPpq` or `endSample`. */
  endSec?: number;
}

/**
 * Capture setup for {@link RealtimeEngine.configureCapture}. The call states the
 * whole setup: an omitted field returns to its default, and an omitted `punch`
 * means no punch window.
 */
export interface CaptureOptions {
  /** Capture buffer length in frames. */
  capacityFrames: number;
  /** Capture planes. Default `2`. */
  numChannels?: number;
  /** Default `'output'`. */
  source?: EngineCaptureSource;
  /**
   * Positive values delay capture relative to the punch window. Input arriving
   * within the offset after a loop wrap belongs to the end of the pass played
   * before it. Default `0`.
   */
  recordOffsetSamples?: number;
  /** Input monitoring; `gain` defaults to `1`. An absent `inputMonitor` is enabled at gain `1`, the engine default. */
  inputMonitor?: { enabled: boolean; gain?: number };
  punch?: CapturePunch;
}

/** Capture state read by {@link RealtimeEngine.captureStatus}. */
export interface EngineCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: EngineCaptureSource;
  recordOffsetSamples: number;
}

/** Loop region of {@link RealtimeEngine.setLoop}. */
export interface EngineLoop {
  /** Loop start in quarter notes. */
  startPpq: number;
  /** Loop end in quarter notes. */
  endPpq: number;
  /** Whether looping is on. Default `true`. */
  enabled?: boolean;
}

/** Transport snapshot read by {@link RealtimeEngine.transportState}. */
export interface EngineTransportState {
  playing: boolean;
  looping: boolean;
  renderFrame: number;
  samplePosition: number;
  ppq: number;
  bpm: number;
  loopStartPpq: number;
  loopEndPpq: number;
  sampleRate: number;
  /** Quarter note of the current bar's downbeat. */
  barStartPpq: number;
  /** Zero-based index of the current bar. */
  barCount: number;
  /** Time signature in effect at the current position. */
  timeSignature: TimeSignature;
  /** One-based beat within the current bar. */
  beat: number;
  /** Position within the current beat, in [0, 1). */
  beatFraction: number;
}

/** Offline bounce settings for {@link RealtimeEngine.bounceOffline}. */
export interface EngineBounceOptions {
  /**
   * Length of the bounce in frames. The result is held in memory under a 1 GiB
   * peak budget, so a longer span throws a `RangeError`; the cap is a frame
   * count, inversely proportional to `numChannels`. Render longer material in
   * chunks with `renderOffline({ finalize: false })`.
   */
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  /** Output sample rate. Omit to keep the source rate; `0` is refused. */
  targetSampleRate?: number;
  /** Rate `totalFrames` is counted in. Omit for the engine rate; `0` is refused, and any other rate throws. */
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
  /** Integrated loudness target in LUFS. Omit for -14; non-finite and an explicit `0` are refused. */
  targetLufs?: number;
  /** Omit for no dither. */
  dither?: EngineDither;
  ditherBits?: number;
  ditherSeed?: number;
}

/** Planar result of {@link RealtimeEngine.bounceOffline}. */
export interface EngineBounceResult {
  /** One `Float32Array` per output channel, all the same length. */
  channels: Float32Array[];
  /** Sample rate of `channels` in Hz. */
  sampleRate: number;
  /** Output channel count, equal to `channels.length`. */
  numChannels: number;
  /** Integrated loudness of the bounce in LUFS. */
  integratedLufs: number;
}

/** Settings for {@link RealtimeEngine.freezeOffline}. */
export interface EngineFreezeOptions {
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  clipId?: number;
  startPpq?: number;
  gain?: number;
}

/** Result of {@link RealtimeEngine.freezeOffline}. */
export interface EngineFreezeResult {
  clipId: number;
  frames: number;
  numChannels: number;
}

/** Program and monitor output from {@link RealtimeEngine.processWithMonitor}. */
export interface EngineProcessWithMonitorResult {
  output: Float32Array[];
  monitor: Float32Array[];
}

/** A tempo-map segment. */
export interface EngineTempoSegment {
  startPpq: number;
  bpm: number;
  endBpm?: number;
}

/** A time-signature-map segment. */
export interface EngineTimeSignatureSegment {
  startPpq: number;
  numerator: number;
  denominator: number;
}

/** A record drained from {@link RealtimeEngine.drainTelemetry}. */
export interface EngineTelemetry {
  type: EngineTelemetryType;
  error: EngineTelemetryError;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  /** Processing latency in samples, fractional. */
  graphLatencySamples: number;
  value: number;
}

/**
 * Meter telemetry target ids: `0` is the master mix, `1..32` the track lanes
 * (lane index + 1), `33..40` the buses, and `0xFFFF` the input-monitor tap.
 */
export interface EngineMeterTelemetry {
  targetId: number;
  renderFrame: number;
  seq: number;
  peakDbL: number;
  peakDbR: number;
  rmsDbL: number;
  rmsDbR: number;
  /** Inter-sample peak in dB (4x polyphase, one filter group delay behind the block). */
  truePeakDbtpL: number;
  truePeakDbtpR: number;
  maxTruePeakDbtp: number;
  correlation: number;
  monoCompatWidth: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /** Maximum pre-trim input peak in dBFS. */
  inputPeakDbL: number;
  inputPeakDbR: number;
  droppedRecords: number;
}

/** Per-plane meter telemetry; `targetId` follows {@link EngineMeterTelemetry}. */
export interface EngineMeterTelemetryWide {
  targetId: number;
  renderFrame: number;
  seq: number;
  numChannels: number;
  peakDb: number[];
  rmsDb: number[];
  /** Per-plane inter-sample peak in dB, as {@link EngineMeterTelemetry.truePeakDbtpL}. */
  truePeakDbtp: number[];
  maxTruePeakDbtp: number;
  correlation: number;
  monoCompatWidth: number;
  momentaryLufs: number;
  shortTermLufs: number;
  integratedLufs: number;
  gainReductionDb: number;
  /** Pre-trim input peak per valid plane, in dBFS. */
  inputPeakDb: number[];
  droppedRecords: number;
}

/** Spectrum and vectorscope snapshot; `targetId` follows {@link EngineMeterTelemetry}. */
export interface EngineScopeTelemetry {
  targetId: number;
  renderFrame: number;
  seq: number;
  droppedRecords: number;
  bands: number[];
  points: { left: number; right: number }[];
}

export interface EngineTrackSend {
  busId: number;
  levelDb?: number;
  enabled?: boolean;
  /** Pre/post-fader tap point. Default `'postFader'`. */
  sendTiming?: SendTiming;
}

export interface EngineTrackLane {
  trackId: number;
  sends?: EngineTrackSend[];
  /** Bus the lane's post-fader output sums into; omit for the master mix. `0` is refused. */
  outputBusId?: number;
  /** Layout of the source feeding this lane. Default `'stereo'`; any other layout throws until multichannel lanes exist. */
  sourceChannelLayout?: ChannelLayout;
}

/** Per-track cue/monitor tap mode: off, pre-fader listen, or after-fader listen. */
export type EngineTrackMonitorMode = 'off' | 'pfl' | 'afl';

export interface EngineBus {
  busId: number;
  gainDb?: number;
  /**
   * Layout of the bus. A surround layout makes it a surround group bus: lanes
   * routed to it are surround-panned and it sums into the master plane by plane.
   * Default `'stereo'`.
   */
  channelLayout?: ChannelLayout;
  /** Bus this bus's output sums into; omit for the master mix. `0` is refused. */
  outputBusId?: number;
  /** Sends to other buses, in the same shape as a track lane's sends. */
  sends?: EngineTrackSend[];
}

/** A sidechain key source for an insert. */
export interface SidechainSource {
  kind: SidechainSourceKind;
  id: number;
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

/** Request of {@link RealtimeEngine.renderOffline}. */
export interface RenderOfflineRequest {
  /** One buffer per output plane; their common length is the render span. */
  channels: Float32Array[];
  /** Render block size. Default `128`. */
  blockSize?: number;
  /**
   * Whether this call ends the timeline. `true` (the default) releases every
   * sounding note and flushes the PDC / alignment delay lines before returning.
   * `false` renders one chunk of a longer timeline: a held note keeps sounding
   * into the next call and the delay lines carry over, so consecutive chunks
   * concatenate to what one continuous render produces. Call
   * {@link RealtimeEngine.finishOfflineRender} once after the last chunk.
   *
   * Bit-exact concatenation needs the same `blockSize` and a whole number of
   * blocks per chunk; otherwise audio stays continuous and only bit-identity is lost.
   */
  finalize?: boolean;
  /** Set `false` to skip the sample scan for NaN and infinities. Default `true`. */
  validate?: boolean;
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

/** One raw UMP message: 1 to 4 words, most significant word first. */
export type UmpWords = Uint32Array | readonly number[];

/**
 * Why a sidechain binding would be refused; the camelCase form of the C ABI
 * `SonareSidechainRefusal`. When several reasons hold, the first check the
 * matching setter runs is reported.
 */
export type SidechainRefusal =
  | 'invalidTarget'
  | 'insertOutOfRange'
  | 'undeclaredSource'
  | 'invalidSourceKind'
  | 'selfKey'
  | 'cycle'
  | 'tableFull'
  | 'planRefused';

/** Result of a `canSet*Sidechain` query; `reason` is absent exactly when `ok`. */
export interface SidechainCheck {
  ok: boolean;
  reason?: SidechainRefusal;
}

/**
 * MIDI-FX chain configuration. Every field is optional; a stage runs only when
 * one of its fields is present, and the stages run in the order listed here.
 * A value the core refuses (out of range, non-integer where an integer is
 * needed) is refused when the config is applied.
 */
export interface MidiFxConfig {
  /** Transpose every note by this many semitones (integer). Present enables the stage. */
  transposeSemitones?: number;
  /** Velocity multiplier. Default `1`. Any of the three velocity fields enables the stage. */
  velocityScale?: number;
  /** Velocity offset added after scaling. Default `0`. */
  velocityOffset?: number;
  /** Velocity curve exponent, greater than `0`. Default `1`. */
  velocityGamma?: number;
  /** Quantize grid in PPQ, greater than `0`. Present enables the stage. */
  quantizePpq?: number;
  /** Quantize strength, `0` (none) to `1` (snap fully). Default `1`. */
  quantizeStrength?: number;
  /** Chord intervals in semitones above each note: 1 to 8 integers. Present enables the stage. */
  chordIntervals?: number[];
  /** Arpeggiator step intervals in semitones: 1 to 16 integers. Present enables the stage; needs `arpeggiatorStepPpq`. */
  arpeggiatorIntervals?: number[];
  /** Arpeggiator step length in PPQ, greater than `0`. Required with `arpeggiatorIntervals`. */
  arpeggiatorStepPpq?: number;
  /** Arpeggiator gate length in PPQ, greater than `0`, capped at the step length. Default: the step length. */
  arpeggiatorGatePpq?: number;
  /** Timing jitter in PPQ, `0` or more. Default `0`. Any humanize field (or `seed`) enables the stage. */
  humanizePpq?: number;
  /** Velocity jitter, integer `0` to `127`. Default `0`. */
  humanizeVelocity?: number;
  /** Random seed of the humanize stage, integer `0` or more. Default `0`. */
  seed?: number;
}
