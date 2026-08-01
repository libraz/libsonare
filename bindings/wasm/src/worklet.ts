// Public entry for the AudioWorklet surface. The implementation is split into
// focused modules under ./worklet/*; tsup bundles them back into a single
// self-contained dist/worklet.js (code-splitting is disabled because a real
// AudioWorkletGlobalScope cannot resolve sibling chunks), so the public surface
// is unchanged.

export type { OpfsClipStream, OpfsClipStreamOptions } from './clip_page_streamer';
export { attachOpfsClipStream } from './clip_page_streamer';
// With code-splitting disabled, the worklet bundle carries its own copy of the
// module singleton. Re-export the lifecycle so that realm can initialize its
// own wasm instance, independent of the main-thread `index` module.
export { init, isInitialized } from './index';
export { SonareEngine } from './worklet/engine';
export { SonareRealtimeEngineNode } from './worklet/engine-node';
export type { SonareEngineOptions } from './worklet/engine-options';
export { SonareRealtimeEngineWorkletProcessor } from './worklet/engine-processor';
export { registerSonareRealtimeEngineWorkletProcessor } from './worklet/engine-register';
export type {
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessage,
  SonareEngineClipPageRequestMessage,
  SonareEngineSyncAutomationMessage,
  SonareEngineSyncBuiltinInstrumentMessage,
  SonareEngineSyncCaptureMessage,
  SonareEngineSyncClipsDeltaMessage,
  SonareEngineSyncClipsMessage,
  SonareEngineSyncLoadSoundFontMessage,
  SonareEngineSyncMarkersMessage,
  SonareEngineSyncMasterStripEqBandMessage,
  SonareEngineSyncMasterStripInsertBypassedMessage,
  SonareEngineSyncMessage,
  SonareEngineSyncMetronomeMessage,
  SonareEngineSyncMidiCcMessage,
  SonareEngineSyncMidiClipsMessage,
  SonareEngineSyncMidiNoteMessage,
  SonareEngineSyncMidiPanicMessage,
  SonareEngineSyncMixerMessage,
  SonareEngineSyncSf2InstrumentMessage,
  SonareEngineSyncSynthInstrumentMessage,
  SonareEngineSyncTempoMessage,
  SonareEngineSyncTrackStripEqBandMessage,
  SonareEngineSyncTrackStripInsertBypassedMessage,
  SonareEngineTransportFacade,
  SonareEngineTransportRequestMessage,
  SonareEngineTransportResponseMessage,
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareRealtimeVoiceChangerDestroyMessage,
  SonareRealtimeVoiceChangerMessage,
  SonareRealtimeVoiceChangerResetMessage,
  SonareRealtimeVoiceChangerSetConfigMessage,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  SonareWorkletDestroyMessage,
  SonareWorkletMessage,
  SonareWorkletProcessorOptions,
  SonareWorkletScheduleInsertAutomationMessage,
  SonareWorkletSetMeterIntervalMessage,
  SonareWorkletTransportMessage,
} from './worklet/messages';
export {
  registerSonareWorkletProcessor,
  SonareWorkletProcessor,
} from './worklet/mixer-processor';
export {
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  createSonareSpectrumRingBuffer,
  decodeFrame,
  encodeFrameHi,
  encodeFrameLo,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  readSonareSpectrumRingBuffer,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  type SonareClipPageRequest,
  type SonareClipPageRequestRingBuffer,
  type SonareClipPageRequestRingReadResult,
  type SonareEngineCommandRecord,
  type SonareEngineCommandRingBuffer,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  type SonareEngineTelemetryRecord,
  type SonareEngineTelemetryRingBuffer,
  type SonareEngineTelemetryRingReadResult,
  SonareEngineTelemetryType,
  type SonareMeterRingBuffer,
  type SonareMeterRingReadResult,
  type SonareScopeRingBuffer,
  type SonareScopeRingReadResult,
  type SonareSpectrumRingBuffer,
  type SonareSpectrumRingReadResult,
  type SonareWorkletMeterSnapshot,
  type SonareWorkletScopeSnapshot,
  type SonareWorkletSpectrumSnapshot,
  sonareClipPageRequestRingBufferByteLength,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  sonareScopeRingBufferByteLength,
  sonareSpectrumRingBufferByteLength,
  writeSonareEngineTelemetryRingBuffer,
} from './worklet/protocol';
export {
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
} from './worklet/voice-changer-processor';
