/**
 * Time and pitch transforms over a whole buffer: stretching, shifting, and
 * correction onto a target pitch.
 */

import { toVoicedFloat32 } from './_effects_common';
import { resolveFftOptions } from './_fft_options';
import { getSonareModule } from './module_state';
import type { PitchCorrectOptions, VoicedFlags } from './public_types';
import type { ValidateOptions } from './validation';
import { assertFiniteScalar, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

export interface TimeStretchRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchShiftRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  semitones: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchCorrectToMidiRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  currentMidi?: number;
  targetMidi?: number;
}

export interface PitchCorrectToMidiTimevaryingRequest extends ValidateOptions {
  samples: Float32Array;
  f0Hz: Float32Array;
  targetMidi: number;
  sampleRate?: number;
  hopLength?: number;
  voiced?: VoicedFlags;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends PitchCorrectOptions {
  samples: Float32Array;
  f0Hz: Float32Array;
  sampleRate?: number;
  hopLength?: number;
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @param nFft - FFT size: an even integer >= 2 (default 2048)
 * @param hopLength - Hop in samples, in `(0, nFft / 2]` (default 512), so
 *   frames overlap by at least half a window
 * @returns Time-stretched audio
 */
export function timeStretch(request: TimeStretchRequest): Float32Array;
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  options?: ValidateOptions,
): Float32Array;
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  nFft?: number,
  hopLength?: number,
  options?: ValidateOptions,
): Float32Array;
export function timeStretch(
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
  nFftOrOptions?: number | ValidateOptions,
  hopLength?: number,
  options: ValidateOptions = {},
): Float32Array {
  if (
    nFftOrOptions !== undefined &&
    nFftOrOptions !== null &&
    typeof nFftOrOptions !== 'number' &&
    typeof nFftOrOptions !== 'object'
  ) {
    throw new TypeError('timeStretch: nFft must be an integer or options object');
  }
  if (nFftOrOptions === null) {
    throw new TypeError('timeStretch: nFft must be an integer or options object');
  }
  const positionalOptions =
    typeof nFftOrOptions === 'object' && nFftOrOptions !== null ? nFftOrOptions : options;
  const positionalNFft = typeof nFftOrOptions === 'number' ? nFftOrOptions : undefined;
  const request: TimeStretchRequest =
    samples instanceof Float32Array
      ? {
          samples,
          sampleRate,
          rate: rate as number,
          nFft: positionalNFft,
          hopLength,
          ...positionalOptions,
        }
      : samples;
  assertSamples('timeStretch', request.samples, request.validate !== false);
  // Matches the addon, which refuses a non-finite rate here rather than letting
  // the core answer it. Does NOT cover a finite value too wide for a float:
  // Number.isFinite(1e39) is true and the demotion to the f32 parameter makes it
  // an infinity, which only the binding-side narrowing can see.
  assertFiniteScalar('timeStretch', request.rate as number, 'rate');
  const fftOptions = resolveFftOptions('timeStretch', request.nFft, request.hopLength);
  return requireModule().timeStretchEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @param nFft - FFT size: an even integer >= 2 (default 2048)
 * @param hopLength - Hop in samples, in `(0, nFft / 2]` (default 512), so
 *   frames overlap by at least half a window
 * @returns Pitch-shifted audio
 */
export function pitchShift(request: PitchShiftRequest): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  options?: ValidateOptions,
): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  nFft?: number,
  hopLength?: number,
  options?: ValidateOptions,
): Float32Array;
export function pitchShift(
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
  nFftOrOptions?: number | ValidateOptions,
  hopLength?: number,
  options: ValidateOptions = {},
): Float32Array {
  if (
    nFftOrOptions !== undefined &&
    nFftOrOptions !== null &&
    typeof nFftOrOptions !== 'number' &&
    typeof nFftOrOptions !== 'object'
  ) {
    throw new TypeError('pitchShift: nFft must be an integer or options object');
  }
  if (nFftOrOptions === null) {
    throw new TypeError('pitchShift: nFft must be an integer or options object');
  }
  const positionalOptions =
    typeof nFftOrOptions === 'object' && nFftOrOptions !== null ? nFftOrOptions : options;
  const positionalNFft = typeof nFftOrOptions === 'number' ? nFftOrOptions : undefined;
  const request: PitchShiftRequest =
    samples instanceof Float32Array
      ? {
          samples,
          sampleRate,
          semitones: semitones as number,
          nFft: positionalNFft,
          hopLength,
          ...positionalOptions,
        }
      : samples;
  assertSamples('pitchShift', request.samples, request.validate !== false);
  // See timeStretch above for what this does and does not cover.
  assertFiniteScalar('pitchShift', request.semitones as number, 'semitones');
  const fftOptions = resolveFftOptions('pitchShift', request.nFft, request.hopLength);
  return requireModule().pitchShiftEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.semitones,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Pitch-correct audio from a current MIDI note to a target MIDI note.
 *
 * Applies one constant, immediate transpose with no retune glide and preserves
 * the input buffer length. The whole interval is applied however large it is:
 * both endpoints are validated to [0, 127], so a two-octave move such as
 * C3 -> C5 transposes by the full 24 semitones. Use
 * {@link pitchCorrectToMidiTimevarying} for a caller-supplied pitch contour.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param currentMidi - Detected/current MIDI note number
 * @param targetMidi - Desired MIDI note number
 * @returns Pitch-corrected audio
 */
export function pitchCorrectToMidi(request: PitchCorrectToMidiRequest): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate?: number,
  currentMidi?: number,
  targetMidi?: number,
  options?: ValidateOptions,
): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array | PitchCorrectToMidiRequest,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
  options: ValidateOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, currentMidi, targetMidi, ...options }
      : samples;
  assertSamples('pitchCorrectToMidi', request.samples, request.validate !== false);
  return requireModule().pitchCorrectToMidi(
    request.samples,
    request.sampleRate ?? 22050,
    request.currentMidi ?? 69.0,
    request.targetMidi ?? 69.0,
  );
}

/**
 * Contour-following ("time-varying") pitch correction toward a MIDI target.
 *
 * Unlike {@link pitchCorrectToMidi} (a single constant transpose), this follows
 * the caller-supplied per-frame `f0Hz` contour and retunes every voiced frame
 * toward `targetMidi`, so vibrato/drift in the source is tracked rather than
 * flattened. `voiced` (truthy = voiced) and `voicedProb` ([0,1]) are optional;
 * omitting them treats every frame as voiced. An `f0Hz` NaN is accepted only
 * when the corresponding `voiced` entry is falsy, matching pYIN output. The
 * `voicedFlag` / `voicedProb` arrays of a {@link PitchResult} can be passed
 * through directly.
 *
 * @param samples - Audio samples (mono, float32)
 * @param f0Hz - Per-frame measured F0 in Hz (one entry per analysis frame)
 * @param targetMidi - Desired MIDI note number
 * @param sampleRate - Sample rate in Hz
 * @param hopLength - F0 hop in samples (frame i covers sample i*hopLength)
 * @param voiced - Optional per-frame voiced flags (truthy = voiced)
 * @param voicedProb - Optional per-frame voicing probability in [0, 1]
 * @returns Pitch-corrected audio
 */
export function pitchCorrectToMidiTimevarying(
  request: PitchCorrectToMidiTimevaryingRequest,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  targetMidi: number,
  sampleRate?: number,
  hopLength?: number,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
  options?: ValidateOptions,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
  options: ValidateOptions = {},
): Float32Array {
  const request: PitchCorrectToMidiTimevaryingRequest =
    samples instanceof Float32Array
      ? {
          samples,
          f0Hz: f0Hz as Float32Array,
          targetMidi: targetMidi as number,
          sampleRate,
          hopLength,
          voiced,
          voicedProb,
          ...options,
        }
      : samples;
  assertSamples('pitchCorrectToMidiTimevarying', request.samples, request.validate !== false);
  if (request.voiced && request.voiced.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voiced length must match f0Hz length');
  }
  if (request.voicedProb && request.voicedProb.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voicedProb length must match f0Hz length');
  }
  const voicedF32 = request.voiced ? toVoicedFloat32(request.voiced) : undefined;
  return requireModule().pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    request.hopLength ?? 512,
    voicedF32,
    request.voicedProb,
  );
}

/**
 * Contour-following pitch correction toward a fixed MIDI note OR a musical
 * scale, with tunable retune strength and vibrato preservation.
 *
 * Generalises {@link pitchCorrectToMidiTimevarying}: the same caller-supplied
 * per-frame `f0Hz` contour drives correction, but `options.mode` selects between
 * a fixed-MIDI target (`'midi'`, default) and scale quantisation (`'scale'`),
 * and the retune knobs shape natural-vs-robotic correction. An `f0Hz` NaN is
 * accepted only for a frame marked unvoiced.
 *
 * @param samples - Audio samples (mono, float32)
 * @param f0Hz - Per-frame measured F0 in Hz (one entry per analysis frame)
 * @param sampleRate - Sample rate in Hz
 * @param hopLength - F0 hop in samples (frame i covers sample i*hopLength)
 * @param options - Target mode + retune knobs + optional voiced/voicedProb arrays
 * @returns Pitch-corrected audio
 */
export function pitchCorrectTimevarying(request: PitchCorrectTimevaryingRequest): Float32Array;
export function pitchCorrectTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  sampleRate?: number,
  hopLength?: number,
  options?: PitchCorrectOptions,
): Float32Array;
export function pitchCorrectTimevarying(
  samples: Float32Array | PitchCorrectTimevaryingRequest,
  f0Hz?: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  options: PitchCorrectOptions = {},
): Float32Array {
  const request: PitchCorrectTimevaryingRequest =
    samples instanceof Float32Array
      ? { samples, f0Hz: f0Hz as Float32Array, sampleRate, hopLength, ...options }
      : samples;
  assertSamples('pitchCorrectTimevarying', request.samples, request.validate !== false);
  if (request.voiced && request.voiced.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voiced length must match f0Hz length');
  }
  if (request.voicedProb && request.voicedProb.length !== request.f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voicedProb length must match f0Hz length');
  }
  const nativeOptions = {
    ...request,
    voiced: request.voiced ? toVoicedFloat32(request.voiced) : undefined,
  };
  return requireModule().pitchCorrectTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.hopLength ?? 512,
    nativeOptions,
  );
}
