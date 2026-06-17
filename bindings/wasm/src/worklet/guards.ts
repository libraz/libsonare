import type {
  SonareEngineCaptureRequestMessage,
  SonareEngineCaptureResponseMessage,
  SonareEngineSyncMessage,
  SonareEngineTransportRequestMessage,
  SonareEngineTransportResponseMessage,
  SonareRealtimeVoiceChangerMessage,
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

export function isEngineSyncMessage(value: unknown): value is SonareEngineSyncMessage {
  if (!isRecord(value) || typeof value.type !== 'string') {
    return false;
  }
  return (
    value.type === 'syncClips' ||
    value.type === 'syncClipsDelta' ||
    value.type === 'syncMidiClips' ||
    value.type === 'syncMarkers' ||
    value.type === 'syncMetronome' ||
    value.type === 'syncAutomation' ||
    value.type === 'syncTempo' ||
    value.type === 'syncMixer' ||
    value.type === 'syncCapture' ||
    value.type === 'syncTrackStripEqBand' ||
    value.type === 'syncMasterStripEqBand' ||
    value.type === 'syncTrackStripInsertBypassed' ||
    value.type === 'syncMasterStripInsertBypassed' ||
    value.type === 'syncTrackStripInsertParamByName' ||
    value.type === 'syncMasterStripInsertParamByName' ||
    value.type === 'syncTrackStripPan' ||
    value.type === 'syncTrackStripPanLaw' ||
    value.type === 'syncTrackStripPanMode' ||
    value.type === 'syncTrackStripDualPan' ||
    value.type === 'syncTrackStripChannelDelaySamples' ||
    value.type === 'syncBuiltinInstrument' ||
    value.type === 'syncSynthInstrument' ||
    value.type === 'syncSf2Instrument' ||
    value.type === 'syncLoadSoundFont' ||
    value.type === 'syncMidiNoteOn' ||
    value.type === 'syncMidiNoteOff' ||
    value.type === 'syncMidiCc' ||
    value.type === 'syncMidiPanic'
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

export function isEngineCaptureResponseMessage(
  value: unknown,
): value is SonareEngineCaptureResponseMessage {
  return (
    isRecord(value) &&
    value.type === 'captureResponse' &&
    typeof value.requestId === 'number' &&
    typeof value.ok === 'boolean'
  );
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
