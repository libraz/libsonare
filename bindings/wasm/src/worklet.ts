/**
 * AudioWorklet entry: the processors, their nodes and rings, and the classes designed to run on
 * the audio thread.
 *
 * This entry is bundled self-contained, so its classes are not the root entry's: an object built
 * from one entry is not an `instanceof` the other entry's class. Pass plain data between realms.
 *
 * Inside an AudioWorkletGlobalScope, `init()` without a `moduleFactory` is refused with
 * `NotSupported`, because the realm cannot fetch. The factory is the default export of
 * `@libraz/libsonare/module`: pass it as `init({ moduleFactory, wasmBinary })`, or, for a
 * processor registered from this entry, set it as `globalThis.SonareEmbindModuleFactory` and
 * pass `wasmBinary` in the processor's `processorOptions`.
 */

// tsup bundles ./worklet/* into one dist/worklet.js; a real AudioWorkletGlobalScope cannot
// resolve sibling chunks, so code-splitting is disabled.

import './lifetime.js';

export type { OpfsClipStream, OpfsClipStreamOptions } from './clip_page_streamer.js';
export { attachOpfsClipStream } from './clip_page_streamer.js';
// One module per realm, shared by every entry point of this build: on the main
// thread `init()` through this entry or `index` loads the same instance, and the
// AudioWorklet realm initialises its own.
export { init, isInitialized } from './index.js';
export type { MixerConfig, MixerRealtimeBuffer } from './mixer.js';
export { Mixer } from './mixer.js';
export { opfsClipPageWorkerSource } from './opfs_clip_pages.js';
// Host-side mastering preview inside the worklet realm. Kept here rather than
// left main-thread-only so a live preview does not have to round-trip audio to
// the main thread; see the class doc for the prepare/loudness/latency contract.
export type { ModuleFactory, StreamingMasteringChainConfig } from './public_types.js';
export type { CaptureOptions } from './public_types_engine.js';
export type { MixerProcessResult, StripMeteringOptions } from './public_types_mixing.js';
export type {
  EqBandInput,
  EqMatchOptions,
  EqSpectrumSnapshot,
  EqStereoPlacement,
  StreamingEqualizerConfig,
  StreamingRetuneAppliedConfig,
  StreamingRetuneConfig,
} from './public_types_realtime.js';
export type {
  EngineClipPageRequestMessage,
  EngineSyncErrorMessage,
  SonarePlaybackNodeOptions,
  SonarePlaybackWorkletProcessorConstructorOptions,
  SonarePlaybackWorkletProcessorDiagnostics,
  SonarePlaybackWorkletProcessorOptions,
  SonareRealtimeEngineNodeCapabilities,
  SonareRealtimeEngineNodeOptions,
  SonareRealtimeEngineWorkletProcessorConstructorOptions,
  SonareRealtimeEngineWorkletProcessorOptions,
  SonareRealtimeVoiceChangerWorkletProcessorOptions,
  SonareStreamAnalyzerNodeOptions,
  SonareStreamAnalyzerWorkletProcessorConstructorOptions,
  SonareStreamAnalyzerWorkletProcessorOptions,
  SonareWorkletProcessorConstructorOptions,
  SonareWorkletProcessorOptions,
  WorkletPort,
  WorkletTransport,
} from './public_types_worklet.js';
export type {
  RealtimeVoiceChangerInterleavedBuffer,
  RealtimeVoiceChangerMonoBuffer,
  RealtimeVoiceChangerOptions,
  RealtimeVoiceChangerPlanarBuffer,
} from './realtime_voice_changer.js';
export { RealtimeVoiceChanger } from './realtime_voice_changer.js';
export { StreamAnalyzer } from './stream_analyzer.js';
export type {
  StreamBarChord,
  StreamChordChange,
  StreamConfig,
  StreamFrames,
  StreamFramesI16,
  StreamFramesU8,
  StreamPatternScore,
  StreamProgressiveEstimate,
  StreamQuantizeConfig,
  StreamStats,
} from './stream_types.js';
export {
  type EqPhaseMode,
  type StereoBlock,
  StreamingEqualizer,
  StreamingMasteringChain,
  StreamingRetune,
} from './streaming_processors.js';
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
export { SonarePlaybackNode } from './worklet/playback-node.js';
export {
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
