// Public entry for the AudioWorklet surface. The implementation is split into
// focused modules under ./worklet/*; tsup bundles them back into a single
// self-contained dist/worklet.js (code-splitting is disabled because a real
// AudioWorkletGlobalScope cannot resolve sibling chunks), so the public surface
// is unchanged.

import './lifetime.js';

export type { OpfsClipStream, OpfsClipStreamOptions } from './clip_page_streamer.js';
export { attachOpfsClipStream } from './clip_page_streamer.js';
// With code-splitting disabled, the worklet bundle carries its own copy of the
// module singleton. Re-export the lifecycle so that realm can initialize its
// own wasm instance, independent of the main-thread `index` module.
export { init, isInitialized } from './index.js';
export { opfsClipPageWorkerSource } from './opfs_clip_pages.js';
// Host-side mastering preview inside the worklet realm. Kept here rather than
// left main-thread-only so a live preview does not have to round-trip audio to
// the main thread; see the class doc for the prepare/loudness/latency contract.
export type { StreamingMasteringChainConfig } from './public_types.js';
export type { CaptureOptions } from './public_types_engine.js';
export type {
  EngineClipPageRequestMessage,
  EngineSyncErrorMessage,
  PlaybackNodeOptions,
  SonarePlaybackWorkletProcessorDiagnostics,
  SonarePlaybackWorkletProcessorOptions,
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  SonareStreamAnalyzerNodeOptions,
  SonareStreamAnalyzerWorkletProcessorOptions,
  SonareWorkletProcessorOptions,
  WorkletPort,
  WorkletTransport,
} from './public_types_worklet.js';
export { type StereoBlock, StreamingMasteringChain } from './streaming_processors.js';
export { SonareEngine } from './worklet/engine.js';
export type { SonareEngineRenderOfflineRequest } from './worklet/engine-export.js';
export { SonareRealtimeEngineNode } from './worklet/engine-node.js';
export type { SonareEngineTransportFacade } from './worklet/engine-offline.js';
export type { SonareEngineOptions } from './worklet/engine-options.js';
export { SonareRealtimeEngineWorkletProcessor } from './worklet/engine-processor.js';
export { registerSonareRealtimeEngineWorkletProcessor } from './worklet/engine-register.js';
export {
  registerSonareWorkletProcessor,
  SonareWorkletProcessor,
} from './worklet/mixer-processor.js';
export {
  createPlaybackNode,
  registerSonarePlaybackWorkletProcessor,
  SonarePlaybackWorkletProcessor,
} from './worklet/playback-processor.js';
export {
  type ClipPagePageRequest,
  type ClipPageRequestRingBuffer,
  type ClipPageRequestRingReadResult,
  clipPageRequestRingBufferByteLength,
  createClipPageRequestRingBuffer,
  createEngineTelemetryRingBuffer,
  createExternalMidiRingBuffer,
  createMeterRingBuffer,
  createScopeRingBuffer,
  createSpectrumRingBuffer,
  type EngineTelemetryRecord,
  type EngineTelemetryRingBuffer,
  type EngineTelemetryRingReadResult,
  type ExternalMidiRingBuffer,
  type ExternalMidiRingReadResult,
  engineTelemetryRingBufferByteLength,
  externalMidiRingBufferByteLength,
  type MeterRingBuffer,
  type MeterRingReadResult,
  meterRingBufferByteLength,
  type RingCapacityOptions,
  readClipPageRequestRingBuffer,
  readEngineTelemetryRingBuffer,
  readExternalMidiRingBuffer,
  readMeterRingBuffer,
  readScopeRingBuffer,
  readSpectrumRingBuffer,
  type ScopeRingBuffer,
  type ScopeRingOptions,
  type ScopeRingReadResult,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
  SONARE_EXTERNAL_MIDI_RING_HEADER_INTS,
  SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SCOPE_RING_PROTOCOL_VERSION,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_PROTOCOL_VERSION,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  type SpectrumRingBuffer,
  type SpectrumRingOptions,
  type SpectrumRingReadResult,
  scopeRingBufferByteLength,
  spectrumRingBufferByteLength,
  type WorkletMeterSnapshot,
  type WorkletScopeSnapshot,
  type WorkletSpectrumSnapshot,
} from './worklet/protocol.js';
export { SonareStreamAnalyzerNode } from './worklet/stream-analyzer-node.js';
export {
  registerSonareStreamAnalyzerWorkletProcessor,
  SonareStreamAnalyzerWorkletProcessor,
} from './worklet/stream-analyzer-processor.js';
export {
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
} from './worklet/voice-changer-processor.js';
