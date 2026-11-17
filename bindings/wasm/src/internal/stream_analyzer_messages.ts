/** Wire protocol between the stream-analyzer AudioWorklet processor and its node. */

/** Returns a consumed chunk buffer to the processor's pool. */
export interface SonareStreamAnalyzerRecycleMessage {
  type: 'recycle';
  samples: Float32Array;
}

export interface SonareStreamAnalyzerDestroyMessage {
  type: 'destroy';
}

export type SonareStreamAnalyzerMessage =
  | SonareStreamAnalyzerRecycleMessage
  | SonareStreamAnalyzerDestroyMessage;

/** One chunk of the mono downmix, transferred from the processor. */
export interface SonareStreamAnalyzerChunkMessage {
  type: 'chunk';
  samples: Float32Array;
  /** Samples the processor has produced before this chunk, dropped chunks included. */
  startSample: number;
  /** True when audio before this chunk was lost, so it does not continue the previous one. */
  discontinuity: boolean;
}
