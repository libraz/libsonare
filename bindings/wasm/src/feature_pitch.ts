import { getSonareModule } from './module_state';
import type { PitchResult } from './public_types';

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
