import { addon } from './native.js';
import type {
  HpssResult,
  NoteMoveOptions,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
} from './types.js';

/** Common audio input fields for stateless effect requests. */
export interface EffectSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

export interface HpssRequest extends EffectSamplesRequest {
  kernelHarmonic?: number;
  kernelPercussive?: number;
}

export interface TimeStretchRequest extends EffectSamplesRequest {
  rate: number;
}

export interface SpectralEditRequest extends EffectSamplesRequest, SpectralEditOptions {
  ops?: SpectralRegionOp[];
}

export interface PitchShiftRequest extends EffectSamplesRequest {
  semitones: number;
}

export interface PitchCorrectToMidiRequest extends EffectSamplesRequest {
  currentMidi?: number;
  targetMidi?: number;
}

export interface PitchCorrectToMidiTimevaryingRequest extends EffectSamplesRequest {
  f0Hz: Float32Array;
  targetMidi: number;
  hopLength?: number;
  voiced?: Int32Array;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends EffectSamplesRequest, PitchCorrectOptions {
  f0Hz: Float32Array;
  hopLength?: number;
}

export interface NoteStretchRequest extends EffectSamplesRequest, NoteStretchOptions {}
export interface NoteMoveRequest extends EffectSamplesRequest, NoteMoveOptions {}

// -- Effects --

export function hpss(request: HpssRequest): HpssResult;
export function hpss(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
): HpssResult;
export function hpss(
  samples: Float32Array | HpssRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive }
      : samples;
  return addon.hpss(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
  );
}

export function harmonic(request: EffectSamplesRequest): Float32Array;
export function harmonic(samples: Float32Array, sampleRate?: number): Float32Array;
export function harmonic(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.harmonic(request.samples, request.sampleRate ?? 22050);
}

export function percussive(request: EffectSamplesRequest): Float32Array;
export function percussive(samples: Float32Array, sampleRate?: number): Float32Array;
export function percussive(
  samples: Float32Array | EffectSamplesRequest,
  sampleRate = 22050,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate } : samples;
  return addon.percussive(request.samples, request.sampleRate ?? 22050);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @returns Time-stretched audio
 */
export function timeStretch(request: TimeStretchRequest): Float32Array;
export function timeStretch(samples: Float32Array, sampleRate: number, rate: number): Float32Array;
export function timeStretch(
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, rate } : samples;
  if (typeof request.rate !== 'number' || !Number.isFinite(request.rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
  return addon.timeStretch(request.samples, request.sampleRate ?? 22050, request.rate);
}

/**
 * Region-based spectral editing: STFT -> per-op time x frequency bin/frame
 * masking -> iSTFT. A stateless mono transform whose output has the same length
 * and sample rate as the input.
 *
 * Each {@link SpectralRegionOp} in `ops` is a time x frequency rectangle applied
 * in array order (gain / attenuate / mute / heal). Passing an empty `ops` array
 * is the identity transform (the input is returned). Wraps the C
 * `sonare_spectral_edit`.
 *
 * @param samples - Mono input audio.
 * @param sampleRate - Sample rate in Hz.
 * @param ops - Region ops applied in order.
 * @param options - Optional STFT + heal configuration.
 * @returns The edited audio (same length as `samples`).
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate?: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions = {},
): Float32Array {
  const request =
    samples instanceof Float32Array ? { samples, sampleRate, ops, ...options } : samples;
  const {
    samples: input,
    sampleRate: requestSampleRate,
    ops: requestOps = [],
    ...requestOptions
  } = request;
  return addon.spectralEdit(input, requestSampleRate ?? 22050, requestOps, requestOptions);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @returns Pitch-shifted audio
 */
export function pitchShift(request: PitchShiftRequest): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
): Float32Array;
export function pitchShift(
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, semitones } : samples;
  if (typeof request.semitones !== 'number' || !Number.isFinite(request.semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
  return addon.pitchShift(request.samples, request.sampleRate ?? 22050, request.semitones);
}

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
 * flattened. `voiced` (non-zero = voiced) and `voicedProb` ([0,1]) are optional;
 * omitting them treats every frame as voiced.
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
  voiced?: Int32Array,
  voicedProb?: Float32Array,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: Int32Array,
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
  return addon.pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    request.hopLength ?? 512,
    request.voiced,
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
  return addon.pitchCorrectTimevarying(
    input,
    requestSampleRate ?? 22050,
    requestF0Hz,
    requestHopLength ?? 512,
    requestOptions,
  );
}

export function noteStretch(request: NoteStretchRequest): Float32Array;
export function noteStretch(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteStretchOptions,
): Float32Array;
export function noteStretch(
  samples: Float32Array | NoteStretchRequest,
  sampleRate = 22050,
  options: NoteStretchOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.noteStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? 0,
    request.stretchRatio ?? 1.0,
  );
}

/** Move a note region to a new onset sample without changing its duration. */
export function noteMove(request: NoteMoveRequest): Float32Array;
export function noteMove(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteMoveOptions,
): Float32Array;
export function noteMove(
  samples: Float32Array | NoteMoveRequest,
  sampleRate = 22050,
  options: NoteMoveOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  return addon.noteMove(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? 0,
    request.targetOnsetSample ?? 0,
  );
}
