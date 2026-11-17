/** Wire protocol between the voice-changer AudioWorklet processor and its node. */
import type { RealtimeVoiceChangerConfig } from '../public_types.js';
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

export type SonareRealtimeVoiceChangerMessage =
  | SonareRealtimeVoiceChangerSetConfigMessage
  | SonareRealtimeVoiceChangerResetMessage
  | SonareRealtimeVoiceChangerDestroyMessage
  | SonareRealtimeVoiceChangerNoteOnMessage
  | SonareRealtimeVoiceChangerNoteOffMessage
  | SonareRealtimeVoiceChangerAllNotesOffMessage;

/** Posted when a control message or the processor's construction is refused. */
export interface SonareRealtimeVoiceChangerErrorMessage {
  type: 'error';
  request: SonareRealtimeVoiceChangerMessage['type'] | 'init' | 'invalid';
  message: string;
  failure: SonareFailurePayload;
}
