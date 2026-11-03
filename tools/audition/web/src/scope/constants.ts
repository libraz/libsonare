/** Spectrogram analysis window, hop (samples) and the floor (dB) that maps to the darkest colour. */
export const FFT_SIZE = 1024;
export const HOP_SIZE = 512;
export const FLOOR_DB = -78;

/** How close to the playhead (CSS pixels) counts as on it. */
export const GRAB_PX = 9;
/** A drag shorter than this (seconds) was a click with a shaky hand. */
export const MIN_DRAG = 0.02;

/** Widest the waveform and spectrogram are computed; wider canvases stretch it. */
export const WAVE_COLS = 3000;
export const SPEC_COLS = 1400;
