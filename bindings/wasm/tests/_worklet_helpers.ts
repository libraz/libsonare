import { beforeAll, describe, expect, it } from 'vitest';
import { init, Mixer, mixingScenePreset } from '../dist/index.js';
import {
  clipPageRequestRingBufferByteLength,
  createClipPageRequestRingBuffer,
  createEngineTelemetryRingBuffer,
  createExternalMidiRingBuffer,
  createMeterRingBuffer,
  createScopeRingBuffer,
  createSpectrumRingBuffer,
  engineTelemetryRingBufferByteLength,
  externalMidiRingBufferByteLength,
  init as initWorklet,
  meterRingBufferByteLength,
  readClipPageRequestRingBuffer,
  readEngineTelemetryRingBuffer,
  readExternalMidiRingBuffer,
  readMeterRingBuffer,
  readScopeRingBuffer,
  readSpectrumRingBuffer,
  registerSonareRealtimeEngineWorkletProcessor,
  registerSonareRealtimeVoiceChangerWorkletProcessor,
  registerSonareWorkletProcessor,
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
  SonareEngine,
  SonareEngineTelemetryError,
  SonareEngineTelemetryType,
  SonareRealtimeEngineNode,
  SonareRealtimeEngineWorkletProcessor,
  SonareRealtimeVoiceChangerWorkletProcessor,
  SonareWorkletProcessor,
  StreamingMasteringChain,
  scopeRingBufferByteLength,
  spectrumRingBufferByteLength,
} from '../dist/worklet.js';
import {
  createSonareEngineCommandRingBuffer,
  popSonareEngineCommandRingBuffer,
  pushSonareEngineCommandRingBuffer,
  SONARE_ENGINE_COMMAND_RECORD_BYTES,
  SonareEngineCommandType,
  sonareEngineCommandRingBufferByteLength,
} from '../src/internal/engine_command_ring.js';
import { pushSonareClipPageRequestRingBuffer } from '../src/worklet/clip-page-ring.js';
import { pushSonareExternalMidiRingBuffer } from '../src/worklet/external-midi-ring.js';
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
  writeSonareEngineTelemetryRingBuffer,
} from '../src/worklet/protocol.js';

export {
  clipPageRequestRingBufferByteLength,
  commitOverwriteWrite,
  commitSonareEngineTelemetryRingWrite,
  createClipPageRequestRingBuffer,
  createEngineTelemetryRingBuffer,
  createExternalMidiRingBuffer,
  createMeterRingBuffer,
  createScopeRingBuffer,
  createSonareEngineCommandRingBuffer,
  createSpectrumRingBuffer,
  describe,
  engineTelemetryRingBufferByteLength,
  engineTelemetryRingFromSharedBuffer,
  expect,
  externalMidiRingBufferByteLength,
  it,
  Mixer,
  meterRingBufferByteLength,
  meterRingFromSharedBuffer,
  mixingScenePreset,
  popSonareEngineCommandRingBuffer,
  pushSonareClipPageRequestRingBuffer,
  pushSonareEngineCommandRingBuffer,
  pushSonareExternalMidiRingBuffer,
  readClipPageRequestRingBuffer,
  readEngineTelemetryRingBuffer,
  readExternalMidiRingBuffer,
  readMeterRingBuffer,
  readScopeRingBuffer,
  readSpectrumRingBuffer,
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
  scopeRingBufferByteLength,
  scopeRingFromSharedBuffer,
  sonareEngineCommandRingBufferByteLength,
  sonareScopeRingRecordFloats,
  spectrumRingBufferByteLength,
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
