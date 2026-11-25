import type { ChannelLayout } from './audio.js';
import type { ProjectWarpMode } from './project_types.js';
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
  maxChannels?: number;
}

/**
 * A strip address: a number is a track id, `'master'` is the master strip. Any
 * other string is refused with a `RangeError`, digit-only strings included.
 */
export type StripRef = number | string;

/** Marker kind spellings. */
export type MarkerKind = 'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature';

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
  /** Clip identifier (unsigned 32-bit). */
  id?: number;
  /** Track lane the clip belongs to (unsigned 32-bit). Omitted: the clip is not tied to a track lane. */
  trackId?: number;
  /** Source audio, one plane per channel, all the same length. Exactly one of `channels` and `pageProvider`. */
  channels?: Float32Array[];
  /** Timeline position of the clip start, in quarter notes, in [0, 1e12]. */
  startPpq: number;
  /** Playback length in frames; must be positive. Omitted: plays to the end of the source (source length minus `clipOffsetSamples`). */
  lengthSamples?: number;
  /** Offset into the source, in frames, where playback starts. Default `0`. */
  clipOffsetSamples?: number;
  /** Whether the clip repeats over its playback length. Default `false`. */
  loop?: boolean;
  /** Linear clip gain. Default `1`. */
  gain?: number;
  /** Fade-in length in frames. Default `0`. */
  fadeInSamples?: number;
  /** Fade-out length in frames. Default `0`. */
  fadeOutSamples?: number;
  /** Default `'off'`. */
  warpMode?: ProjectWarpMode;
  /** Anchor map between warped and source positions (both in source frames). Used when `warpMode` is not `'off'`. */
  warpAnchors?: Array<{
    /** Position in the warped (timeline) clip, in frames. */
    warpSample: number;
    /** Matching position in the source audio, in frames. */
    sourceSample: number;
  }>;
  /** Paged source supplying the audio on demand. Exactly one of `channels` and `pageProvider`. */
  pageProvider?: ClipPageProvider;
}

/** A parameter registered with {@link RealtimeEngine.addParameter}. */
export interface EngineParameterInfo {
  /** Parameter id, unique within the engine. */
  id: number;
  /** Display name. */
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
  /** Position on the transport timeline, in pulses per quarter note, in [0, 1e12]. */
  ppq: number;
  /**
   * Parameter value at `ppq`. Required, and must be finite: the lane
   * interpolates between breakpoints, so a non-finite one poisons every
   * parameter the lane drives.
   */
  value: number;
  /** Curve towards the next point. Default `'linear'`. */
  curveToNext?: AutomationCurve;
}

/** A timeline marker. */
export interface EngineMarker {
  /** Marker id, unique within the engine. */
  id: number;
  /** Position on the transport timeline, in pulses per quarter note, in [0, 1e12]. */
  ppq: number;
  /** Display name. Omitted: unnamed (read back without the key); an empty string is refused. */
  name?: string;
  /** Default `'marker'`. */
  kind?: MarkerKind;
  /** Key-signature field: -7..7, sharps positive; other kinds require `0`. */
  keyFifths?: number;
  /** Key-signature field: `true` for minor; other kinds require `false`. */
  keyMinor?: boolean;
}

/** An entry of `setMarkers`: an {@link EngineMarker} whose `id` may be left out. */
export interface EngineMarkerInput extends Omit<EngineMarker, 'id'> {
  /** Id of the marker, positive and unique within the list. Omit it to have one assigned that no stored or listed marker carries. */
  id?: number;
}

/** Solo/mute state of a track, as taken by `setTrackSoloMute`. */
export interface TrackSoloMuteState {
  /** Whether the track is soloed. */
  solo: boolean;
  /** Whether the track is muted. */
  mute: boolean;
}

/** Insert bypass state, as taken by `setStripInsertBypassed` and `setBusInsertBypassed`. */
export interface InsertBypassState {
  /** Whether the insert is bypassed. */
  bypassed: boolean;
  /** Whether the insert's internal state is reset on bypass. Default `false`. */
  resetOnBypass?: boolean;
}

/** One insert parameter change, addressed by the processor's JSON-key parameter name. */
export interface InsertParamValue {
  /** Parameter name, one of the entries `masteringInsertParamInfo` reports. */
  name: string;
  /** New parameter value. */
  value: number;
}

/** Capture arm state, as taken by `armCapture`. */
export interface CaptureArmState {
  /** `true` arms capture, `false` disarms it. */
  armed: boolean;
}

/**
 * Metronome settings. The click length is `clickSamples` or `clickSec`, never
 * both; an explicit `0` is refused, so omit it for the 2 ms default.
 */
export interface EngineMetronomeConfig {
  /** Whether the click is audible. */
  enabled: boolean;
  /** Linear gain of a regular beat click. Omitted: 0.35. */
  beatGain?: number;
  /** Linear gain of the downbeat (accent) click. Omitted: 0.7. */
  accentGain?: number;
  /** Click length in samples (1..384000), capped to one second at the engine rate. */
  clickSamples?: number;
  /** Click length in seconds, in (0, 1]. */
  clickSec?: number;
}

/**
 * The dry input monitor as {@link RealtimeEngine.inputMonitor} reads it: the live
 * input passed straight to the output ahead of the master, independent of the
 * input track.
 */
export interface EngineInputMonitor {
  /** Whether the live input is monitored through the engine output. */
  readonly enabled: boolean;
  /** Linear monitor gain; a negative gain inverts the polarity. */
  readonly gain: number;
}

/** The dry input monitor as {@link RealtimeEngine.setInputMonitor} takes it. */
export interface EngineInputMonitorInput {
  /** Whether the live input is monitored through the engine output. */
  enabled: boolean;
  /** Linear monitor gain, finite. Default `1`. */
  gain?: number;
}

/** A node of an {@link EngineGraphSpec}. */
export interface EngineGraphNode {
  /** Node name, unique within the graph (at most 63 characters). */
  id: string;
  /** Default `'passThrough'`. */
  type?: EngineGraphNodeType;
  /** Gain in dB applied when `type` is `'gain'`. Default `0`. */
  gainDb?: number;
  /** Port count of the node. Omitted: the spec's `numChannels`; `0` and a negative or fractional value are refused. */
  numPorts?: number;
}

/** A connection between two graph node ports. */
export interface EngineGraphConnection {
  /** `id` of the node the signal comes from. */
  sourceNode: string;
  /** Zero-based output port of `sourceNode`. */
  sourcePort: number;
  /** `id` of the node the signal goes to. */
  destNode: string;
  /** Zero-based input port of `destNode`. */
  destPort: number;
  /** How the edge meets others into the same destination port. Default `'add'`. */
  mix?: EngineGraphMix;
}

/** Binds an automation parameter id to a graph node. */
export interface EngineGraphParameterBinding {
  /** Id of the registered automation parameter. */
  paramId: number;
  /** `id` of the graph node the parameter drives. */
  nodeId: string;
}

/** The processing graph installed by {@link RealtimeEngine.setGraph}. */
export interface EngineGraphSpec {
  /** Nodes of the graph. */
  nodes: EngineGraphNode[];
  /** Edges between node ports. */
  connections: EngineGraphConnection[];
  /** `id` of the node that receives the engine input. */
  inputNode: string;
  /** `id` of the node whose output is the graph result. */
  outputNode: string;
  /** Channel count the graph runs at. Default `2`; `0` is refused. */
  numChannels?: number;
  /** Automation parameters bound to nodes. */
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
  /** Punch-in window. Omitted: no punch window, capture is not gated. */
  punch?: CapturePunch;
}

/** Capture state read by {@link RealtimeEngine.captureStatus}. */
export interface EngineCaptureStatus {
  /** Frames per channel captured so far. */
  capturedFrames: number;
  /** Frames discarded while the capture buffer was full since the last reset. A 64-bit count, exact up to `Number.MAX_SAFE_INTEGER`. */
  droppedFrames: number;
  /** Whether capture is armed. */
  armed: boolean;
  /** Whether a punch window is configured. */
  punchEnabled: boolean;
  /** What is recorded: the engine output or the live input. */
  source: EngineCaptureSource;
  /** Delay of the capture relative to the punch window, in samples. */
  recordOffsetSamples: number;
}

/** Loop region of {@link RealtimeEngine.setLoop}. */
export interface EngineLoop {
  /** Loop start in quarter notes, in [0, 1e12]. */
  startPpq: number;
  /** Loop end in quarter notes, in [0, 1e12]. */
  endPpq: number;
  /** Whether looping is on. Default `true`. */
  enabled?: boolean;
}

/** Transport snapshot read by {@link RealtimeEngine.transportState}. */
export interface EngineTransportState {
  /** Whether the transport is playing. */
  playing: boolean;
  /** Whether looping is enabled. */
  looping: boolean;
  /** Current render-frame counter. */
  renderFrame: number;
  /** Current timeline position in samples. */
  samplePosition: number;
  /** Current position in pulses per quarter note. */
  ppq: number;
  /** Current tempo in beats per minute. */
  bpm: number;
  /** Loop start in pulses per quarter note. */
  loopStartPpq: number;
  /** Loop end in pulses per quarter note. */
  loopEndPpq: number;
  /** Engine sample rate in Hz. */
  sampleRate: number;
  /** Quarter note of the current bar's downbeat. */
  barStartPpq: number;
  /** Zero-based index of the current bar. */
  barIndex: number;
  /** Time signature in effect at the current position. */
  timeSignature: TimeSignature;
  /** One-based beat within the current bar. */
  beat: number;
  /** Position within the current beat, in [0, 1). */
  beatFraction: number;
}

/** Options for {@link RealtimeEngine.setParameter}. */
export interface EngineSetParameterOptions {
  /**
   * Ramp to the value over the engine smoothing time, from the value the target holds, instead
   * of stepping. Default `false`. A
   * reserved target (fader, pan, width, insert or instrument parameter) de-zippers every change
   * over its fixed 5 ms either way, and under a ramp the two cascade.
   */
  smoothed?: boolean;
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
  /** Frames per render block. Omit it for the engine's default block (128, capped at the prepared maximum); an explicit `0` is a `RangeError`. */
  blockSize?: number;
  /** Channel count of the rendered result. Required. */
  numChannels: number;
  /** Output sample rate. Omit to keep the source rate; `0` is refused. */
  targetSampleRate?: number;
  /** Rate `totalFrames` is counted in. Omit for the engine rate; `0` is refused, and any other rate throws. */
  sourceSampleRate?: number;
  /** Normalize the result to `targetLufs`. Default `false`. */
  normalizeLufs?: boolean;
  /** Integrated loudness target in LUFS. Omit for -14; any finite value, `0` included, is literal and a non-finite one is refused. */
  targetLufs?: number;
  /** Omit for no dither. */
  dither?: EngineDither;
  /** Target bit depth the dither is shaped for. Default `16`. */
  ditherBits?: number;
  /** Seed of the dither noise (unsigned 32-bit). Omitted: the library's default seed. */
  ditherSeed?: number;
}

/** Planar result of {@link RealtimeEngine.bounceOffline}. */
export interface EngineBounceResult {
  /** One `Float32Array` per output channel, all the same length. */
  readonly channels: Float32Array[];
  /** Sample rate of `channels` in Hz. */
  readonly sampleRate: number;
  /**
   * Integrated loudness of the bounce in LUFS.
   * Never absent: the integrated measurement clamps to the input, so silence reads `-Infinity`.
   */
  readonly integratedLufs: number;
  /**
   * Samples whose value before quantization and dither lies past full scale
   * (|x| > 1), summed over the channels; 0 when none clipped. Absent when the bounce
   * did not dither (`dither` omitted).
   */
  quantizeClippedSamples?: number;
}

/** Settings for {@link RealtimeEngine.freezeOffline}. */
export interface EngineFreezeOptions {
  /** Length of the frozen render in frames. */
  totalFrames: number;
  /** Frames per render block. Omit it for the engine's default block (128, capped at the prepared maximum); an explicit `0` is a `RangeError`. */
  blockSize?: number;
  /** Channel count of the rendered result. Required. */
  numChannels: number;
  /** Id given to the clip created from the render (unsigned 32-bit). Default `1`. */
  clipId?: number;
  /** Timeline position of the created clip, in quarter notes, in [0, 1e12]. Default `0`. */
  startPpq?: number;
  /** Linear gain applied to the render. Default `1`. */
  gain?: number;
}

/** Result of {@link RealtimeEngine.freezeOffline}. */
export interface EngineFreezeResult {
  /** Id of the clip created from the render. */
  clipId: number;
  /** Length of the frozen clip in frames. */
  frames: number;
  /** Channel count of the frozen clip. */
  numChannels: number;
}

/** Options of {@link RealtimeEngine.process} and {@link RealtimeEngine.processPrepared}. */
export interface EngineProcessOptions {
  /** Keep the monitor (cue/listen) bus apart from the program mix. Omit for `false`. */
  withMonitor?: boolean;
}

/** Program and monitor output from {@link RealtimeEngine.process} with `withMonitor: true`. */
export interface EngineProcessWithMonitorResult {
  /** Program (main mix) output, one buffer per plane. */
  output: Float32Array[];
  /** Monitor (cue/listen) output, one buffer per plane. */
  monitor: Float32Array[];
}

/** A tempo-map segment. */
export interface EngineTempoSegment {
  /** Segment start in pulses per quarter note, in [0, 1e12]. */
  startPpq: number;
  /** Tempo at the segment start, in beats per minute, in [1e-6, 100000]. */
  bpm: number;
  /**
   * Tempo at the segment end for a ramp, in beats per minute, in [1e-6, 100000]
   * when nonzero. Omitted: constant tempo at `bpm`.
   */
  endBpm?: number;
}

/** A time-signature-map segment. */
export interface EngineTimeSignatureSegment {
  /** Segment start in pulses per quarter note, in [0, 1e12]. */
  startPpq: number;
  /** Beats per bar. */
  numerator: number;
  /** Beat unit (note value, e.g. 4 for a quarter note). */
  denominator: number;
}

/** A record drained from {@link RealtimeEngine.drainTelemetry}. */
export interface EngineTelemetry {
  /** Whether this is a processed-block record or an error record. */
  type: EngineTelemetryType;
  /** Error reason; `'none'` on a processed-block record. */
  error: EngineTelemetryError;
  /** Device render-frame counter at publication. */
  renderFrame: number;
  /** Engine timeline position in samples, before output-latency compensation. */
  timelineSample: number;
  /** Timeline position in samples that is audible at the output, after latency compensation. */
  audibleTimelineSample: number;
  /** Processing latency in samples, fractional. */
  graphLatencySamples: number;
  /** Error-specific payload (an id or an occurrence count, per `error`); `0` otherwise. */
  value: number;
}

/**
 * Per-plane meter telemetry. A stereo target's left and right are planes 0 and 1; a surround
 * target carries every plane. Target ids: `0` is the master mix, `1..32` the track lanes
 * (lane index + 1), `33..40` the buses, and `0xFFFF` the input-monitor tap.
 */
export interface EngineMeterTelemetry {
  /** Tap target id: `0` master, `1..32` track lanes, `33..40` buses, `0xFFFF` input-monitor tap (not a lane index + 1). */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Number of valid per-plane meters (1..8). */
  numChannels: number;
  /** Per-plane peak level in dB (length `numChannels`). `-Infinity` for an all-zero plane; `undefined` when not measured. */
  peakDb: (number | undefined)[];
  /** Per-plane RMS level in dB (length `numChannels`). `-Infinity` for an all-zero plane; `undefined` when not measured. */
  rmsDb: (number | undefined)[];
  /** Per-plane inter-sample peak in dB (4x polyphase, one filter group delay behind the block). `-Infinity` for an all-zero plane; `undefined` when not measured. */
  truePeakDbtp: (number | undefined)[];
  /** Maximum inter-sample peak across channels in dB. `-Infinity` for silence; absent when not measured. */
  maxTruePeakDbtp?: number;
  /** Stereo correlation in `[-1, 1]` (front pair). Absent when not measured (a channel has zero energy). */
  correlation?: number;
  /** Mono-compatibility width metric (front pair). Absent when not measured; `+Infinity` when the mid signal is silent but the side is not. */
  monoCompatWidth?: number;
  /** Momentary loudness in LUFS. `-Infinity` for silence; absent until the window has filled. */
  momentaryLufs?: number;
  /** Short-term loudness in LUFS. `-Infinity` for silence; absent until the window has filled. */
  shortTermLufs?: number;
  /** Integrated loudness in LUFS. Absent before the first gating block; `-Infinity` while nothing passes the gate. */
  integratedLufs?: number;
  /** Gain reduction in dB. */
  gainReductionDb: number;
  /** Pre-trim input peak per valid plane, in dBFS; `undefined` when not measured. */
  inputPeakDb: (number | undefined)[];
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
}

/** Spectrum and vectorscope snapshot; `targetId` follows {@link EngineMeterTelemetry}. */
export interface EngineScopeTelemetry {
  /** Telemetry tap target id; see {@link EngineMeterTelemetry}. `0xFFFF` is the input-monitor tap. */
  targetId: number;
  /** Render-frame timestamp of the snapshot. */
  renderFrame: number;
  /** Monotonic sequence number. */
  seq: number;
  /** Number of records dropped before this snapshot. */
  droppedRecords: number;
  /** FFT magnitude spectrum in dBFS, linear-spaced over `[0, Nyquist]`. `-Infinity` for a band of zero power, NaN for a band no bin reaches and before the first transform. */
  bands: number[];
  /** Goniometer/vectorscope sample points. */
  points: {
    /** Left-channel sample value. */
    left: number;
    /** Right-channel sample value. */
    right: number;
  }[];
}

/** A send from a track lane or bus to another bus. */
export interface EngineTrackSend {
  /** Destination bus id; the bus must be declared. */
  busId: number;
  /** Send level in dB. Omitted: 0 dB (unity). */
  levelDb?: number;
  /** Whether the send is active. Omitted: `true`. */
  enabled?: boolean;
  /** Pre/post-fader tap point. Default `'postFader'`. */
  sendTiming?: SendTiming;
}

/** Per-track lane routing: sends, output bus and source layout. */
export interface EngineTrackLane {
  /** Id of the track this lane carries. */
  trackId: number;
  /** Sends from this lane to buses. Omitted: none. */
  sends?: EngineTrackSend[];
  /** Bus the lane's post-fader output sums into; omit for the master mix. `0` is refused. */
  outputBusId?: number;
  /** Layout of the source feeding this lane. Default `'stereo'`; any other layout throws until multichannel lanes exist. */
  sourceChannelLayout?: ChannelLayout;
}

/** Per-track cue/monitor tap mode: off, pre-fader listen, or after-fader listen. */
export type EngineTrackMonitorMode = 'off' | 'pfl' | 'afl';

/** A mix bus declared on the engine. */
export interface EngineBus {
  /** Bus identifier; positive (`0` is not a bus id). */
  busId: number;
  /** Bus fader in dB. Default `0`. */
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
  /** Whether `id` names a track lane or a bus. */
  kind: SidechainSourceKind;
  /** Track id or bus id, per `kind`; never 0. */
  id: number;
}

/** One absolute render-frame MIDI event accepted by {@link RealtimeEngine.setMidiClips}. */
export interface EngineMidiEvent {
  /** Absolute render frame for this event. Default `0`. */
  renderFrame?: number;
  /** First UMP word; carries the message type and group. */
  word0?: number;
  /** Second UMP word. */
  word1?: number;
  /** Third UMP word. */
  word2?: number;
  /** Fourth UMP word. */
  word3?: number;
  /**
   * Number of UMP words in use. Zero or omission infers the count from the last
   * nonzero word. A fraction or a value outside [0, 4] raises `RangeError`
   * naming `clips[i].events[j].wordCount`.
   */
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
  /** Optional SysEx side-store handle for native hosts. */
  sysexHandle?: number;
}

/** One compiled realtime MIDI clip schedule accepted by {@link RealtimeEngine.setMidiClips}. */
export interface EngineMidiClipSchedule {
  /** Clip id. Omitted: 0. */
  id?: number;
  /** Id of the track the clip belongs to. Omitted: 0. */
  trackId?: number;
  /** MIDI destination id the clip's events are sent to. Omitted: destination 0. */
  destinationId?: number;
  /** Start position in timeline samples. */
  startSample?: number;
  /** Start position in pulses per quarter note, in [0, 1e12]. */
  startPpq?: number;
  /** Clip length in samples. Omitted: open-ended; `0` and a negative or fractional value are refused. */
  lengthSamples?: number;
  /** Whether the clip repeats. Omitted: `false`. */
  loop?: boolean;
  /** Positive loop period in samples, required when `loop` is true. Omit for a one-shot clip; a supplied 0 or nonzero inactive period is refused. */
  loopLengthSamples?: number;
  /** Events with absolute render frames. */
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
   * rejected when `lengthSamples` is absent (open-ended): an open-ended clip
   * has no end to fade out towards.
   */
  fadeInSamples?: number;
  /** Fade-out length in samples; see `fadeInSamples`. */
  fadeOutSamples?: number;
}

/** Request of {@link RealtimeEngine.renderOffline}. */
export interface RenderOfflineRequest {
  /** One buffer per output plane; their common length is the render span. */
  channels: Float32Array[];
  /** Render block size. Omit it for the engine's default block (128, capped at the prepared maximum); an explicit `0` is a `RangeError`. */
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
  /** `true` when the binding would be accepted. */
  ok: boolean;
  /** Why the binding would be refused; absent when `ok`. */
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
