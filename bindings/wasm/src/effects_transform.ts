import { getSonareModule } from './module_state';
import type {
  HpssResult,
  NoteMoveOptions,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
} from './public_types';
import type { ValidateOptions } from './validation';
import { assertSampleRate, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

/** Canonical request form for HPSS. */
export interface HpssRequest {
  samples: Float32Array;
  sampleRate?: number;
  kernelHarmonic?: number;
  kernelPercussive?: number;
}

export interface HarmonicRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface PercussiveRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface TimeStretchRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  rate: number;
}

export interface PitchShiftRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  semitones: number;
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
  voiced?: Int32Array;
  voicedProb?: Float32Array;
}

export interface PitchCorrectTimevaryingRequest extends PitchCorrectOptions {
  samples: Float32Array;
  f0Hz: Float32Array;
  sampleRate?: number;
  hopLength?: number;
}

export interface NoteStretchRequest extends NoteStretchOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}
export interface NoteMoveRequest extends NoteMoveOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
}

export interface NormalizeRequest extends ValidateOptions {
  samples: Float32Array;
  sampleRate?: number;
  targetDb?: number;
}

export interface SpectralEditRequest extends SpectralEditOptions, ValidateOptions {
  samples: Float32Array;
  sampleRate: number;
  ops?: SpectralRegionOp[];
}

// ============================================================================
// Effects
// ============================================================================

/**
 * Perform Harmonic-Percussive Source Separation (HPSS).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param kernelHarmonic - Horizontal median filter size for harmonic (default: 31)
 * @param kernelPercussive - Vertical median filter size for percussive (default: 31)
 * @returns Separated harmonic and percussive components
 */
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
  return requireModule().hpss(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
  );
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(request: HarmonicRequest): Float32Array;
export function harmonic(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function harmonic(
  samples: Float32Array | HarmonicRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('harmonic', request.samples, request.validate !== false);
  return requireModule().harmonic(request.samples, request.sampleRate ?? 22050);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(request: PercussiveRequest): Float32Array;
export function percussive(
  samples: Float32Array,
  sampleRate?: number,
  options?: ValidateOptions,
): Float32Array;
export function percussive(
  samples: Float32Array | PercussiveRequest,
  sampleRate = 22050,
  options: ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('percussive', request.samples, request.validate !== false);
  return requireModule().percussive(request.samples, request.sampleRate ?? 22050);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
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
  samples: Float32Array | TimeStretchRequest,
  sampleRate?: number,
  rate?: number,
  options: ValidateOptions = {},
): Float32Array {
  const request: TimeStretchRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, rate: rate as number, ...options }
      : samples;
  assertSamples('timeStretch', request.samples, request.validate !== false);
  return requireModule().timeStretch(request.samples, request.sampleRate ?? 22050, request.rate);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
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
  samples: Float32Array | PitchShiftRequest,
  sampleRate?: number,
  semitones?: number,
  options: ValidateOptions = {},
): Float32Array {
  const request: PitchShiftRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, semitones: semitones as number, ...options }
      : samples;
  assertSamples('pitchShift', request.samples, request.validate !== false);
  return requireModule().pitchShift(
    request.samples,
    request.sampleRate ?? 22050,
    request.semitones,
  );
}

/**
 * Pitch-correct audio from a current MIDI note to a target MIDI note.
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
 * flattened. `voiced` (non-zero = voiced) and `voicedProb` ([0,1]) are optional;
 * omitting them treats every frame as voiced.
 *
 * @param samples - Audio samples (mono, float32)
 * @param f0Hz - Per-frame measured F0 in Hz (one entry per analysis frame)
 * @param targetMidi - Desired MIDI note number
 * @param sampleRate - Sample rate in Hz
 * @param hopLength - F0 hop in samples (frame i covers sample i*hopLength)
 * @param voiced - Optional per-frame voiced flags (non-zero = voiced)
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
  voiced?: Int32Array,
  voicedProb?: Float32Array,
  options?: ValidateOptions,
): Float32Array;
export function pitchCorrectToMidiTimevarying(
  samples: Float32Array | PitchCorrectToMidiTimevaryingRequest,
  f0Hz?: Float32Array,
  targetMidi?: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: Int32Array,
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
  // The embind layer reads the companion arrays as Float32Array (voiced uses
  // 0.0/1.0); convert here so a single native conversion path suffices.
  const voicedF32 = request.voiced ? Float32Array.from(request.voiced) : undefined;
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
 * and the retune knobs shape natural-vs-robotic correction.
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
  // The embind layer reads the companion arrays as Float32Array (voiced uses
  // 0.0/1.0); convert here so a single native conversion path suffices.
  const nativeOptions = {
    ...request,
    voiced: request.voiced ? Float32Array.from(request.voiced) : undefined,
  };
  return requireModule().pitchCorrectTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.hopLength ?? 512,
    nativeOptions,
  );
}

/**
 * Time-stretch a note region between two sample offsets without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onsetSample - Note onset position in samples
 * @param offsetSample - Note offset position in samples
 * @param stretchRatio - Stretch ratio (0.5 = double duration, 2.0 = half duration)
 * @returns Audio with the note region stretched
 */
export function noteStretch(request: NoteStretchRequest): Float32Array;
export function noteStretch(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteStretchOptions & ValidateOptions,
): Float32Array;
export function noteStretch(
  samples: Float32Array | NoteStretchRequest,
  sampleRate = 22050,
  options: NoteStretchOptions & ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('noteStretch', request.samples, request.validate !== false);
  return requireModule().noteStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? 0,
    request.stretchRatio ?? 1.0,
  );
}

/** Move a note region to a new onset without changing its duration. */
export function noteMove(request: NoteMoveRequest): Float32Array;
export function noteMove(
  samples: Float32Array,
  sampleRate?: number,
  options?: NoteMoveOptions & ValidateOptions,
): Float32Array;
export function noteMove(
  samples: Float32Array | NoteMoveRequest,
  sampleRate = 22050,
  options: NoteMoveOptions & ValidateOptions = {},
): Float32Array {
  const request = samples instanceof Float32Array ? { samples, sampleRate, ...options } : samples;
  assertSamples('noteMove', request.samples, request.validate !== false);
  return requireModule().noteMove(
    request.samples,
    request.sampleRate ?? 22050,
    request.onsetSample ?? 0,
    request.offsetSample ?? 0,
    request.targetOnsetSample ?? 0,
  );
}

/**
 * Normalize audio to target peak level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param targetDb - Target peak level in dB (default: 0 dB = full scale)
 * @returns Normalized audio
 */
export function normalize(request: NormalizeRequest): Float32Array;
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate?: number,
  targetDb = 0.0,
  options: ValidateOptions = {},
): Float32Array {
  const request: NormalizeRequest =
    samples instanceof Float32Array ? { samples, sampleRate, targetDb, ...options } : samples;
  assertSamples('normalize', request.samples, request.validate !== false);
  return requireModule().normalize(
    request.samples,
    request.sampleRate ?? 22050,
    request.targetDb ?? 0.0,
  );
}

/**
 * Apply region-based spectral edits (gain/attenuate/mute/heal) to mono audio.
 *
 * Each op is a time x frequency rectangle applied in array order over a single
 * STFT buffer, so a later op observes the result of earlier ops. The output has
 * the same length and sample rate as the input; an empty `ops` list is an
 * identity transform (within the iSTFT's own tolerance).
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param ops - Region edit ops applied in order ({@link SpectralRegionOp})
 * @param options - STFT + heal configuration ({@link SpectralEditOptions})
 * @returns Edited audio
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops?: SpectralRegionOp[],
  options?: SpectralEditOptions & ValidateOptions,
): Float32Array;
export function spectralEdit(
  samples: Float32Array | SpectralEditRequest,
  sampleRate?: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions & ValidateOptions = {},
): Float32Array {
  const request: SpectralEditRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate: sampleRate as number, ops, ...options }
      : samples;
  assertSamples('spectralEdit', request.samples, request.validate !== false);
  assertSampleRate('spectralEdit', request.sampleRate);
  return requireModule().spectralEdit(
    request.samples,
    request.sampleRate,
    request.ops ?? [],
    request as unknown as Record<string, unknown>,
  );
}
