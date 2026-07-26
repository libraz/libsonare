import type { ValidateOptions } from './validation';

/** Options for `pitchCorrectTimevarying`. All fields are optional. */
export interface PitchCorrectOptions extends ValidateOptions {
  /** `'midi'` retunes toward `targetMidi`; `'scale'` snaps to the key. Default `'midi'`. */
  mode?: 'midi' | 'scale';
  /** Fixed target note when `mode` is `'midi'`, in `[0, 127]`. Default 69 (A4). */
  targetMidi?: number;
  /** Scale root pitch class (0=C .. 11=B) when `mode` is `'scale'`. Default 0. */
  scaleRoot?: number;
  /** 12-bit degree mask, bit `i` = semitone `i` above the root enabled. Default C major. */
  scaleModeMask?: number;
  /** Reference MIDI anchoring the scale grid. Default 69 (A4). */
  referenceMidi?: number;
  /** Correction strength in `[0, 1]`; 1 = full snap, 0 = bypass. Default 1. */
  retuneAmount?: number;
  /** Hard clamp on per-frame correction magnitude (semitones). Default 12. */
  maxCorrectionSemitones?: number;
  /** Retune IIR time constant (ms); larger = slower glide. Default 50. */
  retuneSpeedMs?: number;
  /** Corrections below this are bypassed to preserve vibrato (cents). Default 20. */
  vibratoThresholdCents?: number;
  /** Per-frame voiced flags (non-zero = voiced); omit to treat all frames as voiced. */
  voiced?: Int32Array;
  /** Per-frame voicing probability in `[0, 1]`; omit to derive from `voiced`. */
  voicedProb?: Float32Array;
}

/** Options for `noteStretch`. All fields are optional. */
export interface NoteStretchOptions {
  /** Note onset position in samples (selects the region). Default 0. */
  onsetSample?: number;
  /** Note offset position in samples (selects the region). Defaults to the input length. */
  offsetSample?: number;
  /** Stretch ratio (0.5 = half duration, 2.0 = double duration). Default 1. */
  stretchRatio?: number;
}

/** Options for `noteMove`. */
export interface NoteMoveOptions {
  onsetSample?: number;
  /** Defaults to the input length. */
  offsetSample?: number;
  targetOnsetSample?: number;
}

/** How a `spectralEdit` region op modifies the masked bins. */
export type SpectralEditMode = 'gain' | 'attenuate' | 'mute' | 'heal';

/** Analysis/synthesis window used by `spectralEdit`. */
export type SpectralEditWindow = 'hann' | 'hamming' | 'blackman' | 'rectangular' | 'rect';

/** One time x frequency rectangle edit op for `spectralEdit`. */
export interface SpectralRegionOp {
  /** Region time start (input samples); clamped to [0, length]. Default 0. */
  startSample?: number;
  /** Region time end, exclusive (input samples); clamped to [0, length]. Default = signal length. */
  endSample?: number;
  /** Region frequency low edge in Hz; clamped to [0, nyquist]. Default 0. */
  lowHz?: number;
  /** Region frequency high edge in Hz; <=0 or >= nyquist means nyquist. Default 0. */
  highHz?: number;
  /** Linear gain in dB for 'gain'/'attenuate'; ignored by 'mute'/'heal'. Default 0. */
  gainDb?: number;
  /** Edit mode. Default 'gain'. */
  mode?: SpectralEditMode;
}

/** STFT + heal parameters for `spectralEdit`. All fields are optional. */
export interface SpectralEditOptions {
  /** FFT size; must be a power of two (>= 2). Default 2048. */
  nFft?: number;
  /** Hop length; must satisfy 0 < hop <= nFft/2. Default 512. */
  hopLength?: number;
  /** Analysis + synthesis window. Default 'hann'. */
  window?: SpectralEditWindow;
  /** Neighbour frames each side used by 'heal' (>= 1). Default 2. */
  healRadiusFrames?: number;
}

/**
 * Constant-Q / Variable-Q transform magnitude result (mirrors the C
 * `SonareCqtResult`).
 */
export interface CqtResult {
  /** Number of frequency bins. */
  nBins: number;
  /** Number of time frames. */
  nFrames: number;
  /** Hop length in samples. */
  hopLength: number;
  /** Sample rate in Hz. */
  sampleRate: number;
  /** Row-major `[nBins x nFrames]` magnitude matrix. */
  magnitude: Float32Array;
  /** Center frequency (Hz) of each of the `nBins` bins. */
  frequencies: Float32Array;
}

/**
 * Multi-band onset strength matrix result.
 */
export interface OnsetStrengthMultiResult {
  nBands: number;
  nFrames: number;
  data: Float32Array;
}

/**
 * STFT (Short-Time Fourier Transform) result
 */
export interface StftResult {
  nBins: number;
  nFrames: number;
  nFft: number;
  hopLength: number;
  sampleRate: number;
  magnitude: Float32Array;
  power: Float32Array;
}

/**
 * Mel spectrogram result
 */
export interface MelSpectrogramResult {
  nMels: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  power: Float32Array;
  db: Float32Array;
}

/**
 * MFCC result
 */
export interface MfccResult {
  nMfcc: number;
  nFrames: number;
  coefficients: Float32Array;
}

/**
 * STFT power spectrogram result (from inverse Mel reconstruction)
 */
export interface StftPowerResult {
  nBins: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Mel power spectrogram result (from inverse MFCC reconstruction)
 */
export interface MelPowerResult {
  nMels: number;
  nFrames: number;
  power: Float32Array;
}

/**
 * Chroma features result
 */
export interface ChromaResult {
  nChroma: number;
  nFrames: number;
  sampleRate: number;
  hopLength: number;
  features: Float32Array;
  meanEnergy: number[];
}

/**
 * Pitch detection result
 */
export interface PitchResult {
  f0: Float32Array;
  voicedProb: Float32Array;
  voicedFlag: boolean[];
  nFrames: number;
  medianF0: number;
  meanF0: number;
}

/**
 * Loudness measurement result (EBU R128 / ITU-R BS.1770)
 */
export interface LufsResult {
  integratedLufs: number;
  /** Final complete 400 ms window, not Max-M. */
  momentaryLufs: number;
  /** Final complete 3 s window, not Max-S. */
  shortTermLufs: number;
  /** Maximum 400 ms window (EBU R128 Max-M). */
  maxMomentaryLufs: number;
  /** Maximum 3 s window (EBU R128 Max-S). */
  maxShortTermLufs: number;
  loudnessRange: number;
}

/**
 * HPSS (Harmonic-Percussive Source Separation) result
 */
export interface HpssResult {
  harmonic: Float32Array;
  percussive: Float32Array;
  sampleRate: number;
}
