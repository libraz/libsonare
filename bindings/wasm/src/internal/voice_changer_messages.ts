/** Wire protocol between the voice-changer AudioWorklet processor and its node. */
import type { RealtimeVoiceChangerConfig } from '../public_types.js';
import type { SonareRealtimeVoiceChangerNodeStatusReplyMessage } from '../public_types_worklet.js';
import type { SonareFailurePayload } from './failure_payload.js';

export interface SonareRealtimeVoiceChangerSetConfigMessage {
  type: 'setConfig';
  /** The complete flat config; a partial object is refused. */
  config: RealtimeVoiceChangerConfig;
}

export interface SonareRealtimeVoiceChangerResetMessage {
  type: 'reset';
}

export interface SonareRealtimeVoiceChangerDestroyMessage {
  type: 'destroy';
}

/** Holds a MIDI note. `velocity` is in (0, 1] and defaults to 1. */
export interface SonareRealtimeVoiceChangerNoteOnMessage {
  type: 'noteOn';
  note: number;
  velocity?: number;
}

export interface SonareRealtimeVoiceChangerNoteOffMessage {
  type: 'noteOff';
  note: number;
}

export interface SonareRealtimeVoiceChangerAllNotesOffMessage {
  type: 'allNotesOff';
}

/** Asks for a {@link SonareRealtimeVoiceChangerNodeStatusReplyMessage}. */
export interface SonareRealtimeVoiceChangerStatusMessage {
  type: 'status';
}

export type SonareRealtimeVoiceChangerMessage =
  | SonareRealtimeVoiceChangerStatusMessage
  | SonareRealtimeVoiceChangerSetConfigMessage
  | SonareRealtimeVoiceChangerResetMessage
  | SonareRealtimeVoiceChangerDestroyMessage
  | SonareRealtimeVoiceChangerNoteOnMessage
  | SonareRealtimeVoiceChangerNoteOffMessage
  | SonareRealtimeVoiceChangerAllNotesOffMessage;

/** Posted once the changer exists, with the latency it was prepared with. */
export interface SonareRealtimeVoiceChangerReadyMessage {
  type: 'ready';
  runtimeTarget: 'embind';
  latencySamples: number;
}

/** Posted when a control message or the processor's construction is refused. */
export interface SonareRealtimeVoiceChangerErrorMessage {
  type: 'error';
  request: SonareRealtimeVoiceChangerMessage['type'] | 'init' | 'invalid' | 'process';
  message: string;
  failure: SonareFailurePayload;
}

export type { SonareRealtimeVoiceChangerNodeStatusReplyMessage };
