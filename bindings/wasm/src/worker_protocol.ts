/**
 * Internal structured-clone protocol for the offline Web Worker bridge.
 *
 * Keep this deliberately limited to one-shot operations. Embind handles own
 * native resources and cannot safely cross a worker boundary.
 */

export type OfflineWorkerOperation =
  | 'analyze'
  | 'detectBpm'
  | 'detectKey'
  | 'detectChords'
  | 'masterAudio'
  | 'masterAudioStereo';

export interface OfflineWorkerRunMessage {
  type: 'sonare:offline-run';
  id: number;
  operation: OfflineWorkerOperation;
  request: Record<string, unknown>;
  /** A shared flag lets cancellation reach synchronous WASM progress callbacks. */
  cancelBuffer?: SharedArrayBuffer;
}

export interface OfflineWorkerCancelMessage {
  type: 'sonare:offline-cancel';
  id: number;
}

export type OfflineWorkerRequestMessage = OfflineWorkerRunMessage | OfflineWorkerCancelMessage;

export interface OfflineWorkerProgressMessage {
  type: 'sonare:offline-progress';
  id: number;
  progress: number;
  stage: string;
}

export interface OfflineWorkerResultMessage {
  type: 'sonare:offline-result';
  id: number;
  result: unknown;
}

export interface OfflineWorkerErrorMessage {
  type: 'sonare:offline-error';
  id: number;
  error: {
    name: string;
    message: string;
    code?: number;
    codeName?: string;
  };
}

export type OfflineWorkerResponseMessage =
  | OfflineWorkerProgressMessage
  | OfflineWorkerResultMessage
  | OfflineWorkerErrorMessage;
