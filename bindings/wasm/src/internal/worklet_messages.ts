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
  ProjectMidiCcBinding,
  UmpWords,
} from '../index.js';
import type { AutomationCurve, SurroundPan } from '../public_types.js';
import type { ExternalMidiEvent, MidiFxConfig } from '../public_types_engine.js';
import type { PlaybackRendererConfig } from '../public_types_playback.js';
import type {
  EngineClipPageRequestMessage,
  SonarePlaybackWorkletProcessorDiagnostics,
} from '../public_types_worklet.js';
import type {
  EngineTelemetryRecord,
  WorkletMeterSnapshot,
  WorkletScopeSnapshot,
  WorkletSpectrumSnapshot,
} from '../worklet/protocol.js';
import { refuse } from './entry.js';
import type { SonareFailurePayload } from './failure_payload.js';

export type {
  EngineClipPageRequestMessage,
  EngineSyncErrorMessage,
  WorkletPort,
} from '../public_types_worklet.js';

export type {
  SonareStreamAnalyzerChunkMessage,
  SonareStreamAnalyzerDestroyMessage,
  SonareStreamAnalyzerMessage,
  SonareStreamAnalyzerRecycleMessage,
} from './stream_analyzer_messages.js';
export type {
  SonareRealtimeVoiceChangerAllNotesOffMessage,
  SonareRealtimeVoiceChangerDestroyMessage,
  SonareRealtimeVoiceChangerErrorMessage,
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerNoteOffMessage,
  SonareRealtimeVoiceChangerNoteOnMessage,
  SonareRealtimeVoiceChangerResetMessage,
  SonareRealtimeVoiceChangerSetConfigMessage,
} from './voice_changer_messages.js';

export interface SonarePlaybackConfigMessage {
  type: 'config';
  /** A complete configuration document. */
  config: PlaybackRendererConfig;
}

export interface SonarePlaybackOrientationMessage {
  type: 'orientation';
  /** Degrees; see `PlaybackRenderer.setHeadOrientation`. */
  yaw: number;
  pitch?: number;
  roll?: number;
}

/** Clears the pipeline after a seek; the handler runs between render quanta. */
export interface SonarePlaybackResetMessage {
  type: 'reset';
}

/** Requests a {@link SonarePlaybackDiagnosticsReplyMessage}. */
export interface SonarePlaybackDiagnosticsRequestMessage {
  type: 'diagnostics';
}

export interface SonarePlaybackDestroyMessage {
  type: 'destroy';
}

export type SonarePlaybackMessage =
  | SonarePlaybackConfigMessage
  | SonarePlaybackOrientationMessage
  | SonarePlaybackResetMessage
  | SonarePlaybackDiagnosticsRequestMessage
  | SonarePlaybackDestroyMessage;

export interface SonarePlaybackDiagnosticsReplyMessage {
  type: 'diagnostics';
  diagnostics: SonarePlaybackWorkletProcessorDiagnostics;
}

/** Posted when a control message is refused (e.g. a changed prepare key). */
export interface SonarePlaybackErrorMessage {
  type: 'error';
  request: SonarePlaybackMessage['type'] | 'init';
  message: string;
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

/** Batch of external-MIDI events posted from the worklet once per render block. */
export interface SonareWorkletExternalMidiMessage {
  type: 'externalMidi';
  events: ExternalMidiEvent[];
}

/** A control-plane sync message that the worklet rejected without crashing, as posted. */
export interface SonareEngineSyncErrorWireMessage {
  type: 'syncError';
  /** The refusal's class, message and any SonareError code, as raised on the audio thread. */
  failure?: SonareFailurePayload;
  syncType: SonareEngineSyncMessage['type'] | (string & Record<never, never>);
  message: string;
}

export type SonareWorkletTransportMessage =
  | WorkletMeterSnapshot
  | WorkletScopeSnapshot
  | WorkletSpectrumSnapshot
  | SonareWorkletExternalMidiMessage
  | EngineClipPageRequestMessage
  | EngineTelemetryRecord;

/** A processor's host channel as it posts through it; constructors take the public `WorkletTransport`. */
export interface TypedWorkletTransport {
  postMessage?: (
    message:
      | SonareWorkletTransportMessage
      | SonareEngineCaptureResponseMessageInternal
      | SonareEngineTransportResponseMessage
      | SonareEngineInsertGainReductionResponseMessage
      | SonareEngineSyncErrorWireMessage,
    transfer?: Transferable[],
  ) => void;
  onMeter?: (meter: WorkletMeterSnapshot) => void;
  onSpectrum?: (spectrum: WorkletSpectrumSnapshot) => void;
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
    clickSeconds: config.clickSec ?? DEFAULT_METRONOME_CONFIG.clickSeconds,
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
    (config.clickSec !== undefined &&
      (!Number.isFinite(config.clickSec) || config.clickSec < 0 || config.clickSec > 1))
  ) {
    throw refuse('invalid metronome gains or click length');
  }
  return resolved;
}

/** A clip as posted to the processor; a page provider never crosses the port. */
export type SonareEngineSyncedClip = Omit<EngineClip, 'pageProvider'>;

// Out-of-band control messages posted from the main-thread SonareEngine facade
// to the worklet engine processor over node.port. Unlike SonareEngineCommandRecord
// (a small POD POSTed/ringed every block) these carry bulk/structured payloads
// (clip audio buffers, marker lists, metronome config) that cannot fit the
// fixed-size SAB command record. Their port handlers still execute on the
// AudioWorklet rendering thread, so consumers must validate them without
// allocation and publish bounded snapshots for process() to consume.
export interface SonareEngineSyncClipsMessage {
  type: 'syncClips';
  clips: SonareEngineSyncedClip[];
}

export interface SonareEngineSyncClipsDeltaMessage {
  type: 'syncClipsDelta';
  upserts: SonareEngineSyncedClip[];
  removeIds: number[];
}

/** Begins a paged, pre-baked clip transfer. PCM pages follow in FIFO order. */
export interface SonareEngineSyncClipPageProviderMessage {
  type: 'syncClipPageProvider';
  clipId: number;
  /**
   * Identity of the OPFS stream that owns the provider. A message whose key
   * differs from the provider's is ignored; omitted for pre-baked transfers.
   */
  streamKey?: number;
  /** Omitted when an OPFS stream is primed before its clip is scheduled. */
  clip?: SonareEngineSyncedClip;
  numChannels: number;
  numSamples: number;
  pageFrames: number;
}

/** Supplies one bounded PCM page for a pending pre-baked clip transfer. */
export interface SonareEngineSyncClipPageMessage {
  type: 'syncClipPage';
  clipId: number;
  /** Owning OPFS stream; see {@link SonareEngineSyncClipPageProviderMessage.streamKey}. */
  streamKey?: number;
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

/**
 * Sets the worklet engine's concurrent time-stretch voice capacity. The
 * capacity was already validated (and applied to the offline engine) by the
 * caller, so the worklet side just mirrors the accepted value; the pool
 * rebuild itself happens on the audio thread, at the next block boundary.
 */
export interface SonareEngineSyncWarpVoiceCapacityMessage {
  type: 'syncWarpVoiceCapacity';
  voices: number;
}

/** Evicts one page after the main-thread sliding window advances. */
export interface SonareEngineSyncClipPageClearMessage {
  type: 'syncClipPageClear';
  clipId: number;
  /** Owning OPFS stream; see {@link SonareEngineSyncClipPageProviderMessage.streamKey}. */
  streamKey?: number;
  pageIndex: number;
}

/** Releases a provider whose clip was removed or whose stream was closed. */
export interface SonareEngineSyncClipPageDestroyMessage {
  type: 'syncClipPageDestroy';
  clipId: number;
  /** Owning OPFS stream; see {@link SonareEngineSyncClipPageProviderMessage.streamKey}. */
  streamKey?: number;
}

/** Makes a fully supplied paged clip visible to the audio engine. */
export interface SonareEngineSyncClipPageCommitMessage {
  type: 'syncClipPageCommit';
  clipId: number;
  /** Owning OPFS stream; see {@link SonareEngineSyncClipPageProviderMessage.streamKey}. */
  streamKey?: number;
  /**
   * Clip schedule to publish with a provider that was primed before its
   * schedule was known. The original pre-baked push path supplies it on the
   * provider message instead.
   */
  clip?: SonareEngineSyncedClip;
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

export type SonareEngineSyncMixerInsertParamOverride =
  | {
      kind: 'track';
      trackId: number;
      insertIndex: number;
      paramName: string;
      value: number;
    }
  | {
      kind: 'bus';
      busId: number;
      insertIndex: number;
      paramName: string;
      value: number;
    }
  | {
      kind: 'master';
      insertIndex: number;
      paramName: string;
      value: number;
    };

export interface SonareEngineSyncMixerMessage {
  type: 'syncMixer';
  /** Apply only supplied sidechain bindings; preserve current mixer topology. */
  sidechainDelta?: boolean;
  lanes: EngineTrackLane[];
  buses?: EngineBus[];
  trackStrips?: Array<{ trackId: number; sceneJson: string }>;
  busStrips?: Array<{ busId: number; sceneJson: string }>;
  masterStripJson?: string;
  /** Retained by-name insert values, applied after strip and sidechain replay. */
  insertParamOverrides?: SonareEngineSyncMixerInsertParamOverride[];
  /** Strips whose manual insert bases are cleared after the strip replay, before insertParamOverrides. */
  insertBaseResets?: Array<
    { kind: 'track'; trackId: number } | { kind: 'bus'; busId: number } | { kind: 'master' }
  >;
  /** Lane insert sidechain bindings (replayed after lanes/strips). */
  laneSidechains?: Array<{ trackId: number; insertIndex: number; sourceTrackId: number }>;
  /**
   * Bus insert sidechain bindings (replayed after lanes/buses/strips).
   * `sourceKind` is a `SonareSidechainSourceKind` ordinal (0 track / 1 bus).
   */
  busSidechains?: Array<{
    busId: number;
    insertIndex: number;
    sourceKind: number;
    sourceId: number;
  }>;
  /** Master strip insert sidechain bindings. Same `sourceKind` convention. */
  masterSidechains?: Array<{ insertIndex: number; sourceKind: number; sourceId: number }>;
}

export interface SonareEngineSyncCaptureMessage {
  type: 'syncCapture';
  bufferFrames: number;
  channels: number;
  source: EngineCaptureStatus['source'];
  recordOffsetSamples: number;
  inputMonitor: { enabled: boolean; gain: number };
  /** Punch window in timeline samples; absent means no punch window. */
  punch?: { startSample: number; endSample: number };
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

export interface SonareEngineSyncBusStripEqBandMessage {
  type: 'syncBusStripEqBand';
  busId: number;
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

export interface SonareEngineSyncTrackStripSurroundPanMessage {
  type: 'syncTrackStripSurroundPan';
  trackId: number;
  pan: Required<SurroundPan>;
}

export interface SonareEngineSyncBusStripPanMessage {
  type: 'syncBusStripPan';
  busId: number;
  pan: number;
}

export interface SonareEngineSyncBusStripPanLawMessage {
  type: 'syncBusStripPanLaw';
  busId: number;
  panLaw: number;
}

export interface SonareEngineSyncBusStripPanModeMessage {
  type: 'syncBusStripPanMode';
  busId: number;
  panMode: number;
}

export interface SonareEngineSyncBusStripDualPanMessage {
  type: 'syncBusStripDualPan';
  busId: number;
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
  patch: Record<string, unknown>;
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
    gsEfxRealization?: 'modern' | 'classic';
  };
}

export interface SonareEngineSyncLoadSoundFontMessage {
  type: 'syncLoadSoundFont';
  data: Uint8Array;
}

export interface SonareEngineSyncMidiFxMessage {
  type: 'syncMidiFx' | 'syncClearMidiFx';
  destinationId: number;
  /** Engine MIDI-FX config; present for 'syncMidiFx', absent for a clear. */
  config?: MidiFxConfig;
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
  words: UmpWords;
  renderFrame: number;
}

/**
 * One of the three per-note expression dimensions, addressed to a destination.
 * `data0`/`data1` carry the dimension's own payload the way the live-input
 * message beside this one does: bend14 / 0, pressure / 0, or note / pressure.
 * A bend is 14-bit, which is why these do not ride the 7-bit CC message.
 */
export interface SonareEngineSyncMidiExpressionMessage {
  type: 'syncMidiPitchBend' | 'syncMidiChannelPressure' | 'syncMidiPolyPressure';
  destinationId: number;
  group: number;
  channel: number;
  data0: number;
  data1: number;
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
  binding: ProjectMidiCcBinding;
}

export interface SonareEngineSyncMidiInputEventMessage {
  type:
    | 'syncMidiInputNoteOn'
    | 'syncMidiInputNoteOff'
    | 'syncMidiInputCc'
    | 'syncMidiInputPitchBend'
    | 'syncMidiInputChannelPressure'
    | 'syncMidiInputPolyPressure';
  group: number;
  channel: number;
  data0: number;
  data1: number;
  offsetInBlockSamples: number;
}

export interface SonareEngineSyncMidiInputUmpMessage {
  type: 'syncMidiInputUmp';
  words: UmpWords;
  offsetInBlockSamples: number;
}

/** Releases the realtime engine and all worklet-owned clip buffers. */
export interface SonareEngineDestroyMessage {
  type: 'destroy';
}

export type SonareEngineSyncMessage =
  | SonareEngineSyncClipsMessage
  | SonareEngineSyncClipsDeltaMessage
  | SonareEngineSyncClipPageProviderMessage
  | SonareEngineSyncClipPageMessage
  | SonareEngineSyncClipPageClearMessage
  | SonareEngineSyncClipPagePrefetchFramesMessage
  | SonareEngineSyncWarpVoiceCapacityMessage
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
  | SonareEngineSyncBusStripEqBandMessage
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
  | SonareEngineSyncTrackStripSurroundPanMessage
  | SonareEngineSyncBusStripPanMessage
  | SonareEngineSyncBusStripPanLawMessage
  | SonareEngineSyncBusStripPanModeMessage
  | SonareEngineSyncBusStripDualPanMessage
  | SonareEngineSyncTrackStripChannelDelaySamplesMessage
  | SonareEngineSyncBuiltinInstrumentMessage
  | SonareEngineSyncSynthInstrumentMessage
  | SonareEngineSyncSf2InstrumentMessage
  | SonareEngineSyncLoadSoundFontMessage
  | SonareEngineSyncMidiFxMessage
  | SonareEngineSyncMidiNoteMessage
  | SonareEngineSyncMidiCcMessage
  | SonareEngineSyncMidiUmpMessage
  | SonareEngineSyncMidiExpressionMessage
  | SonareEngineSyncMidiSysexMessage
  | SonareEngineSyncMidiPanicMessage
  | SonareEngineSyncMidiDestinationExternalMessage
  | SonareEngineSyncExternalMidiClockMessage
  | SonareEngineSyncMidiInputSourceMessage
  | SonareEngineSyncMidiCcBindingMessage
  | SonareEngineSyncMidiInputEventMessage
  | SonareEngineSyncMidiInputUmpMessage
  | SonareEngineDestroyMessage;

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
  /** The failure's class, message and any SonareError code, when `ok` is false. */
  failure?: SonareFailurePayload;
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
  failure: SonareFailurePayload;
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
  /** The failure's class, message and any SonareError code, when `ok` is false. */
  failure?: SonareFailurePayload;
}

/** Asks the worklet for one target's per-insert gain reduction (any meter delivery mode). */
export interface SonareEngineInsertGainReductionRequestMessage {
  type: 'insertGainReductionRequest';
  requestId: number;
  targetId: number;
}

/** Reply to {@link SonareEngineInsertGainReductionRequestMessage}; `values` is present when `ok`. */
export interface SonareEngineInsertGainReductionResponseMessage {
  type: 'insertGainReductionResponse';
  requestId: number;
  ok: boolean;
  values?: number[];
  error?: string;
  /** The failure's class, message and any SonareError code, when `ok` is false. */
  failure?: SonareFailurePayload;
}
