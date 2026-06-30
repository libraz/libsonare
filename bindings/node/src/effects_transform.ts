import { addon } from './native.js';
import type {
  HpssResult,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
} from './types.js';

// -- Effects --

export function hpss(
  samples: Float32Array,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
): HpssResult {
  return addon.hpss(samples, sampleRate, kernelHarmonic, kernelPercussive);
}

export function harmonic(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.harmonic(samples, sampleRate);
}

export function percussive(samples: Float32Array, sampleRate = 22050): Float32Array {
  return addon.percussive(samples, sampleRate);
}

export function timeStretch(samples: Float32Array, rate: number, sampleRate = 22050): Float32Array {
  if (typeof rate !== 'number' || !Number.isFinite(rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
  return addon.timeStretch(samples, sampleRate, rate);
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
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
  ops: SpectralRegionOp[] = [],
  options: SpectralEditOptions = {},
): Float32Array {
  return addon.spectralEdit(samples, sampleRate, ops, options);
}

export function pitchShift(
  samples: Float32Array,
  semitones: number,
  sampleRate = 22050,
): Float32Array {
  if (typeof semitones !== 'number' || !Number.isFinite(semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
  return addon.pitchShift(samples, sampleRate, semitones);
}

export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate = 22050,
  currentMidi = 69.0,
  targetMidi = 69.0,
): Float32Array {
  return addon.pitchCorrectToMidi(samples, sampleRate, currentMidi, targetMidi);
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
  samples: Float32Array,
  f0Hz: Float32Array,
  targetMidi: number,
  sampleRate = 22050,
  hopLength = 512,
  voiced?: Int32Array,
  voicedProb?: Float32Array,
): Float32Array {
  return addon.pitchCorrectToMidiTimevarying(
    samples,
    sampleRate,
    f0Hz,
    targetMidi,
    hopLength,
    voiced,
    voicedProb,
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
export function pitchCorrectTimevarying(
  samples: Float32Array,
  f0Hz: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  options: PitchCorrectOptions = {},
): Float32Array {
  return addon.pitchCorrectTimevarying(samples, sampleRate, f0Hz, hopLength, options);
}

export function noteStretch(
  samples: Float32Array,
  sampleRate = 22050,
  options: NoteStretchOptions = {},
): Float32Array {
  return addon.noteStretch(
    samples,
    sampleRate,
    options.onsetSample ?? 0,
    options.offsetSample ?? 0,
    options.stretchRatio ?? 1.0,
  );
}
