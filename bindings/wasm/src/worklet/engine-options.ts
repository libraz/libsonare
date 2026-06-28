import type { RealtimeEngine } from '../index';
import type { SonareRealtimeEngineNodeOptions } from './messages';

export interface SonareEngineOptions extends SonareRealtimeEngineNodeOptions {
  offlineEngine?: RealtimeEngine;
  offlineBlockSize?: number;
  offlineChannelCount?: number;
}

export type SuspendableAudioContext = BaseAudioContext & {
  suspend?: () => Promise<void>;
  resume?: () => Promise<void>;
};
