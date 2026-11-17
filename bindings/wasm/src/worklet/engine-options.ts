import type { RealtimeEngine } from '../index.js';
import type { SonareRealtimeEngineNodeOptions } from '../public_types_worklet.js';

/** Options of {@link SonareEngine.create}, on top of the node options. */
export interface SonareEngineOptions extends SonareRealtimeEngineNodeOptions {
  /** Offline mirror to use instead of one created at the context rate. */
  offlineEngine?: RealtimeEngine;
  /** Offline render block size in frames. Default: `maxBlockSize`, else `128`. */
  offlineBlockSize?: number;
  /** Planes the offline mirror renders and captures. Default: `numChannels`, else `2`. */
  offlineNumChannels?: number;
}

export type SuspendableAudioContext = BaseAudioContext & {
  suspend?: () => Promise<void>;
  resume?: () => Promise<void>;
};
