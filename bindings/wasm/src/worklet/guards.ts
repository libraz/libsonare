import type { EngineCaptureStatus } from '../index';
import type {
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessageInternal,
  SonareEngineClipPageRequestMessage,
  SonareEngineSyncMessage,
  SonareEngineTransportRequestMessage,
  SonareEngineTransportResponseMessage,
  SonareRealtimeVoiceChangerMessage,
  SonareWorkletExternalMidiMessage,
  SonareWorkletMessage,
} from './messages';
import {
  isRecord,
  type SonareEngineCommandRecord,
  type SonareEngineTelemetryRecord,
  type SonareWorkletMeterSnapshot,
} from './protocol';

export function isWorkletMessage(value: unknown): value is SonareWorkletMessage {
  if (!isRecord(value) || typeof value.type !== 'string') {
    return false;
  }
  return (
    value.type === 'scheduleInsertAutomation' ||
    value.type === 'setMeterInterval' ||
    value.type === 'destroy'
  );
}

export function isEngineCommandRecord(value: unknown): value is SonareEngineCommandRecord {
  return isRecord(value) && typeof value.type === 'number';
}

/**
 * Every discriminant the worklet accepts on the control plane, derived from the
 * `SonareEngineSyncMessage` union rather than restated as literals.
 *
 * The `Record<SonareEngineSyncMessage['type'], true>` annotation rejects a
 * missing key and the object literal's excess-property check rejects a stale
 * one, so a union member that never reaches this table is a compile error
 * instead of a message the port drops in silence.
 */
export const ENGINE_SYNC_MESSAGE_TYPES: Record<SonareEngineSyncMessage['type'], true> = {
  destroy: true,
  syncAutomation: true,
  syncBuiltinInstrument: true,
  syncBusStripInsertBypassed: true,
  syncBusStripInsertParamByName: true,
  syncCapture: true,
  syncClearMidiFx: true,
  syncClearMidiInputSource: true,
  syncClipPage: true,
  syncClipPageClear: true,
  syncClipPageCommit: true,
  syncClipPageDestroy: true,
  syncClipPagePrefetchFrames: true,
  syncClipPageProvider: true,
  syncClips: true,
  syncClipsDelta: true,
  syncExternalMidiClock: true,
  syncLoadSoundFont: true,
  syncMarkers: true,
  syncMasterStripEqBand: true,
  syncMasterStripInsertBypassed: true,
  syncMasterStripInsertParamByName: true,
  syncMetronome: true,
  syncMidiCc: true,
  syncMidiCcBinding: true,
  syncMidiClips: true,
  syncMidiDestinationExternal: true,
  syncMidiFx: true,
  syncMidiInputCc: true,
  syncMidiInputNoteOff: true,
  syncMidiInputNoteOn: true,
  syncMidiInputSource: true,
  syncMidiNoteOff: true,
  syncMidiNoteOn: true,
  syncMidiPanic: true,
  syncMidiSysex: true,
  syncMidiUmp: true,
  syncMixer: true,
  syncParameters: true,
  syncSf2Instrument: true,
  syncSynthInstrument: true,
  syncTempo: true,
  syncTrackStripChannelDelaySamples: true,
  syncTrackStripDualPan: true,
  syncTrackStripEqBand: true,
  syncTrackStripInsertBypassed: true,
  syncTrackStripInsertParamByName: true,
  syncTrackStripPan: true,
  syncTrackStripPanLaw: true,
  syncTrackStripPanMode: true,
};

// A Set rather than a property lookup on the table: an inherited member name
// ('constructor', 'toString') must not read as an accepted discriminant.
const engineSyncMessageTypes: ReadonlySet<string> = new Set(Object.keys(ENGINE_SYNC_MESSAGE_TYPES));

export function isEngineSyncMessage(value: unknown): value is SonareEngineSyncMessage {
  return (
    isRecord(value) && typeof value.type === 'string' && engineSyncMessageTypes.has(value.type)
  );
}

export function isEngineCaptureRequestMessage(
  value: unknown,
): value is SonareEngineCaptureRequestMessage {
  return (
    isRecord(value) &&
    value.type === 'captureRequest' &&
    typeof value.requestId === 'number' &&
    (value.op === 'status' || value.op === 'read' || value.op === 'reset')
  );
}

function isCaptureStatus(value: unknown): value is EngineCaptureStatus {
  return (
    isRecord(value) &&
    typeof value.capturedFrames === 'number' &&
    Number.isSafeInteger(value.capturedFrames) &&
    value.capturedFrames >= 0 &&
    typeof value.overflowCount === 'number' &&
    Number.isSafeInteger(value.overflowCount) &&
    value.overflowCount >= 0 &&
    typeof value.armed === 'boolean' &&
    typeof value.punchEnabled === 'boolean' &&
    (value.source === 'input' || value.source === 'output') &&
    typeof value.recordOffsetSamples === 'number' &&
    Number.isSafeInteger(value.recordOffsetSamples)
  );
}

function isCaptureChannel(value: unknown): value is Float32Array {
  // Capture responses are transferred out of the worklet. A typed view onto
  // the WASM heap or a SharedArrayBuffer must not cross that boundary: the
  // former can be detached by memory growth and the latter cannot be listed
  // in a transfer list. `instanceof` also rejects boxed number arrays.
  return (
    value instanceof Float32Array &&
    typeof ArrayBuffer !== 'undefined' &&
    value.buffer instanceof ArrayBuffer
  );
}

function isCaptureChannels(value: unknown): value is Float32Array[] {
  return Array.isArray(value) && value.every((channel) => isCaptureChannel(channel));
}

function hasOwn(value: Record<string, unknown>, key: string): boolean {
  // biome-ignore lint/suspicious/noPrototypeBuiltins: Object.hasOwn is newer than the ES2020 target.
  return Object.prototype.hasOwnProperty.call(value, key);
}

/** Return a request ID even when the response payload itself is malformed. */
export function engineCaptureResponseRequestId(value: unknown): number | undefined {
  if (
    !isRecord(value) ||
    value.type !== 'captureResponse' ||
    typeof value.requestId !== 'number' ||
    !Number.isSafeInteger(value.requestId)
  ) {
    return undefined;
  }
  return value.requestId;
}

export function isEngineCaptureResponseMessage(
  value: unknown,
): value is SonareEngineCaptureResponseMessageInternal {
  const requestId = engineCaptureResponseRequestId(value);
  if (requestId === undefined || !isRecord(value)) {
    return false;
  }
  if (value.ok === false) {
    return (
      typeof value.error === 'string' && !hasOwn(value, 'status') && !hasOwn(value, 'channels')
    );
  }
  if (value.ok !== true || hasOwn(value, 'error')) {
    return false;
  }
  const hasStatus = hasOwn(value, 'status');
  const hasChannels = hasOwn(value, 'channels');
  if (hasStatus && hasChannels) {
    return false;
  }
  if (hasStatus) {
    return isCaptureStatus(value.status);
  }
  if (hasChannels) {
    return isCaptureChannels(value.channels);
  }
  return true;
}

/**
 * Check that a strict success response belongs to the requested operation.
 * Failure responses are operation-independent and are therefore valid for
 * every pending request (the node rejects them after this check).
 */
export function isEngineCaptureResponseForOperation(
  response: SonareEngineCaptureResponseMessageInternal,
  op: SonareEngineCaptureRequestMessage['op'],
): boolean {
  if (!response.ok) {
    return true;
  }
  switch (op) {
    case 'status':
      return 'status' in response;
    case 'read':
      return 'channels' in response;
    case 'reset':
      return !('status' in response) && !('channels' in response);
  }
}

export function isEngineTransportRequestMessage(
  value: unknown,
): value is SonareEngineTransportRequestMessage {
  return (
    isRecord(value) &&
    value.type === 'transportRequest' &&
    typeof value.requestId === 'number' &&
    value.op === 'state'
  );
}

export function isEngineTransportResponseMessage(
  value: unknown,
): value is SonareEngineTransportResponseMessage {
  return (
    isRecord(value) &&
    value.type === 'transportResponse' &&
    typeof value.requestId === 'number' &&
    typeof value.ok === 'boolean'
  );
}

export function isRealtimeVoiceChangerMessage(
  value: unknown,
): value is SonareRealtimeVoiceChangerMessage {
  if (!isRecord(value) || typeof value.type !== 'string') {
    return false;
  }
  return value.type === 'setConfig' || value.type === 'reset' || value.type === 'destroy';
}

export function isEngineTelemetryRecord(value: unknown): value is SonareEngineTelemetryRecord {
  return (
    isRecord(value) &&
    typeof value.type === 'number' &&
    typeof value.error === 'number' &&
    typeof value.renderFrame === 'number' &&
    typeof value.timelineSample === 'number' &&
    typeof value.audibleTimelineSample === 'number' &&
    typeof value.graphLatencySamplesQ8 === 'number' &&
    typeof value.value === 'number'
  );
}

export function isExternalMidiBatchMessage(
  value: unknown,
): value is SonareWorkletExternalMidiMessage {
  return isRecord(value) && value.type === 'externalMidi' && Array.isArray(value.events);
}

export function isClipPageRequestMessage(
  value: unknown,
): value is SonareEngineClipPageRequestMessage {
  return (
    isRecord(value) &&
    value.type === 'clipPageRequest' &&
    Array.isArray(value.requests) &&
    value.requests.every(
      (request) =>
        isRecord(request) &&
        typeof request.clipId === 'number' &&
        typeof request.pageIndex === 'number',
    )
  );
}

export function isMeterSnapshot(value: unknown): value is SonareWorkletMeterSnapshot {
  return (
    isRecord(value) &&
    value.type === 'meter' &&
    typeof value.frame === 'number' &&
    typeof value.peakDbL === 'number' &&
    typeof value.peakDbR === 'number' &&
    typeof value.rmsDbL === 'number' &&
    typeof value.rmsDbR === 'number' &&
    typeof value.correlation === 'number' &&
    (typeof value.targetId === 'number' || value.targetId === undefined)
  );
}
