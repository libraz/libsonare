/**
 * Time and pitch transforms over a whole buffer: stretching, shifting, and
 * correction onto a target pitch.
 */

import type { EffectSamplesRequest } from './_effects_common.js';
import { assertPitchTrackLengths, toVoicedInt32 } from './_effects_common.js';
import { resolvePositiveIntegerOption } from './_feature_options.js';
import { resolveFftOptions } from './_fft_options.js';
import { addon } from './native.js';
import type { PitchCorrectOptions, VoicedFlags } from './types.js';
import { assertFiniteScalar } from './validation.js';

export interface TimeStretchRequest extends EffectSamplesRequest {
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchShiftRequest extends EffectSamplesRequest {
  semitones: number;
  nFft?: number;
  hopLength?: number;
}

export interface PitchCorrectToMidiRequest extends EffectSamplesRequest {
  currentMidi?: number;
  targetMidi?: number;
}

export interface PitchCorrectToMidiTimevaryingRequest extends EffectSamplesRequest {
  f0Hz: Float32Array;
  targetMidi: number;
  hopLength?: number;
  voiced?: VoicedFlags;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends EffectSamplesRequest, PitchCorrectOptions {
  f0Hz: Float32Array;
  hopLength?: number;
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
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
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function timeStretch(
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, rate, nFft, hopLength } : samples;
  assertFiniteScalar('timeStretch', request.rate as number, 'rate');
  const fftOptions = resolveFftOptions('timeStretch', request.nFft, request.hopLength);
  return addon.timeStretch(
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
 * @param sampleRate - Sample rate in Hz
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
  nFft?: number,
  hopLength?: number,
): Float32Array;
export function pitchShift(
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
  nFft?: number,
  hopLength?: number,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, semitones, nFft, hopLength } : samples;
  assertFiniteScalar('pitchShift', request.semitones as number, 'semitones');
  const fftOptions = resolveFftOptions('pitchShift', request.nFft, request.hopLength);
  return addon.pitchShift(
    request.samples,
    request.sampleRate ?? 22050,
    request.semitones,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Apply one constant, immediate transpose from `currentMidi` to `targetMidi`.
 *
 * The result has exactly the input length. Both endpoints must be finite, and
 * the whole interval is applied however large it is: they are validated to
 * [0, 127], so a two-octave move such as C3 -> C5 transposes by the full 24
 * semitones. Use
 * {@link pitchCorrectToMidiTimevarying} for a caller-supplied pitch contour and
 * retune glide.
 */
export function pitchCorrectToMidi(request: PitchCorrectToMidiRequest): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate?: number,
  currentMidi?: number,
  targetMidi?: number,
): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array | PitchCorrectToMidiRequest,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, currentMidi, targetMidi } : samples;
  return addon.pitchCorrectToMidi(
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
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): Float32Array {
  const request =
    samples instanceof Float32Array
      ? {
          samples,
          f0Hz: f0Hz as Float32Array,
          targetMidi: targetMidi as number,
          sampleRate,
          hopLength,
          voiced,
          voicedProb,
        }
      : samples;
  assertPitchTrackLengths(request.f0Hz, request.voiced, request.voicedProb);
  // Positivity only: the corrector requires a positive hop to place the contour
  // and carries no further domain.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'pitchCorrectToMidiTimevarying',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    resolvedHopLength,
    request.voiced ? toVoicedInt32(request.voiced) : undefined,
    request.voicedProb,
  );
}

/**
 * Contour-following pitch correction toward a fixed MIDI note OR a musical
 * scale, with tunable retune strength and vibrato preservation.
 *
 * Generalises {@link pitchCorrectToMidiTimevarying}: the same caller-supplied
 * per-frame `f0Hz` contour drives correction, but {@link options.mode} selects
 * between a fixed-MIDI target (`'midi'`, default) and scale quantisation
 * (`'scale'`), and the retune knobs (`retuneAmount`, `maxCorrectionSemitones`,
 * `retuneSpeedMs`, `vibratoThresholdCents`) shape natural-vs-robotic correction.
 * An `f0Hz` NaN is accepted only for a frame marked unvoiced.
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
  const request =
    samples instanceof Float32Array
      ? { samples, f0Hz: f0Hz as Float32Array, sampleRate, hopLength, ...options }
      : samples;
  const {
    samples: input,
    sampleRate: requestSampleRate,
    f0Hz: requestF0Hz,
    hopLength: requestHopLength,
    ...requestOptions
  } = request;
  assertPitchTrackLengths(requestF0Hz, requestOptions.voiced, requestOptions.voicedProb);
  // Positivity only, as pitchCorrectToMidiTimevarying: the same corrector.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'pitchCorrectTimevarying',
    'hopLength',
    requestHopLength,
    512,
  );
  return addon.pitchCorrectTimevarying(
    input,
    requestSampleRate ?? 22050,
    requestF0Hz,
    resolvedHopLength,
    {
      ...requestOptions,
      voiced: requestOptions.voiced ? toVoicedInt32(requestOptions.voiced) : undefined,
    },
  );
}
