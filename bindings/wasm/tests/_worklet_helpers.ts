import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePresetJson } from '../dist/index.js';
import {
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareExternalMidiRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  createSonareSpectrumRingBuffer,
  init as initWorklet,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareEngineCommandRingBuffer,
  pushSonareExternalMidiRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareExternalMidiRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareWorkletProcessor,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
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
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareWorkletProcessor,
  StreamingMasteringChain,
  sonareClipPageRequestRingBufferByteLength,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareExternalMidiRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  sonareScopeRingBufferByteLength,
  sonareSpectrumRingBufferByteLength,
  writeSonareEngineTelemetryRingBuffer,
} from '../dist/worklet.js';
import {
  commitOverwriteWrite,
  commitSonareEngineTelemetryRingWrite,
  engineTelemetryRingFromSharedBuffer,
  meterRingFromSharedBuffer,
  releaseOverwriteRead,
  scopeRingFromSharedBuffer,
  sonareScopeRingRecordFloats,
  spectrumRingFromSharedBuffer,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
} from '../src/worklet/protocol.js';

export {
  commitOverwriteWrite,
  commitSonareEngineTelemetryRingWrite,
  createSonareClipPageRequestRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSonareEngineTelemetryRingBuffer,
  createSonareExternalMidiRingBuffer,
  createSonareMeterRingBuffer,
  createSonareScopeRingBuffer,
  createSonareSpectrumRingBuffer,
  describe,
  engineTelemetryRingFromSharedBuffer,
  expect,
  it,
  Mixer,
  meterRingFromSharedBuffer,
  mixingScenePresetJson,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareEngineCommandRingBuffer,
  pushSonareExternalMidiRingBuffer,
  readSonareClipPageRequestRingBuffer,
  readSonareEngineTelemetryRingBuffer,
  readSonareExternalMidiRingBuffer,
  readSonareMeterRingBuffer,
  readSonareScopeRingBuffer,
  readSonareSpectrumRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareWorkletProcessor,
  releaseOverwriteRead,
  SONARE_CLIP_PAGE_REQUEST_RING_HEADER_INTS,
  SONARE_CLIP_PAGE_REQUEST_RING_RECORD_UINT32S,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
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
  SonareEngine,
  SonareEngineCommandType,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareWorkletProcessor,
  StreamingMasteringChain,
  scopeRingFromSharedBuffer,
  sonareClipPageRequestRingBufferByteLength,
  sonareEngineCommandRingBufferByteLength,
  sonareEngineTelemetryRingBufferByteLength,
  sonareExternalMidiRingBufferByteLength,
  sonareMeterRingBufferByteLength,
  sonareScopeRingBufferByteLength,
  sonareScopeRingRecordFloats,
  sonareSpectrumRingBufferByteLength,
  spectrumRingFromSharedBuffer,
  tryAcquireOverwriteRead,
  tryBeginOverwriteWrite,
  writeSonareEngineTelemetryRingBuffer,
};

export function setupWorklet(): void {
  beforeAll(async () => {
    // index.js and worklet.js are separate self-contained bundles (code-splitting
    // is disabled), so each owns its module singleton and must be initialized.
    await init();
    await initWorklet();
  });
}

/**
 * Runs `body` and returns how many `Float32Array` views `subarray` created
 * meanwhile. A full render quantum should create none.
 */
export function countFloat32Subarrays(body: () => void): number {
  const typedArrayPrototype = Object.getPrototypeOf(Float32Array.prototype) as {
    subarray: (this: unknown, ...args: unknown[]) => unknown;
  };
  const original = typedArrayPrototype.subarray;
  let count = 0;
  typedArrayPrototype.subarray = function (this: unknown, ...args: unknown[]) {
    if (this instanceof Float32Array) {
      count++;
    }
    return original.apply(this, args);
  };
  try {
    body();
  } finally {
    typedArrayPrototype.subarray = original;
  }
  return count;
}
