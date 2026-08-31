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
  EngineTransportState,
  RealtimeVoiceChangerConfigInput,
  RealtimeVoiceChangerPodConfig,
} from '../index';
import type { AutomationCurve } from '../public_types';
import type {
  SonareEngineCommandRecord,
  SonareEngineTelemetryRecord,
  SonareWorkletMeterSnapshot,
  SonareWorkletSpectrumSnapshot,
} from './protocol';

export interface SonareWorkletProcessorOptions {
  sceneJson: string;
  sampleRate?: number;
  blockSize?: number;
  stripCount?: number;
  meterIntervalFrames?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  spectrumIntervalFrames?: number;
  spectrumBands?: number;
  spectrumSharedBuffer?: SharedArrayBuffer;
  spectrumRingCapacity?: number;
}

export interface SonareRealtimeEngineWorkletProcessorOptions {
  wasmBinary?: ArrayBuffer | Uint8Array;
  initialSyncMessages?: SonareEngineSyncMessage[];
  initialCommands?: SonareEngineCommandRecord[];
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
  meterIntervalFrames?: number;
  commandSharedBuffer?: SharedArrayBuffer;
  commandRingCapacity?: number;
  telemetrySharedBuffer?: SharedArrayBuffer;
  telemetryRingCapacity?: number;
  meterSharedBuffer?: SharedArrayBuffer;
  meterRingCapacity?: number;
  // Scope telemetry (FFT spectrum + goniometer): opt-in. The ring is created
  // only when scopeIntervalFrames > 0, since the per-block FFT is heavier than
  // the meter path. scopeBands selects the linear band resolution.
  scopeIntervalFrames?: number;
  scopeBands?: number;
  scopeSharedBuffer?: SharedArrayBuffer;
  scopeRingCapacity?: number;
  /**
   * Lock-free SPSC queue for clip-page cache misses. Supplying this is required
   * for realtime-safe OPFS streaming: it avoids both embind object creation and
   * postMessage structured cloning from AudioWorklet process().
   */
  clipPageRequestSharedBuffer?: SharedArrayBuffer;
  clipPageRequestRingCapacity?: number;
  /** Lock-free worklet-to-main-thread MIDI-1 output ring. */
  externalMidiSharedBuffer?: SharedArrayBuffer;
  externalMidiRingCapacity?: number;
  /**
   * Route the PFL/AFL cue bus to the processor's SECOND output instead of
   * folding it into the program output. Off by default, so an existing
   * single-output host keeps its current mix sample for sample. The node must
   * be constructed with two outputs for the cue to be audible.
   */
  cueOutput?: boolean;
}

export interface SonareRealtimeVoiceChangerWorkletProcessorOptions {
  preset?: RealtimeVoiceChangerConfigInput;
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
}

export interface SonareRealtimeVoiceChangerSetConfigMessage {
  type: 'setConfig';
  /** Pre-normalized by the main thread; never JSON parsed in the worklet. */
  config: RealtimeVoiceChangerPodConfig;
}

export interface SonareRealtimeVoiceChangerResetMessage {
  type: 'reset';
}

export interface SonareRealtimeVoiceChangerDestroyMessage {
  type: 'destroy';
}

export type SonareRealtimeVoiceChangerMessage =
  | SonareRealtimeVoiceChangerSetConfigMessage
  | SonareRealtimeVoiceChangerResetMessage
  | SonareRealtimeVoiceChangerDestroyMessage;

export interface SonareRealtimeEngineNodeCapabilities {
  mode: 'sab' | 'postMessage';
  runtimeTarget: 'embind';
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
  /** True only when clip-page misses use the bounded SAB ring. */
  clipPageRequestsRealtimeSafe: boolean;
  /** True when external MIDI uses the SAB output ring rather than postMessage. */
  externalMidiRealtimeSafe: boolean;
  /**
   * True when the node carries a second output fed by the PFL/AFL cue bus. When
   * false the cue is folded into the program output, as it always was.
   */
  cueOutput: boolean;
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  abiCompatible?: boolean;
  degradedReason?: string;
  readyMessage?: boolean;
}

export interface SonareRealtimeEngineNodeOptions
  extends SonareRealtimeEngineWorkletProcessorOptions {
  processorName?: string;
  moduleUrl?: string | URL;
  mode?: 'auto' | 'sab' | 'postMessage';
  engineAbiVersion?: number;
  expectedEngineAbiVersion?: number;
  requireAbiCompatible?: boolean;
  nodeFactory?: (
    context: BaseAudioContext,
    processorName: string,
    options: AudioWorkletNodeOptions,
  ) => AudioWorkletNode;
}

export interface SonareEngineTransportFacade {
  play(sampleTime?: number): boolean;
  stop(sampleTime?: number): boolean;
  seekPpq(ppq: number, sampleTime?: number): boolean;
  seekSeconds(seconds: number, sampleTime?: number): boolean;
  setTempo(bpm: number): void;
  setTempoSegments(segments: readonly EngineTempoSegment[]): void;
  setLoop(startPpq: number, endPpq: number, enabled?: boolean): boolean;
}

export interface SonareWorkletScheduleInsertAutomationMessage {
  type: 'scheduleInsertAutomation';
  stripIndex: number;
  insertIndex: number;
  paramId: number;
  value: number;
  samplePos?: number;
  curve?: AutomationCurve;
}

export interface SonareWorkletSetMeterIntervalMessage {
  type: 'setMeterInterval';
  frames: number;
}

export interface SonareWorkletDestroyMessage {
  type: 'destroy';
}

export type SonareWorkletMessage =
  | SonareWorkletScheduleInsertAutomationMessage
  | SonareWorkletSetMeterIntervalMessage
  | SonareWorkletDestroyMessage;

/** One external-MIDI event delivered to the main thread, lowered to MIDI 1.0
 * bytes ready for a Web MIDI output port. */
export interface SonareWorkletExternalMidiEvent {
  /** Originating track lane, or 0xFFFFFFFF for transport/clock bytes. */
  destinationId: number;
  /** Sample position within the producing block. */
  renderFrame: number;
  /** MIDI 1.0 status + data bytes (1..3 entries). */
  bytes: number[];
}

/** Batch of external-MIDI events posted from the worklet once per render block. */
export interface SonareWorkletExternalMidiMessage {
  type: 'externalMidi';
  events: SonareWorkletExternalMidiEvent[];
}

/** A control-plane sync message that the worklet rejected without crashing. */
export interface SonareEngineSyncErrorMessage {
  type: 'syncError';
  /**
   * Discriminant of the rejected sync. Usually a known member, but a
   * `sync`-prefixed type this build does not recognize is reported here too
   * rather than dropped, so the set is open. `Record<never, never>` keeps the
   * known members as editor completions instead of collapsing to `string`.
   */
  syncType: SonareEngineSyncMessage['type'] | (string & Record<never, never>);
  message: string;
}

export type SonareWorkletTransportMessage =
  | SonareWorkletMeterSnapshot
  | SonareWorkletSpectrumSnapshot
  | SonareWorkletExternalMidiMessage
  | SonareEngineClipPageRequestMessage
  | SonareEngineTelemetryRecord;

export interface WorkletTransport {
  postMessage?: (
    message:
      | SonareWorkletTransportMessage
      | SonareEngineCaptureResponseMessageInternal
      | SonareEngineTransportResponseMessage
      | SonareEngineSyncErrorMessage,
    transfer?: Transferable[],
  ) => void;
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
  onSpectrum?: (spectrum: SonareWorkletSpectrumSnapshot) => void;
}

export interface ResolvedMetronomeConfig {
  beatGain: number;
  accentGain: number;
  clickSamples: number;
  clickSeconds: number;
}

// Fallback metronome gains/click length used by the worklet consumer until the
// host posts a 'syncMetronome' config. Aligned with the embind setMetronome
// defaults (src/wasm/bindings.cpp) so offline and realtime metronomes match.
export const DEFAULT_METRONOME_CONFIG: ResolvedMetronomeConfig = {
  beatGain: 0.35,
  accentGain: 0.7,
  clickSamples: 0,
  clickSeconds: 0,
};

export function resolveMetronomeConfig(config: EngineMetronomeConfig): ResolvedMetronomeConfig {
  const resolved = {
    beatGain: config.beatGain ?? DEFAULT_METRONOME_CONFIG.beatGain,
    accentGain: config.accentGain ?? DEFAULT_METRONOME_CONFIG.accentGain,
    clickSamples: config.clickSamples ?? DEFAULT_METRONOME_CONFIG.clickSamples,
    clickSeconds: config.clickSeconds ?? DEFAULT_METRONOME_CONFIG.clickSeconds,
  };
  if (
    !Number.isFinite(resolved.beatGain) ||
    resolved.beatGain < 0 ||
    !Number.isFinite(resolved.accentGain) ||
    resolved.accentGain < 0 ||
    !Number.isInteger(resolved.clickSamples) ||
    resolved.clickSamples < 0 ||
    !Number.isFinite(resolved.clickSeconds) ||
    resolved.clickSeconds < 0 ||
    resolved.clickSamples > 384000 ||
    (config.clickSeconds !== undefined &&
      (!Number.isFinite(config.clickSeconds) || config.clickSeconds < 0 || config.clickSeconds > 1))
  ) {
    throw new RangeError('invalid metronome gains or click length');
  }
  return resolved;
}

// Out-of-band control messages posted from the main-thread SonareEngine facade
// to the worklet engine processor over node.port. Unlike SonareEngineCommandRecord
// (a small POD POSTed/ringed every block) these carry bulk/structured payloads
// (clip audio buffers, marker lists, metronome config) that cannot fit the
// fixed-size SAB command record. Their port handlers still execute on the
// AudioWorklet rendering thread, so consumers must validate them without
// allocation and publish bounded snapshots for process() to consume.
export interface SonareEngineSyncClipsMessage {
  type: 'syncClips';
  clips: EngineClip[];
}

export interface SonareEngineSyncClipsDeltaMessage {
  type: 'syncClipsDelta';
  upserts: EngineClip[];
  removeIds: number[];
}

/** Begins a paged, pre-baked clip transfer. PCM pages follow in FIFO order. */
export interface SonareEngineSyncClipPageProviderMessage {
  type: 'syncClipPageProvider';
  clipId: number;
  /** Omitted when an OPFS stream is primed before its clip is scheduled. */
  clip?: EngineClip;
  numChannels: number;
  numSamples: number;
  pageFrames: number;
}

/** Supplies one bounded PCM page for a pending pre-baked clip transfer. */
export interface SonareEngineSyncClipPageMessage {
  type: 'syncClipPage';
  clipId: number;
  pageIndex: number;
  channels: Float32Array[];
}

/**
 * Sets the worklet engine's clip-page look-ahead window in timeline frames, so
 * the audio thread reports the pages it is about to read before it reads them.
 * 0 disables the look-ahead.
 */
export interface SonareEngineSyncClipPagePrefetchFramesMessage {
  type: 'syncClipPagePrefetchFrames';
  frames: number;
}

/** Evicts one page after the main-thread sliding window advances. */
export interface SonareEngineSyncClipPageClearMessage {
  type: 'syncClipPageClear';
  clipId: number;
  pageIndex: number;
}

/** Releases a provider whose clip was removed or whose stream was closed. */
export interface SonareEngineSyncClipPageDestroyMessage {
  type: 'syncClipPageDestroy';
  clipId: number;
}

/** Makes a fully supplied paged clip visible to the audio engine. */
export interface SonareEngineSyncClipPageCommitMessage {
  type: 'syncClipPageCommit';
  clipId: number;
  /**
   * Clip schedule to publish with a provider that was primed before its
   * schedule was known. The original pre-baked push path supplies it on the
   * provider message instead.
   */
  clip?: EngineClip;
}

/**
 * A bounded batch of page misses emitted by the AudioWorklet. The main thread
 * resolves these from OPFS and responds with `syncClipPage`; a missing page is
 * silent until that response arrives.
 */
export interface SonareEngineClipPageRequestMessage {
  type: 'clipPageRequest';
  requests: Array<{ clipId: number; pageIndex: number }>;
  /** Number of misses dropped because a bounded native or SAB request queue was full. */
  dropped?: number;
}

export interface SonareEngineSyncMidiClipsMessage {
  type: 'syncMidiClips';
  clips: EngineMidiClipSchedule[];
}

export interface SonareEngineSyncMarkersMessage {
  type: 'syncMarkers';
  markers: EngineMarker[];
}

export interface SonareEngineSyncMetronomeMessage {
  type: 'syncMetronome';
  config: EngineMetronomeConfig;
}

export interface SonareEngineSyncAutomationMessage {
  type: 'syncAutomation';
  paramId: number;
  points: EngineAutomationPoint[];
}

/** Replaces the live engine's registered custom-parameter set. */
export interface SonareEngineSyncParametersMessage {
  type: 'syncParameters';
  parameters: EngineParameterInfo[];
}

export interface SonareEngineSyncTempoMessage {
  type: 'syncTempo';
  bpm: number;
  timeSignature: { numerator: number; denominator: number };
  tempoSegments?: EngineTempoSegment[];
  timeSignatureSegments?: EngineTimeSignatureSegment[];
}

export interface SonareEngineSyncMixerMessage {
  type: 'syncMixer';
  lanes: EngineTrackLane[];
  buses?: EngineBus[];
  trackStrips?: Array<{ trackId: number; sceneJson: string }>;
  busStrips?: Array<{ busId: number; sceneJson: string }>;
  masterStripJson?: string;
  /** Lane insert sidechain bindings (replayed after lanes/strips). */
  laneSidechains?: Array<{ trackId: number; insertIndex: number; sourceTrackId: number }>;
}

export interface SonareEngineSyncCaptureMessage {
  type: 'syncCapture';
  bufferFrames: number;
  channels: number;
  source: EngineCaptureStatus['source'];
  recordOffsetSamples: number;
  inputMonitor: { enabled: boolean; gain: number };
}

export interface SonareEngineSyncTrackStripEqBandMessage {
  type: 'syncTrackStripEqBand';
  trackId: number;
  bandIndex: number;
  bandJson: string;
}

export interface SonareEngineSyncMasterStripEqBandMessage {
  type: 'syncMasterStripEqBand';
  bandIndex: number;
  bandJson: string;
}

export interface SonareEngineSyncTrackStripInsertBypassedMessage {
  type: 'syncTrackStripInsertBypassed';
  trackId: number;
  insertIndex: number;
  bypassed: boolean;
  resetOnBypass: boolean;
}

export interface SonareEngineSyncMasterStripInsertBypassedMessage {
  type: 'syncMasterStripInsertBypassed';
  insertIndex: number;
  bypassed: boolean;
  resetOnBypass: boolean;
}

export interface SonareEngineSyncTrackStripInsertParamByNameMessage {
  type: 'syncTrackStripInsertParamByName';
  trackId: number;
  insertIndex: number;
  paramName: string;
  value: number;
}

export interface SonareEngineSyncMasterStripInsertParamByNameMessage {
  type: 'syncMasterStripInsertParamByName';
  insertIndex: number;
  paramName: string;
  value: number;
}

export interface SonareEngineSyncBusStripInsertParamByNameMessage {
  type: 'syncBusStripInsertParamByName';
  busId: number;
  insertIndex: number;
  paramName: string;
  value: number;
}

export interface SonareEngineSyncBusStripInsertBypassedMessage {
  type: 'syncBusStripInsertBypassed';
  busId: number;
  insertIndex: number;
  bypassed: boolean;
  resetOnBypass: boolean;
}

export interface SonareEngineSyncTrackStripPanMessage {
  type: 'syncTrackStripPan';
  trackId: number;
  pan: number;
}

export interface SonareEngineSyncTrackStripPanLawMessage {
  type: 'syncTrackStripPanLaw';
  trackId: number;
  panLaw: number;
}

export interface SonareEngineSyncTrackStripPanModeMessage {
  type: 'syncTrackStripPanMode';
  trackId: number;
  panMode: number;
}

export interface SonareEngineSyncTrackStripDualPanMessage {
  type: 'syncTrackStripDualPan';
  trackId: number;
  leftPan: number;
  rightPan: number;
}

export interface SonareEngineSyncTrackStripChannelDelaySamplesMessage {
  type: 'syncTrackStripChannelDelaySamples';
  trackId: number;
  delaySamples: number;
}

export interface SonareEngineSyncBuiltinInstrumentMessage {
  type: 'syncBuiltinInstrument';
  destinationId: number;
  config: { destinationId?: number } & Record<string, unknown>;
}

export interface SonareEngineSyncSynthInstrumentMessage {
  type: 'syncSynthInstrument';
  destinationId: number;
  patch: Record<string, unknown> | string;
}

export interface SonareEngineSyncSf2InstrumentMessage {
  type: 'syncSf2Instrument';
  destinationId: number;
  config: {
    destinationId?: number;
    gain?: number;
    polyphony?: number;
    preferModelForModeledFamilies?: boolean;
    clearBankRig?: boolean;
  };
}

export interface SonareEngineSyncLoadSoundFontMessage {
  type: 'syncLoadSoundFont';
  data: Uint8Array;
}

export interface SonareEngineSyncMidiFxMessage {
  type: 'syncMidiFx' | 'syncClearMidiFx';
  destinationId: number;
  /** Engine MIDI-FX config JSON; present for 'syncMidiFx', absent for a clear. */
  configJson?: string;
}

export interface SonareEngineSyncMidiNoteMessage {
  type: 'syncMidiNoteOn' | 'syncMidiNoteOff';
  destinationId: number;
  group: number;
  channel: number;
  note: number;
  velocity: number;
  renderFrame: number;
}

export interface SonareEngineSyncMidiCcMessage {
  type: 'syncMidiCc';
  destinationId: number;
  group: number;
  channel: number;
  controller: number;
  value: number;
  renderFrame: number;
}

export interface SonareEngineSyncMidiUmpMessage {
  type: 'syncMidiUmp';
  destinationId: number;
  word0: number;
  renderFrame: number;
}

export interface SonareEngineSyncMidiSysexMessage {
  type: 'syncMidiSysex';
  destinationId: number;
  data: Uint8Array;
  renderFrame: number;
}

export interface SonareEngineSyncMidiPanicMessage {
  type: 'syncMidiPanic';
  renderFrame: number;
}

export interface SonareEngineSyncMidiDestinationExternalMessage {
  type: 'syncMidiDestinationExternal';
  destinationId: number;
  external: boolean;
}

export interface SonareEngineSyncExternalMidiClockMessage {
  type: 'syncExternalMidiClock';
  enabled: boolean;
}

export interface SonareEngineSyncMidiInputSourceMessage {
  type: 'syncMidiInputSource' | 'syncClearMidiInputSource';
  destinationId?: number;
}

export interface SonareEngineSyncMidiCcBindingMessage {
  type: 'syncMidiCcBinding';
  channel: number;
  controller: number;
  paramId: number;
  minValue: number;
  maxValue: number;
}

export interface SonareEngineSyncMidiInputEventMessage {
  type: 'syncMidiInputNoteOn' | 'syncMidiInputNoteOff' | 'syncMidiInputCc';
  group: number;
  channel: number;
  data0: number;
  data1: number;
  portTimeSamples: number;
}

/** Releases the realtime engine and all worklet-owned clip buffers. */
export interface SonareEngineDestroyMessage {
  type: 'destroy';
}

export type SonareEngineInstrumentSyncMessage =
  | SonareEngineSyncBuiltinInstrumentMessage
  | SonareEngineSyncSynthInstrumentMessage
  | SonareEngineSyncSf2InstrumentMessage
  | SonareEngineSyncLoadSoundFontMessage
  | SonareEngineSyncMidiFxMessage;

export type SonareEngineSyncMessage =
  | SonareEngineSyncClipsMessage
  | SonareEngineSyncClipsDeltaMessage
  | SonareEngineSyncClipPageProviderMessage
  | SonareEngineSyncClipPageMessage
  | SonareEngineSyncClipPageClearMessage
  | SonareEngineSyncClipPagePrefetchFramesMessage
  | SonareEngineSyncClipPageCommitMessage
  | SonareEngineSyncClipPageDestroyMessage
  | SonareEngineSyncMidiClipsMessage
  | SonareEngineSyncMarkersMessage
  | SonareEngineSyncMetronomeMessage
  | SonareEngineSyncAutomationMessage
  | SonareEngineSyncParametersMessage
  | SonareEngineSyncTempoMessage
  | SonareEngineSyncMixerMessage
  | SonareEngineSyncCaptureMessage
  | SonareEngineSyncTrackStripEqBandMessage
  | SonareEngineSyncMasterStripEqBandMessage
  | SonareEngineSyncTrackStripInsertBypassedMessage
  | SonareEngineSyncMasterStripInsertBypassedMessage
  | SonareEngineSyncTrackStripInsertParamByNameMessage
  | SonareEngineSyncMasterStripInsertParamByNameMessage
  | SonareEngineSyncBusStripInsertParamByNameMessage
  | SonareEngineSyncBusStripInsertBypassedMessage
  | SonareEngineSyncTrackStripPanMessage
  | SonareEngineSyncTrackStripPanLawMessage
  | SonareEngineSyncTrackStripPanModeMessage
  | SonareEngineSyncTrackStripDualPanMessage
  | SonareEngineSyncTrackStripChannelDelaySamplesMessage
  | SonareEngineSyncBuiltinInstrumentMessage
  | SonareEngineSyncSynthInstrumentMessage
  | SonareEngineSyncSf2InstrumentMessage
  | SonareEngineSyncLoadSoundFontMessage
  | SonareEngineSyncMidiFxMessage
  | SonareEngineSyncMidiNoteMessage
  | SonareEngineSyncMidiCcMessage
  | SonareEngineSyncMidiUmpMessage
  | SonareEngineSyncMidiSysexMessage
  | SonareEngineSyncMidiPanicMessage
  | SonareEngineSyncMidiDestinationExternalMessage
  | SonareEngineSyncExternalMidiClockMessage
  | SonareEngineSyncMidiInputSourceMessage
  | SonareEngineSyncMidiCcBindingMessage
  | SonareEngineSyncMidiInputEventMessage
  | SonareEngineDestroyMessage;

export interface WorkletPort {
  postMessage?: (message: unknown, transfer?: Transferable[]) => void;
  onmessage?: (event: { data: unknown }) => void;
  addEventListener?: (type: 'message', listener: (event: { data: unknown }) => void) => void;
  start?: () => void;
}

export interface SonareEngineCaptureRequestMessage {
  type: 'captureRequest';
  requestId: number;
  op: 'status' | 'read' | 'reset';
}

/**
 * Public capture response shape retained for source compatibility.
 *
 * The worklet wire protocol is stricter than this legacy structural type; use
 * `SonareEngineCaptureResponseMessageInternal` inside the implementation.
 */
export interface SonareEngineCaptureResponseMessage {
  type: 'captureResponse';
  requestId: number;
  ok: boolean;
  status?: EngineCaptureStatus;
  channels?: Float32Array[] | number[][];
  error?: string;
}

interface SonareEngineCaptureStatusResponseMessageInternal {
  type: 'captureResponse';
  requestId: number;
  ok: true;
  status: EngineCaptureStatus;
}

interface SonareEngineCaptureReadResponseMessageInternal {
  type: 'captureResponse';
  requestId: number;
  ok: true;
  channels: Float32Array[];
}

interface SonareEngineCaptureResetResponseMessageInternal {
  type: 'captureResponse';
  requestId: number;
  ok: true;
}

interface SonareEngineCaptureErrorResponseMessageInternal {
  type: 'captureResponse';
  requestId: number;
  ok: false;
  error: string;
}

/** Strict wire response type used only by the worklet implementation. */
export type SonareEngineCaptureResponseMessageInternal =
  | SonareEngineCaptureStatusResponseMessageInternal
  | SonareEngineCaptureReadResponseMessageInternal
  | SonareEngineCaptureResetResponseMessageInternal
  | SonareEngineCaptureErrorResponseMessageInternal;

export interface SonareEngineTransportRequestMessage {
  type: 'transportRequest';
  requestId: number;
  op: 'state';
}

export interface SonareEngineTransportResponseMessage {
  type: 'transportResponse';
  requestId: number;
  ok: boolean;
  state?: EngineTransportState;
  error?: string;
}
