import type { SendTiming } from './public_types.js';
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

/** Punch-in window for {@link RealtimeEngine.setCapturePunch}; each bound is samples or seconds, not both. */
export interface EngineCapturePunchRequest {
  /** First timeline sample captured. Not with `startSec`. */
  startSample?: number;
  /** First timeline second captured, rounded to the nearest sample. Not with `startSample`. */
  startSec?: number;
  /** One past the last timeline sample captured. Not with `endSec`. */
  endSample?: number;
  /** End of the window in timeline seconds, rounded to the nearest sample. Not with `endSample`. */
  endSec?: number;
  /** Whether the punch window is active. Default `true`. */
  enabled?: boolean;
}
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
/** Program and monitor output from {@link RealtimeEngine.processWithMonitor}. */
export type EngineProcessWithMonitorResult = WasmEngineProcessWithMonitorResult;
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

export interface EngineCapabilities {
  engineAbiVersion: number;
  expectedEngineAbiVersion: number;
  abiCompatible: boolean;
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
  mode: 'sab' | 'postMessage';
}
