import { getSonareModule } from './module_state';
import type { NoteSegment, PiptrackResult, PitchResult } from './public_types';

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
  segmentationThresholdCents?: number;
  minNoteMs?: number;
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
  return requireModule().noteSegments(request.f0Hz, request.voicedProb, request.frameRate, {
    segmentationThresholdCents: request.segmentationThresholdCents,
    minNoteMs: request.minNoteMs,
    referenceHz: request.referenceHz,
    voicedThreshold: request.voicedThreshold,
  });
}
