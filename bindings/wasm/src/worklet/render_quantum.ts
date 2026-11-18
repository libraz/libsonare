/**
 * Frames per `process()` call of an AudioWorklet: the Web Audio render quantum, a platform
 * constant rather than a library choice. Worklet processors only; main-thread facades leave
 * block sizes to the core.
 */
export const AUDIO_WORKLET_RENDER_QUANTUM = 128;
