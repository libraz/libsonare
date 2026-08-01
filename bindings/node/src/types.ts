export * from './types_analysis.js';
export * from './types_capabilities.js';
export * from './types_engine.js';
export * from './types_features.js';
export * from './types_mastering.js';
export * from './types_project.js';

/**
 * Synchronous progress callback for cancellable offline operations.
 *
 * Return exactly `false` to request cooperative cancellation after the current
 * progress-report boundary. Returning `undefined` (the usual `void` callback
 * behavior) continues processing.
 */
export type ProgressCallback = (progress: number, stage: string) => boolean | undefined;
