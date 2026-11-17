import type { TimeSignature } from './types_analysis.js';
import type { AutomationCurve, SendTiming } from './types_mixing.js';
import type { ProjectWarpAnchor } from './types_project.js';

/** Kind of a telemetry record: a processed block or an engine error. */
export type EngineTelemetryType = 'processBlock' | 'error';

/** One raw UMP message of 1 to 4 words, most significant word first. */
export type UmpWords = Uint32Array | readonly number[];

/** Per-track cue/monitor tap mode: off, pre-fader listen, or after-fader listen. */
export type EngineTrackMonitorMode = 'off' | 'pfl' | 'afl';

/** Reason an engine error telemetry record was published; `'none'` on a processed-block record. */
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

/** Telemetry record drained from {@link RealtimeEngine.drainTelemetry}. */
export interface EngineTelemetry {
  type: EngineTelemetryType;
  error: EngineTelemetryError;
  renderFrame: number;
  timelineSample: number;
  audibleTimelineSample: number;
  /** Processing latency of the engine in samples (fractional). */
  graphLatencySamples: number;
  value: number;
}

/** Meter telemetry record drained from {@link RealtimeEngine.drainMeterTelemetry}. */
export interface EngineMeterTelemetry {
  /**
   * Telemetry tap target id: `0` master mix, `laneIndex + 1` (1..32) for track
   * lanes, `33 + busIndex` (33..40) for buses, and `0xFFFF` for the
   * input-monitor capture tap. Handle `0xFFFF` explicitly -- a naive
   * `targetId - 1` index into a 32-entry lane array runs past its end.
   */
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
  /**
   * Left-channel inter-sample (true) peak in dB, from the ITU-R BS.1770-4
   * polyphase reconstruction at 4x. A streaming measurement that runs one
   * reconstruction-filter group delay (a few samples) behind the block, so every
   * value has its whole stencil and the reading does not depend on the block
   * size. `meteringTruePeakDb` measures a whole signal at once.
   */
  truePeakDbL: number;
  /** Right-channel inter-sample (true) peak in dB. See {@link truePeakDbL}. */
  truePeakDbR: number;
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDbL}. */
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
  /** Left-channel peak at the engine input tap, in dB. */
  inputPeakDbL: number;
  /** Right-channel peak at the engine input tap, in dB. */
  inputPeakDbR: number;
}

/**
 * One lowered MIDI 1.0 message drained from
 * {@link RealtimeEngine.drainExternalMidi}. A single queued channel-voice event
 * may lower to more than one message (e.g. a MIDI 2.0 program change with bank
 * select), so the drain returns one entry per lowered message.
 */
export interface ExternalMidiEvent {
  /**
   * Originating track MIDI destination id, or the transport sentinel
   * `0xFFFFFFFF` (4294967295) for clock / start / continue / stop bytes meant
   * for every external port.
   */
  destinationId: number;
  /**
   * Absolute engine device render frame of the event, for every record
   * (channel-voice, live input and clock/transport bytes alike). It is not an
   * offset within a block: do not add a block or batch base to it.
   */
  renderFrame: number;
  /** MIDI 1.0 status + data bytes (1..3 entries). */
  bytes: number[];
}

/**
 * Per-plane meter telemetry record drained from
 * {@link RealtimeEngine.drainMeterTelemetryWide} for a surround target. The
 * `peakDb`/`rmsDb`/`truePeakDb` arrays carry `channelCount` planes in canonical
 * WAVE order (5.1 = L R C LFE Ls Rs, 7.1 = L R C LFE Ls Rs Lss Rss).
 */
export interface EngineMeterTelemetryWide {
  /**
   * Telemetry tap target id: `0` master mix, `laneIndex + 1` (1..32) for track
   * lanes, `33 + busIndex` (33..40) for buses, and `0xFFFF` for the
   * input-monitor capture tap. Handle `0xFFFF` explicitly -- a naive
   * `targetId - 1` index into a 32-entry lane array runs past its end.
   */
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
  /**
   * Per-plane inter-sample (true) peak in dB (length `channelCount`), from the
   * ITU-R BS.1770-4 polyphase reconstruction at 4x. A streaming measurement that
   * runs one reconstruction-filter group delay (a few samples) behind the block,
   * so the reading does not depend on the block size. `meteringTruePeakDb`
   * measures a whole signal at once.
   */
  truePeakDb: number[];
  /** Maximum inter-sample peak across channels in dB. See {@link truePeakDb}. */
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
  /** Per-plane peak at the engine input tap, in dB. */
  inputPeakDb: number[];
}

/** Scope telemetry record drained from {@link RealtimeEngine.drainScopeTelemetry}. */
export interface EngineScopeTelemetry {
  /**
   * Telemetry tap target id: `0` master mix, `laneIndex + 1` (1..32) for track
   * lanes, `33 + busIndex` (33..40) for buses, and `0xFFFF` for the
   * input-monitor capture tap. Handle `0xFFFF` explicitly -- a naive
   * `targetId - 1` index into a 32-entry lane array runs past its end.
   */
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

/** Read-only engine transport snapshot from {@link RealtimeEngine.transportState}. */
export interface EngineTransportState {
  /** Whether the transport is currently playing. */
  playing: boolean;
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
 * Parameter descriptor accepted by {@link RealtimeEngine.addParameter}. The
 * optional fields fall back to the documented default when omitted; the
 * `parameterInfo` getters always return a fully populated object.
 */
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

export interface EngineAutomationPoint {
  /** Position on the transport timeline, in pulses per quarter note. */
  ppq: number;
  /**
   * Parameter value at `ppq`. Required, and must be finite: the lane
   * interpolates between breakpoints, so a non-finite one poisons every
   * parameter the lane drives.
   */
  value: number;
  /** Curve to the next point. Default `'linear'`; a number or an unknown spelling is refused. */
  curveToNext?: AutomationCurve;
}

/** Structured marker kind; mirrors SonareMarkerKind / SmfMarkerKind. */
export type MarkerKind = 'marker' | 'text' | 'lyric' | 'cuePoint' | 'keySignature';

export interface EngineMarker {
  id: number;
  ppq: number;
  name?: string;
  /** Marker kind; defaults to `'marker'`. A number is refused. */
  kind?: MarkerKind;
  /** Key signature only: -7..7 (sharps positive). */
  keyFifths?: number;
  /** Key signature only: false = major, true = minor. */
  keyMinor?: boolean;
}

/**
 * Metronome configuration. Give at most one of `clickSamples` and `clickSec`;
 * both absent selects the sample-rate-derived click length. An explicit `0` for
 * either is a `RangeError`. {@link RealtimeEngine.metronome} reports only the
 * spelling that is in effect.
 */
export interface EngineMetronomeConfig {
  enabled: boolean;
  beatGain?: number;
  accentGain?: number;
  /** Explicit click length in samples (1..384000). */
  clickSamples?: number;
  /** Click duration in seconds (0..1]. */
  clickSec?: number;
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
  /** Warp mode; absent is `'off'`. A number is refused. */
  warpMode?: 'off' | 'repitch' | 'tempoSync' | 'timeStretch';
  warpAnchors?: ProjectWarpAnchor[];
  pageProvider?: number | { readonly id: number };
}

/**
 * Speaker bed layout for a bus or source, mirroring the C enum
 * `SonareChannelLayout`: `'mono'`, `'stereo'`, `'5.1'` (L R C LFE Ls Rs) and
 * `'7.1'` (L R C LFE Ls Rs Lss Rss).
 */
export type ChannelLayout = 'mono' | 'stereo' | '5.1' | '7.1';

export interface EngineTrackLane {
  trackId: number;
  sends?: EngineTrackSend[];
  /**
   * Bus the lane's post-fader output sums into instead of the master mix
   * (group/folder routing); absent keeps the lane on the master mix. An explicit
   * `0` is a `RangeError`.
   */
  outputBusId?: number;
  /**
   * Input channel layout of the source feeding this lane. Absent defaults to
   * stereo. Only `'stereo'` is accepted; any other layout throws until
   * multichannel lanes are implemented.
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
  sendTiming?: SendTiming;
}

export interface EngineBus {
  busId: number;
  gainDb?: number;
  /**
   * Channel layout of this bus. Absent defaults to stereo. The master bus
   * carries the project output layout. A surround layout (5.1/7.1) makes this a
   * surround group bus: lanes routed to it are surround-panned and its insert
   * chain runs at the bus width.
   */
  channelLayout?: ChannelLayout;
  /**
   * Bus this bus's output sums into instead of the master mix (bus-to-bus
   * routing); absent keeps the bus on the master mix and an explicit `0` is a
   * `RangeError`. An output naming an
   * undeclared bus, itself, or forming a cycle (through outputs, sends, or
   * bus-sourced sidechain keys) is rejected.
   */
  outputBusId?: number;
  /**
   * Sends to other buses, in the same shape as a track lane's sends. Pre-fader
   * sends tap before {@link gainDb}; post-fader after.
   */
  sends?: EngineTrackSend[];
}

/**
 * Source a bus or master insert's sidechain key is taken from, mirroring
 * `SonareSidechainSourceKind`: a track lane's post-strip signal (before its
 * lane fader), or a bus's processed signal (before its `gainDb`, folded to
 * stereo when wider).
 */
export type SidechainSourceKind = 'track' | 'bus';

/** A sidechain key source: a track lane or a bus, by id (`id` is never 0; pass `null` to remove a binding). */
export interface SidechainSource {
  kind: SidechainSourceKind;
  id: number;
}

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

/** Result of a `canSet*Sidechain` query; `reason` is `null` exactly when `ok`. */
export interface SidechainCheck {
  ok: boolean;
  reason: SidechainRefusal | null;
}

/** Layout of the file read by a {@link FileClipPageProvider}. */
export interface FileClipPageProviderOptions {
  /** Channel count of the interleaved data; must be positive. */
  numChannels: number;
  /** Length of the clip in frames; must be positive. */
  numSamples: number;
  /** Frames per page; must be positive. */
  pageFrames: number;
  /** Byte offset of the first frame in the file. Default `0`. */
  dataOffsetBytes?: number;
}

/**
 * Punch-in window of {@link CaptureOptions}, half-open `[start, end)` on the
 * timeline. Give exactly one start spelling and exactly one end spelling; each
 * is rounded to the nearest sample at the engine's rate.
 */
export interface CapturePunch {
  /** Start in quarter notes. */
  startPpq?: number;
  /** Start in timeline samples. */
  startSample?: number;
  /** Start in timeline seconds. */
  startSec?: number;
  /** End in quarter notes. */
  endPpq?: number;
  /** End in timeline samples. */
  endSample?: number;
  /** End in timeline seconds. */
  endSec?: number;
}

/**
 * Input-monitor setting of {@link CaptureOptions}.
 */
export interface CaptureInputMonitor {
  enabled: boolean;
  /** Linear monitor gain. Default `1`. */
  gain?: number;
}

/**
 * Capture setup for {@link RealtimeEngine.configureCapture}. The set is applied
 * as a whole: an absent `source` is `'output'`, an absent `recordOffsetSamples`
 * is `0`, an absent `inputMonitor` is enabled at gain `1` (the engine default) and an absent `punch` is no punch.
 */
export interface CaptureOptions {
  /** Capture length in frames per channel. Required, positive. */
  capacityFrames: number;
  /** Channels captured. Default `2`. */
  numChannels?: number;
  /** What is recorded: the engine output or the live input. Default `'output'`. */
  source?: EngineCaptureSource;
  /** Delay of the capture relative to the punch window, in samples. Default `0`. */
  recordOffsetSamples?: number;
  inputMonitor?: CaptureInputMonitor;
  punch?: CapturePunch;
}

export interface EngineCaptureStatus {
  capturedFrames: number;
  overflowCount: number;
  armed: boolean;
  punchEnabled: boolean;
  source: 'output' | 'input';
  recordOffsetSamples: number;
}

export type EngineCaptureSource = EngineCaptureStatus['source'];

export interface EngineBounceOptions {
  /**
   * Length of the bounce in frames. The whole result is held in memory, so the
   * span is capped by a 1 GiB peak budget over the full-size float buffers a
   * bounce holds at once, and exceeding it throws a `RangeError`.
   *
   * The cap is a **frame** count: inversely proportional to `numChannels`, and
   * independent of the sample rate. Stereo refuses past 44,739,242 frames, or
   * past 33,554,432 frames when `dither` is set, which adds one more copy; 5.1
   * refuses past a third of each. Read as a **duration** those two stereo counts
   * are 15 min 32 s and 11 min 39 s at 48 kHz, and half that at 96 kHz. When
   * `sourceSampleRate` and `targetSampleRate` differ both the source and the
   * resampled length are checked, so the longer of the two is what binds.
   *
   * Render longer material in chunks via `renderOffline({ finalize: false })`.
   */
  totalFrames: number;
  blockSize?: number;
  numChannels?: number;
  /**
   * Output sample rate. Omitting it keeps the source rate, so nothing is
   * resampled; a different rate resamples. An explicit `0` is a `RangeError`.
   * The result's `sampleRate` reports the rate of the returned audio.
   */
  targetSampleRate?: number;
  /**
   * Rate `totalFrames` is counted in. Omitting it means the engine's prepared
   * rate; a different value throws a `RangeError`, because the render always
   * runs at the prepared rate, and so does an explicit `0`.
   */
  sourceSampleRate?: number;
  normalizeLufs?: boolean;
  /**
   * Target integrated loudness in LUFS when `normalizeLufs` is set. Omitting it
   * means -14 LUFS. `0` is a literal (if extreme) target and a non-finite value
   * is a `RangeError`.
   */
  targetLufs?: number;
  /** Dither applied when reducing bit depth; absent is none. A number is refused. */
  dither?: EngineDither;
  ditherBits?: number;
  ditherSeed?: number;
}

/** Dither shape of {@link EngineBounceOptions.dither}. */
export type EngineDither = 'rpdf' | 'tpdf' | 'noiseShaped';

/** Planar audio rendered by {@link RealtimeEngine.bounceOffline}. */
export interface EngineBounceResult {
  /** One plane per output channel, all the same length. */
  readonly channels: Float32Array[];
  /** Number of output channels; the length of {@link channels}. */
  readonly numChannels: number;
  /** Sample rate of the returned audio in Hz. */
  readonly sampleRate: number;
  /** Integrated loudness of the returned audio, in LUFS. */
  readonly integratedLufs: number;
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

/** Processing of a graph node: pass the signal through, or apply `gainDb`. */
export type EngineGraphNodeType = 'passThrough' | 'gain';

/**
 * Mixing intent for a graph edge: `'replace'` replaces what the destination port
 * already holds, `'add'` adds to it. Omitted means add; any other value is refused.
 */
export type EngineGraphMix = 'replace' | 'add';

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

/** One absolute render-frame MIDI event accepted by {@link RealtimeEngine.setMidiClips}. */
export interface EngineMidiEvent {
  /** Absolute render frame for this event. Default `0`. */
  renderFrame?: number;
  /** First UMP word. `data0` is accepted by native wrappers as an alias. */
  word0?: number;
  /** Second UMP word. `data1` is accepted by native wrappers as an alias. */
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
  /**
   * Linear gain applied to the destination instrument's rendered audio while
   * this clip is the most recently started active clip on it. Absent defaults
   * to `1` (unity); a zero-initialized C struct is silent, so this facade's
   * default deliberately differs from the ABI's own zero-init default.
   */
  gain?: number;
  /**
   * Linear fade lengths in samples over the clip's full length (not per
   * internal loop repeat). Absent defaults to `0` (no fade).
   * `fadeOutSamples` must be `0` when `lengthSamples` is `0` (open-ended).
   */
  fadeInSamples?: number;
  fadeOutSamples?: number;
}

/**
 * Request of {@link RealtimeEngine.renderOffline}. The engine renders at its own
 * sample rate, so the request carries none.
 */
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
  /**
   * Whether `channels` is scanned for NaN and Inf before rendering (a `RangeError`
   * on a hit). Default `true`; `false` skips the O(n) scan.
   */
  validate?: boolean;
}

/**
 * Construction and preparation settings of {@link RealtimeEngine}. An unknown key
 * is a `TypeError`; an explicit `0` on an optional field is a `RangeError`
 * (omit it for the default).
 */
export interface RealtimeEngineConfig {
  /** Engine sample rate in Hz. Required. */
  sampleRate: number;
  /** Largest block the engine renders, in frames. Default `128`. */
  maxBlockSize?: number;
  /** Command ring capacity, at most 65536. Default `1024`. */
  commandCapacity?: number;
  /**
   * Telemetry ring capacity, at most 16384. Default `1024`. The engine reserves
   * that many meter records per metered lane, so the memory cost is far larger
   * than the number given here.
   */
  telemetryCapacity?: number;
  /** Most channels the engine renders. Default `64`. */
  maxNumChannels?: number;
}

/** Loop region of {@link RealtimeEngine.setLoop}, in quarter notes. */
export interface EngineLoop {
  startPpq: number;
  endPpq: number;
  /** Whether looping is active. Default `true`. */
  enabled?: boolean;
}

/** One tempo ramp segment of {@link RealtimeEngine.setTempoSegments}. */
export interface EngineTempoSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Tempo in BPM at the segment start. */
  bpm: number;
  /** Tempo in BPM at the segment end for a ramp; `0` / omitted = constant tempo. */
  endBpm?: number;
}

/** One time-signature segment of {@link RealtimeEngine.setTimeSignatureSegments}. */
export interface EngineTimeSignatureSegment {
  /** Segment start position in PPQ. */
  startPpq: number;
  /** Beats per bar. */
  numerator: number;
  /** Beat unit (e.g. `4` for quarter note). */
  denominator: number;
}

/**
 * Typed MIDI-FX chain configuration. Every field is optional; a stage runs only
 * when one of its fields is given, and stages apply in the order listed here.
 */
export interface MidiFxConfig {
  /** Transpose every note by this many semitones (integer). Giving it enables the transpose stage. */
  transposeSemitones?: number;
  /** Velocity multiplier. Default `1`. Any velocity field enables the velocity-curve stage. */
  velocityScale?: number;
  /** Velocity offset added after scaling. Default `0`. */
  velocityOffset?: number;
  /** Velocity gamma, `> 0`. Default `1`. */
  velocityGamma?: number;
  /** Quantize grid in quarter notes, `> 0`. Giving it enables the quantize stage. */
  quantizePpq?: number;
  /** Quantize strength, `0`..`1`. Default `1`. */
  quantizeStrength?: number;
  /** Semitone intervals stacked on each note, 1 to 8 integers. Giving it enables the chord stage. */
  chordIntervals?: number[];
  /** Semitone intervals of the arpeggio steps, 1 to 16 integers. Giving it enables the arpeggiator and requires `arpeggiatorStepPpq`. */
  arpeggiatorIntervals?: number[];
  /** Length of one arpeggio step in quarter notes, `> 0`. */
  arpeggiatorStepPpq?: number;
  /** Sounding length of one arpeggio step in quarter notes, `> 0`, capped at the step. Default: the step length. */
  arpeggiatorGatePpq?: number;
  /** Timing jitter in quarter notes, `>= 0`. Default `0`. Any humanize field enables the humanize stage. */
  humanizePpq?: number;
  /** Velocity jitter, integer `0`..`127`. Default `0`. */
  humanizeVelocity?: number;
  /** Humanize random seed, integer `>= 0`. Default `0`. */
  seed?: number;
}
