const ENGINE_MIXER_TARGET_BASE = 0x4d580000;
export const ENGINE_MIXER_PARAM_FADER_DB = 1;
export const ENGINE_MIXER_PARAM_PAN = 2;

export function engineMixerMasterTarget(paramKind: number): number {
  return ENGINE_MIXER_TARGET_BASE | (0xff << 8) | (paramKind & 0xff);
}

export type {
  SonareEngineCommandRecord,
  SonareEngineCommandRingBuffer,
} from '../internal/engine_command_ring.js';
export {
  createSonareEngineCommandRingBuffer,
  pendingSonareEngineCommandCount,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SonareEngineCommandType,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineCommandRingHasRoom,
} from '../internal/engine_command_ring.js';
export type {
  ClipPagePageRequest,
  ClipPageRequestRingBuffer,
  ClipPageRequestRingReadResult,
  SharedClipPageRequestRingWriter,
} from './clip-page-ring.js';
export {
  clipPageRequestRingBufferByteLength,
  clipPageRequestRingFromSharedBuffer,
  createClipPageRequestRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  readClipPageRequestRingBuffer,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
} from './clip-page-ring.js';
export type {
  EngineTelemetryRecord,
  EngineTelemetryRingBuffer,
  EngineTelemetryRingReadResult,
} from './engine-rings.js';
export {
  commitSonareEngineTelemetryRingWrite,
  createEngineTelemetryRingBuffer,
  decodeFrame,
  encodeFrameHi,
  encodeFrameLo,
  engineRingFromSharedBuffer,
  engineTelemetryRingBufferByteLength,
  engineTelemetryRingFromSharedBuffer,
  readEngineTelemetryRingBuffer,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
  SONARE_MAX_PUBLIC_PPQ,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  telemetryFromEngine,
  writeSonareEngineTelemetryRingBuffer,
} from './engine-rings.js';
export type {
  ExternalMidiRingBuffer,
  ExternalMidiRingReadResult,
  SharedExternalMidiRingWriter,
  SonareExternalMidiRingEvent,
} from './external-midi-ring.js';
export {
  createExternalMidiRingBuffer,
  externalMidiRingBufferByteLength,
  externalMidiRingFromSharedBuffer,
  pushSonareExternalMidiRingBuffer,
  readExternalMidiRingBuffer,
  SONARE_EXTERNAL_MIDI_RING_HEADER_INTS,
  SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S,
  sonareExternalMidiRingHasRoom,
} from './external-midi-ring.js';
export type {
  MeterRingBuffer,
  MeterRingReadResult,
  SharedMeterRingWriter,
  SharedSpectrumRingWriter,
  SpectrumRingBuffer,
  SpectrumRingOptions,
  SpectrumRingReadResult,
  WorkletMeterSnapshot,
  WorkletSpectrumSnapshot,
} from './meter-rings.js';
export {
  createMeterRingBuffer,
  createSpectrumRingBuffer,
  magnitudeToDb,
  meterFromEngine,
  meterRingBufferByteLength,
  meterRingFromSharedBuffer,
  readMeterRingBuffer,
  readSpectrumRingBuffer,
  SONARE_FLOOR_DB,
  SONARE_METER_RING_CHANNEL_COUNT_OFFSET,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_MAX_CHANNELS,
  SONARE_METER_RING_PLANES_OFFSET,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_PROTOCOL_VERSION,
  spectrumRingBufferByteLength,
  spectrumRingFromSharedBuffer,
} from './meter-rings.js';
export {
  commitOverwriteWrite,
  isRecord,
  isUint32Slot,
  loadSonareRingCursor,
  type RingCapacityOptions,
  readInt64Words,
  releaseOverwriteRead,
  sonareRingCursorDistance,
  sonareRingCursorSlot,
  storeSonareRingCursor,
  toSafeInteger,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
  writeInt64Words,
  writePreparedInt64Words,
} from './ring-core.js';
export type {
  ScopeRingBuffer,
  ScopeRingOptions,
  ScopeRingReadResult,
  SharedScopeRingWriter,
  WorkletScopeSnapshot,
} from './scope-ring.js';
export {
  createScopeRingBuffer,
  readScopeRingBuffer,
  SONARE_SCOPE_DEFAULT_BANDS,
  SONARE_SCOPE_DEFAULT_MAX_POINTS,
  SONARE_SCOPE_MAX_BANDS,
  SONARE_SCOPE_MAX_POINTS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SCOPE_RING_PROTOCOL_VERSION,
  SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS,
  scopeRingBufferByteLength,
  scopeRingFromSharedBuffer,
  sonareScopeRingRecordFloats,
} from './scope-ring.js';
