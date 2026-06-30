import { getSonareModule } from './module_state';
import type {
  HpssResult,
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
export function hpss(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  return requireModule().hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

/**
 * Extract harmonic component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Harmonic component
 */
export function harmonic(
  samples: Float32Array,
  sampleRate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('harmonic', samples, options.validate !== false);
  return requireModule().harmonic(samples, sampleRate);
}

/**
 * Extract percussive component from audio.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @returns Percussive component
 */
export function percussive(
  samples: Float32Array,
  sampleRate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('percussive', samples, options.validate !== false);
  return requireModule().percussive(samples, sampleRate);
}

/**
 * Time-stretch audio without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param rate - Time stretch rate (0.5 = double duration, 2.0 = half duration)
 * @returns Time-stretched audio
 */
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  rate: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('timeStretch', samples, options.validate !== false);
  return requireModule().timeStretch(samples, sampleRate, rate);
}

/**
 * Pitch-shift audio without changing duration.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param semitones - Pitch shift in semitones (+12 = one octave up, -12 = one octave down)
 * @returns Pitch-shifted audio
 */
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  semitones: number,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('pitchShift', samples, options.validate !== false);
  return requireModule().pitchShift(samples, sampleRate, semitones);
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
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('pitchCorrectToMidi', samples, options.validate !== false);
  return requireModule().pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
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
  samples: Float32Array,
  f0Hz: Float32Array,
  targetMidi: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: Int32Array,
  voicedProb?: Float32Array,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('pitchCorrectToMidiTimevarying', samples, options.validate !== false);
  if (voiced && voiced.length !== f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voiced length must match f0Hz length');
  }
  if (voicedProb && voicedProb.length !== f0Hz.length) {
    throw new RangeError('pitchCorrectToMidiTimevarying: voicedProb length must match f0Hz length');
  }
  // The embind layer reads the companion arrays as Float32Array (voiced uses
  // 0.0/1.0); convert here so a single native conversion path suffices.
  const voicedF32 = voiced ? Float32Array.from(voiced) : undefined;
  return requireModule().pitchCorrectToMidiTimevarying(
    samples,
    sampleRate,
    f0Hz,
    targetMidi,
    hopLength,
    voicedF32,
    voicedProb,
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
export function pitchCorrectTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  options: PitchCorrectOptions = {},
): Float32Array {
  assertSamples('pitchCorrectTimevarying', samples, options.validate !== false);
  if (options.voiced && options.voiced.length !== f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voiced length must match f0Hz length');
  }
  if (options.voicedProb && options.voicedProb.length !== f0Hz.length) {
    throw new RangeError('pitchCorrectTimevarying: voicedProb length must match f0Hz length');
  }
  // The embind layer reads the companion arrays as Float32Array (voiced uses
  // 0.0/1.0); convert here so a single native conversion path suffices.
  const nativeOptions = {
    ...options,
    voiced: options.voiced ? Float32Array.from(options.voiced) : undefined,
  };
  return requireModule().pitchCorrectTimevarying(
    samples,
    sampleRate,
    f0Hz,
    hopLength,
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
export function noteStretch(
  samples: Float32Array,
  sampleRate = 22050,
  options: NoteStretchOptions & ValidateOptions = {},
): Float32Array {
  assertSamples('noteStretch', samples, options.validate !== false);
  return requireModule().noteStretch(
    samples,
    sampleRate,
    options.onsetSample ?? 0,
    options.offsetSample ?? 0,
    options.stretchRatio ?? 1.0,
  );
}

/**
 * Normalize audio to target peak level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param targetDb - Target peak level in dB (default: 0 dB = full scale)
 * @returns Normalized audio
 */
export function normalize(
  samples: Float32Array,
  sampleRate: number,
  targetDb = 0.0,
  options: ValidateOptions = {},
): Float32Array {
  assertSamples('normalize', samples, options.validate !== false);
  return requireModule().normalize(samples, sampleRate, targetDb);
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
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions & ValidateOptions = {},
): Float32Array {
  assertSamples('spectralEdit', samples, options.validate !== false);
  assertSampleRate('spectralEdit', sampleRate);
  return requireModule().spectralEdit(samples, sampleRate, ops, options as Record<string, unknown>);
}
