import type { SonareEngineCaptureResponseMessage } from '../../src/worklet.js';

const status = {
  capturedFrames: 128,
  overflowCount: 0,
  armed: true,
  punchEnabled: false,
  source: 'input' as const,
  recordOffsetSamples: -12,
};

// Keep these assignments deliberately structural: consumers of the public
// message type historically used any subset of the optional response fields.
const empty: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 1,
  ok: true,
};
const statusResponse: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 2,
  ok: true,
  status,
};
const typedChannels: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 3,
  ok: true,
  channels: [new Float32Array([0.25])],
};
const numberChannels: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 4,
  ok: true,
  channels: [[0.25]],
};
const legacyOk: boolean = true;
const legacyResponse: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 6,
  ok: legacyOk,
  status: undefined,
  channels: [[0.5]],
  error: undefined,
};
const errorResponse: SonareEngineCaptureResponseMessage = {
  type: 'captureResponse',
  requestId: 5,
  ok: false,
  error: 'capture failed',
};

void empty;
void statusResponse;
void typedChannels;
void numberChannels;
void legacyResponse;
void errorResponse;
