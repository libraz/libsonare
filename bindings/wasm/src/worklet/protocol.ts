const ENGINE_MIXER_TARGET_BASE = 0x4d580000;
export const ENGINE_MIXER_PARAM_FADER_DB = 1;
export const ENGINE_MIXER_PARAM_PAN = 2;

export function engineMixerMasterTarget(paramKind: number): number {
  return ENGINE_MIXER_TARGET_BASE | (0xff << 8) | (paramKind & 0xff);
}

export type {
  SharedClipPageRequestRingWriter,
  SonareClipPageRequest,
  SonareClipPageRequestRingBuffer,
  SonareClipPageRequestRingReadResult,
} from './clip-page-ring.js';
export {
  clipPageRequestRingFromSharedBuffer,
  createSonareClipPageRequestRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  readSonareClipPageRequestRingBuffer,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  sonareClipPageRequestRingBufferByteLength,
} from './clip-page-ring.js';
export type {
  SonareEngineCommandRecord,
  SonareEngineCommandRingBuffer,
  SonareEngineTelemetryRecord,
  SonareEngineTelemetryRingBuffer,
  SonareEngineTelemetryRingReadResult,
} from './engine-rings.js';
export {
  commitSonareEngineTelemetryRingWrite,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  decodeFrame,
  encodeFrameHi,
  encodeFrameLo,
  engineRingFromSharedBuffer,
  engineTelemetryRingFromSharedBuffer,
  pendingSonareEngineCommandCount,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SONARE_ENGINE_RING_HEADER_INTS,
  SONARE_ENGINE_TELEMETRY_RECORD_BYTES,
  SONARE_ENGINE_TELEMETRY_RING_PROTOCOL_VERSION,
  SONARE_MAX_PUBLIC_PPQ,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineCommandRingHasRoom,
  sonareEngineTelemetryRingBufferByteLength,
  telemetryFromEngine,
  writeSonareEngineTelemetryRingBuffer,
} from './engine-rings.js';
export type {
  SharedExternalMidiRingWriter,
  SonareExternalMidiRingBuffer,
  SonareExternalMidiRingEvent,
  SonareExternalMidiRingReadResult,
} from './external-midi-ring.js';
export {
  createSonareExternalMidiRingBuffer,
  externalMidiRingFromSharedBuffer,
  pushSonareExternalMidiRingBuffer,
  readSonareExternalMidiRingBuffer,
  SONARE_EXTERNAL_MIDI_RING_HEADER_INTS,
  SONARE_EXTERNAL_MIDI_RING_RECORD_UINT32S,
  sonareExternalMidiRingBufferByteLength,
  sonareExternalMidiRingHasRoom,
} from './external-midi-ring.js';
export type {
  SharedMeterRingWriter,
  SharedSpectrumRingWriter,
  SonareMeterRingBuffer,
  SonareMeterRingReadResult,
  SonareSpectrumRingBuffer,
  SonareSpectrumRingReadResult,
  SonareWorkletMeterSnapshot,
  SonareWorkletSpectrumSnapshot,
} from './meter-rings.js';
export {
  createSonareMeterRingBuffer,
  createSonareSpectrumRingBuffer,
  magnitudeToDb,
  meterFromEngine,
  meterRingFromSharedBuffer,
  readSonareMeterRingBuffer,
  readSonareSpectrumRingBuffer,
  SONARE_FLOOR_DB,
  SONARE_METER_RING_CHANNEL_COUNT_OFFSET,
  SONARE_METER_RING_HEADER_INTS,
  SONARE_METER_RING_MAX_CHANNELS,
  SONARE_METER_RING_PLANES_OFFSET,
  SONARE_METER_RING_PROTOCOL_VERSION,
  SONARE_METER_RING_RECORD_FLOATS,
  SONARE_SPECTRUM_RING_HEADER_INTS,
  SONARE_SPECTRUM_RING_PROTOCOL_VERSION,
  sonareMeterRingBufferByteLength,
  sonareSpectrumRingBufferByteLength,
  spectrumRingFromSharedBuffer,
  toDb,
} from './meter-rings.js';
export {
  commitOverwriteWrite,
  isRecord,
  isUint32Slot,
  loadSonareRingCursor,
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
  SharedScopeRingWriter,
  SonareScopeRingBuffer,
  SonareScopeRingReadResult,
  SonareWorkletScopeSnapshot,
} from './scope-ring.js';
export {
  createSonareScopeRingBuffer,
  readSonareScopeRingBuffer,
  SONARE_SCOPE_DEFAULT_BANDS,
  SONARE_SCOPE_DEFAULT_MAX_POINTS,
  SONARE_SCOPE_MAX_BANDS,
  SONARE_SCOPE_MAX_POINTS,
  SONARE_SCOPE_RING_HEADER_INTS,
  SONARE_SCOPE_RING_PROTOCOL_VERSION,
  SONARE_SCOPE_RING_RECORD_PREFIX_FLOATS,
  scopeRingFromSharedBuffer,
  sonareScopeRingBufferByteLength,
  sonareScopeRingRecordFloats,
} from './scope-ring.js';
