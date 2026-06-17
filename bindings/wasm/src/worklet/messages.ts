import type {
  EngineAutomationPoint,
  EngineBus,
  EngineCaptureStatus,
  EngineClip,
  EngineMarker,
  EngineMetronomeConfig,
  EngineMidiClipSchedule,
  EngineTempoSegment,
  EngineTimeSignatureSegment,
  EngineTrackLane,
  EngineTransportState,
  RealtimeVoiceChangerConfigInput,
} from '../index';
import type { AutomationCurve } from '../public_types';
import type { SonareRtModule } from '../sonare-rt';
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
  runtimeTarget?: 'embind' | 'sonare-rt';
  rtModuleUrl?: string;
  rtWasmBinary?: ArrayBuffer | Uint8Array;
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
}

export interface SonareRealtimeVoiceChangerWorkletProcessorOptions {
  preset?: RealtimeVoiceChangerConfigInput;
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
}

export interface SonareRealtimeVoiceChangerSetConfigMessage {
  type: 'setConfig';
  preset: RealtimeVoiceChangerConfigInput;
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
  runtimeTarget: 'embind' | 'sonare-rt';
  sharedArrayBuffer: boolean;
  atomics: boolean;
  audioWorklet: boolean;
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
  rtModuleUrl?: string;
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

export interface SonareRtRealtimeEngineRuntimeOptions {
  module: SonareRtModule;
  memory: WebAssembly.Memory;
  sampleRate?: number;
  blockSize?: number;
  channelCount?: number;
  commandSharedBuffer?: SharedArrayBuffer;
  commandRingCapacity?: number;
  telemetrySharedBuffer?: SharedArrayBuffer;
  telemetryRingCapacity?: number;
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

export type SonareWorkletTransportMessage =
  | SonareWorkletMeterSnapshot
  | SonareWorkletSpectrumSnapshot
  | SonareEngineTelemetryRecord;

export interface WorkletTransport {
  postMessage?: (
    message:
      | SonareWorkletTransportMessage
      | SonareEngineCaptureResponseMessage
      | SonareEngineTransportResponseMessage,
    transfer?: Transferable[],
  ) => void;
  onMeter?: (meter: SonareWorkletMeterSnapshot) => void;
  onSpectrum?: (spectrum: SonareWorkletSpectrumSnapshot) => void;
}

export interface ResolvedMetronomeConfig {
  beatGain: number;
  accentGain: number;
  clickSamples: number;
}

// Fallback metronome gains/click length used by the worklet consumer until the
// host posts a 'syncMetronome' config. Aligned with the embind setMetronome
// defaults (src/wasm/bindings.cpp) so offline and realtime metronomes match.
export const DEFAULT_METRONOME_CONFIG: ResolvedMetronomeConfig = {
  beatGain: 0.35,
  accentGain: 0.7,
  clickSamples: 96,
};

export function resolveMetronomeConfig(config: EngineMetronomeConfig): ResolvedMetronomeConfig {
  return {
    beatGain: config.beatGain ?? DEFAULT_METRONOME_CONFIG.beatGain,
    accentGain: config.accentGain ?? DEFAULT_METRONOME_CONFIG.accentGain,
    clickSamples: config.clickSamples ?? DEFAULT_METRONOME_CONFIG.clickSamples,
  };
}

// Out-of-band control messages posted from the main-thread SonareEngine facade
// to the worklet engine processor over node.port. Unlike SonareEngineCommandRecord
// (a small POD POSTed/ringed every block) these carry bulk/structured payloads
// (clip audio buffers, marker lists, metronome config) that cannot fit the
// fixed-size SAB command record, so they are applied OUTSIDE process() — the
// audio-thread equivalent of the engine's control-thread RtPublisher setters.
export interface SonareEngineSyncClipsMessage {
  type: 'syncClips';
  clips: EngineClip[];
}

export interface SonareEngineSyncClipsDeltaMessage {
  type: 'syncClipsDelta';
  upserts: EngineClip[];
  removeIds: number[];
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
  config: { destinationId?: number; gain?: number; polyphony?: number };
}

export interface SonareEngineSyncLoadSoundFontMessage {
  type: 'syncLoadSoundFont';
  data: Uint8Array;
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

export interface SonareEngineSyncMidiPanicMessage {
  type: 'syncMidiPanic';
  renderFrame: number;
}

export type SonareEngineInstrumentSyncMessage =
  | SonareEngineSyncBuiltinInstrumentMessage
  | SonareEngineSyncSynthInstrumentMessage
  | SonareEngineSyncSf2InstrumentMessage
  | SonareEngineSyncLoadSoundFontMessage;

export type SonareEngineSyncMessage =
  | SonareEngineSyncClipsMessage
  | SonareEngineSyncClipsDeltaMessage
  | SonareEngineSyncMidiClipsMessage
  | SonareEngineSyncMarkersMessage
  | SonareEngineSyncMetronomeMessage
  | SonareEngineSyncAutomationMessage
  | SonareEngineSyncTempoMessage
  | SonareEngineSyncMixerMessage
  | SonareEngineSyncCaptureMessage
  | SonareEngineSyncTrackStripEqBandMessage
  | SonareEngineSyncMasterStripEqBandMessage
  | SonareEngineSyncTrackStripInsertBypassedMessage
  | SonareEngineSyncMasterStripInsertBypassedMessage
  | SonareEngineSyncTrackStripInsertParamByNameMessage
  | SonareEngineSyncMasterStripInsertParamByNameMessage
  | SonareEngineSyncTrackStripPanMessage
  | SonareEngineSyncTrackStripPanLawMessage
  | SonareEngineSyncTrackStripPanModeMessage
  | SonareEngineSyncTrackStripDualPanMessage
  | SonareEngineSyncTrackStripChannelDelaySamplesMessage
  | SonareEngineSyncBuiltinInstrumentMessage
  | SonareEngineSyncSynthInstrumentMessage
  | SonareEngineSyncSf2InstrumentMessage
  | SonareEngineSyncLoadSoundFontMessage
  | SonareEngineSyncMidiNoteMessage
  | SonareEngineSyncMidiCcMessage
  | SonareEngineSyncMidiPanicMessage;

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

export interface SonareEngineCaptureResponseMessage {
  type: 'captureResponse';
  requestId: number;
  ok: boolean;
  status?: EngineCaptureStatus;
  channels?: Float32Array[] | number[][];
  error?: string;
}

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
