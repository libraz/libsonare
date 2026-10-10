import type { SpectralFrameRequest } from './feature_spectral.js';
import { getSonareModule } from './module_state.js';
import type { NoteSegment, PiptrackResult, PitchResult } from './public_types.js';
import { assertAudioInput, requestObject } from './validation.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Features - Pitch
// ============================================================================

/**
 * Detect pitch using YIN algorithm.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.1)
 * @param fillNa - Retained for compatibility; YIN always returns a finite per-frame estimate.
 * @returns Pitch detection result
 */
export interface PitchYinRequest {
  samples: Float32Array;
  sampleRate?: number;
  frameLength?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  threshold?: number;
  fillNa?: boolean;
}

export interface PiptrackRequest {
  samples: Float32Array;
  sampleRate?: number;
  nFft?: number;
  hopLength?: number;
  fmin?: number;
  fmax?: number;
  threshold?: number;
}

/** Per-bin spectral pitch candidates and peak magnitudes (librosa.piptrack). */
export function piptrack(request: PiptrackRequest): PiptrackResult;
export function piptrack(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
): PiptrackResult;
export function piptrack(
  samples: Float32Array | PiptrackRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  fmin = 150,
  fmax = 4000,
  threshold = 0.1,
): PiptrackResult {
  if (!(samples instanceof Float32Array)) {
    requestObject('piptrack', samples);
    const request = samples;
    return piptrack(
      request.samples,
      request.sampleRate,
      request.nFft,
      request.hopLength,
      request.fmin,
      request.fmax,
      request.threshold,
    );
  }
  assertAudioInput('piptrack', samples, sampleRate);
  return requireModule().piptrack(samples, sampleRate, nFft, hopLength, fmin, fmax, threshold);
}

export function pitchYin(request: PitchYinRequest): PitchResult;
export function pitchYin(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
  fillNa?: boolean,
): PitchResult;
export function pitchYin(
  samples: Float32Array | PitchYinRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.1,
  fillNa = false,
): PitchResult {
  if (!(samples instanceof Float32Array)) {
    requestObject('pitchYin', samples);
    const request = samples;
    return pitchYin(
      request.samples,
      request.sampleRate,
      request.frameLength,
      request.hopLength,
      request.fmin,
      request.fmax,
      request.threshold,
      request.fillNa,
    );
  }
  assertAudioInput('pitchYin', samples, sampleRate);
  return requireModule().pitchYin(
    samples,
    sampleRate,
    frameLength,
    hopLength,
    fmin,
    fmax,
    threshold,
    fillNa,
  );
}

/**
 * Detect pitch using pYIN algorithm (probabilistic YIN with HMM smoothing).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param frameLength - Frame length (default: 2048)
 * @param hopLength - Hop length (default: 512)
 * @param fmin - Minimum frequency in Hz (default: 65)
 * @param fmax - Maximum frequency in Hz (default: 2093)
 * @param threshold - YIN threshold (default: 0.1)
 * @param fillNa - If true, return 0 for unvoiced f0 frames; otherwise keep NaN (default: false)
 * @returns Pitch detection result
 */
export interface PitchPyinRequest extends PitchYinRequest {}

export function pitchPyin(request: PitchPyinRequest): PitchResult;
export function pitchPyin(
  samples: Float32Array,
  sampleRate?: number,
  frameLength?: number,
  hopLength?: number,
  fmin?: number,
  fmax?: number,
  threshold?: number,
  fillNa?: boolean,
): PitchResult;
export function pitchPyin(
  samples: Float32Array | PitchPyinRequest,
  sampleRate = 22050,
  frameLength = 2048,
  hopLength = 512,
  fmin = 65.0,
  fmax = 2093.0,
  threshold = 0.1,
  fillNa = false,
): PitchResult {
  if (!(samples instanceof Float32Array)) {
    requestObject('pitchPyin', samples);
    const request = samples;
    return pitchPyin(
      request.samples,
      request.sampleRate,
      request.frameLength,
      request.hopLength,
      request.fmin,
      request.fmax,
      request.threshold,
      request.fillNa,
    );
  }
  assertAudioInput('pitchPyin', samples, sampleRate);
  return requireModule().pitchPyin(
    samples,
    sampleRate,
    frameLength,
    hopLength,
    fmin,
    fmax,
    threshold,
    fillNa,
  );
}

/** Parameters for segmenting an F0 track into stable monophonic notes. */
export interface NoteSegmentsRequest {
  f0Hz: Float32Array;
  /**
   * Per-frame voicing values in `[0, 1]`; anything below `voicedThreshold` is
   * unvoiced.
   *
   * Pass {@link PitchResult.voicedFlag} converted to `0`/`1`. Do **not** pass
   * {@link pitchPyin}'s `voicedProb`: that value is the frame's voiced
   * observation mass and rises with F0 for a fixed `frameLength`, so a fixed
   * threshold silently returns no segments at all for low-register material
   * (a steady tone below roughly C5 never reaches 0.5).
   */
  voicedProb: Float32Array;
  frameRate: number;
  /** Cents of pitch change that start a new segment. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest segment kept, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each segment's cents are measured against. Default 440. */
  referenceHz?: number;
  /** Voicing threshold applied to `voicedProb`; defaults to `0.5`. */
  voicedThreshold?: number;
}

/**
 * Segment a caller-supplied monophonic F0 track into stable note regions.
 *
 * `f0Hz` and `voicedProb` must have the same non-zero length. Zero-Hz frames
 * and values below `voicedThreshold` (default `0.5`) are treated as unvoiced.
 */
export function noteSegments(request: NoteSegmentsRequest): NoteSegment[] {
  requestObject('noteSegments', request, 'request', true);
  return requireModule().noteSegments(request.f0Hz, request.voicedProb, request.frameRate, {
    segmentationThresholdCents: request.segmentationThresholdCents,
    minNoteMs: request.minNoteMs,
    referenceHz: request.referenceHz,
    voicedThreshold: request.voicedThreshold,
  });
}

// ============================================================================
// Features - Tuning
// ============================================================================

export interface PitchTuningRequest {
  frequencies: Float32Array;
  resolution?: number;
  binsPerOctave?: number;
}

export interface EstimateTuningRequest extends SpectralFrameRequest {
  resolution?: number;
  binsPerOctave?: number;
}

/**
 * Estimate the global tuning offset from a set of frequencies
 * (librosa.pitch_tuning). Returns a deviation in fractions of a bin.
 */
export function pitchTuning(request: PitchTuningRequest): number;
export function pitchTuning(
  frequencies: Float32Array,
  resolution?: number,
  binsPerOctave?: number,
): number;
export function pitchTuning(
  frequencies: Float32Array | PitchTuningRequest,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  if (!(frequencies instanceof Float32Array)) {
    requestObject('pitchTuning', frequencies, 'frequencies');
    const r = frequencies;
    return pitchTuning(r.frequencies, r.resolution, r.binsPerOctave);
  }
  return requireModule().pitchTuning(frequencies, resolution, binsPerOctave);
}

/**
 * Reference frequency of an A4 raised by `tuning` fractions of a semitone:
 * `a4 * 2 ** (tuning / 12)`. The converter from the analysis unit (`tuning`, in
 * `[-0.5, 0.5)` for the analysis options) to the Hz a pitch reference is stated
 * in, such as `referenceHz` and the streaming `tuningRefHz`.
 *
 * @param tuning - Finite fraction of a semitone.
 * @param a4 - Finite positive concert pitch the tuning is measured from. Default 440.
 * @throws `RangeError` for a non-finite `tuning`, `SonareError` for a non-positive `a4`.
 */
export function tuningToReferenceHz(tuning: number, a4 = 440): number {
  return requireModule().tuningToReferenceHz(tuning, a4);
}

/**
 * Tuning, in fractions of a semitone, of a recording whose A4 sits at `hz`:
 * `12 * log2(hz / a4)`. Inverse of {@link tuningToReferenceHz}.
 *
 * @param hz - Finite positive reference frequency.
 * @param a4 - Finite positive concert pitch. Default 440.
 * @throws `RangeError` for a non-finite `hz` or `a4`, `SonareError` for a non-positive one.
 */
export function referenceHzToTuning(hz: number, a4 = 440): number {
  return requireModule().referenceHzToTuning(hz, a4);
}

/**
 * Estimate the tuning offset of an audio signal (librosa.estimate_tuning).
 *
 * A librosa mirror: the offset is a fraction of a bin of `binsPerOctave`, which
 * is the semitone fraction the analysis options take only at 12 (otherwise
 * multiply by `12 / binsPerOctave`). Analysis measures the semitone fraction
 * itself with `tuning: 'auto'`; {@link tuningToReferenceHz} converts it to Hz.
 */
export function estimateTuning(request: EstimateTuningRequest): number;
export function estimateTuning(
  samples: Float32Array,
  sampleRate?: number,
  nFft?: number,
  hopLength?: number,
  resolution?: number,
  binsPerOctave?: number,
): number;
export function estimateTuning(
  samples: Float32Array | EstimateTuningRequest,
  sampleRate = 22050,
  nFft = 2048,
  hopLength = 512,
  resolution = 0.01,
  binsPerOctave = 12,
): number {
  if (!(samples instanceof Float32Array)) {
    requestObject('estimateTuning', samples);
    const r = samples;
    return estimateTuning(
      r.samples,
      r.sampleRate,
      r.nFft,
      r.hopLength,
      r.resolution,
      r.binsPerOctave,
    );
  }
  assertAudioInput('estimateTuning', samples, sampleRate);
  return requireModule().estimateTuning(
    samples,
    sampleRate,
    nFft,
    hopLength,
    resolution,
    binsPerOctave,
  );
}
