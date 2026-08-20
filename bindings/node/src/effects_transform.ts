import { addon } from './native.js';
import type {
  HpssResult,
  NoteMoveOptions,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
  VoicedFlags,
} from './types.js';
import { assertSampleRate } from './validation.js';

// The addon reads the companion voicing array as an Int32Array and silently
// ignores any other type, so normalize here rather than at the N-API boundary.
// A flag is a decision, not a magnitude: collapse to 1/0 on truthiness, which
// is the same reduction the WASM facade applies, so both surfaces agree on
// every accepted input type.
function toVoicedInt32(voiced: VoicedFlags): Int32Array {
  const out = new Int32Array(voiced.length);
  for (let index = 0; index < voiced.length; index += 1) {
    out[index] = voiced[index] ? 1 : 0;
  }
  return out;
}

function resolveEffectFftOptions(
  fnName: string,
  nFft: unknown,
  hopLength: unknown,
): { nFft: number; hopLength: number } {
  const resolvedNFft = nFft === undefined ? 2048 : nFft;
  const resolvedHopLength = hopLength === undefined ? 512 : hopLength;
  if (typeof resolvedNFft !== 'number' || !Number.isInteger(resolvedNFft)) {
    throw new TypeError(`${fnName}: nFft must be an integer`);
  }
  // The core FFT is mixed-radix, so any even size transforms exactly; only the
  // real one-sided spectrum's n_fft/2 + 1 bin layout needs the evenness. A
  // power-of-two restriction here would reject sizes the C ABI and the native
  // CLI accept.
  if (resolvedNFft < 2 || resolvedNFft > 2 ** 30 || resolvedNFft % 2 !== 0) {
    throw new RangeError(`${fnName}: nFft must be an even integer >= 2`);
  }
  if (typeof resolvedHopLength !== 'number' || !Number.isInteger(resolvedHopLength)) {
    throw new TypeError(`${fnName}: hopLength must be an integer`);
  }
  if (resolvedHopLength <= 0 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: hopLength must be a positive integer`);
  }
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}

function resolveHardMask(fnName: string, hardMask: unknown): boolean {
  if (hardMask === undefined) {
    return false;
  }
  if (typeof hardMask !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return hardMask;
}

/** Common audio input fields for stateless effect requests. */
export interface EffectSamplesRequest {
  samples: Float32Array;
  sampleRate?: number;
}

export interface HpssRequest extends EffectSamplesRequest {
  kernelHarmonic?: number;
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
}

export interface TimeStretchRequest extends EffectSamplesRequest {
  rate: number;
  nFft?: number;
  hopLength?: number;
}

export interface SpectralEditRequest extends EffectSamplesRequest, SpectralEditOptions {
  /**
   * Sample rate in Hz. Required: region frequency boundaries are mapped to STFT
   * bins using this rate, so a wrong/omitted value silently corrupts the edit
   * (unlike the other effects, this has no safe default).
   */
  sampleRate: number;
  ops?: SpectralRegionOp[];
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

export interface NoteStretchRequest extends EffectSamplesRequest, NoteStretchOptions {}
export interface NoteMoveRequest extends EffectSamplesRequest, NoteMoveOptions {}

function assertPitchTrackLengths(
  f0Hz: Float32Array,
  voiced?: VoicedFlags,
  voicedProb?: Float32Array,
): void {
  if (voiced !== undefined && voiced.length !== f0Hz.length) {
    throw new RangeError('voiced must have the same length as f0Hz');
  }
  if (voicedProb !== undefined && voicedProb.length !== f0Hz.length) {
    throw new RangeError('voicedProb must have the same length as f0Hz');
  }
}

// -- Effects --

export function hpss(request: HpssRequest): HpssResult;
export function hpss(
  samples: Float32Array,
  sampleRate?: number,
  kernelHarmonic?: number,
  kernelPercussive?: number,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult;
export function hpss(
  samples: Float32Array | HpssRequest,
  sampleRate = 22050,
  kernelHarmonic = 31,
  kernelPercussive = 31,
  nFft?: number,
  hopLength?: number,
  hardMask?: boolean,
): HpssResult {
  const request =
    samples instanceof Float32Array
      ? { samples, sampleRate, kernelHarmonic, kernelPercussive, nFft, hopLength, hardMask }
      : samples;
  const fftOptions = resolveEffectFftOptions('hpss', request.nFft, request.hopLength);
  const resolvedHardMask = resolveHardMask('hpss', request.hardMask);
  return addon.hpss(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
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
  if (typeof request.rate !== 'number' || !Number.isFinite(request.rate)) {
    throw new TypeError('timeStretch: rate must be a finite number');
  }
  const fftOptions = resolveEffectFftOptions('timeStretch', request.nFft, request.hopLength);
  return addon.timeStretch(
    request.samples,
    request.sampleRate ?? 22050,
    request.rate,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
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
 * @param sampleRate - Sample rate in Hz (required; region frequency boundaries
 *   are mapped to STFT bins using this rate, so a wrong/omitted value silently
 *   corrupts the edit).
 * @param ops - Region ops applied in order.
 * @param options - Optional STFT + heal configuration.
 * @returns The edited audio (same length as `samples`).
 */
export function spectralEdit(request: SpectralEditRequest): Float32Array;
export function spectralEdit(
  samples: Float32Array,
  sampleRate: number,
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
  assertSampleRate('spectralEdit', requestSampleRate as number);
  return addon.spectralEdit(input, requestSampleRate as number, requestOps, requestOptions);
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
  if (typeof request.semitones !== 'number' || !Number.isFinite(request.semitones)) {
    throw new TypeError('pitchShift: semitones must be a finite number');
  }
  const fftOptions = resolveEffectFftOptions('pitchShift', request.nFft, request.hopLength);
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
 * The result has exactly the input length. Use {@link pitchCorrectToMidiTimevarying}
 * for a caller-supplied pitch contour and retune glide.
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
  return addon.pitchCorrectToMidiTimevarying(
    request.samples,
    request.sampleRate ?? 22050,
    request.f0Hz,
    request.targetMidi,
    request.hopLength ?? 512,
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
  return addon.pitchCorrectTimevarying(
    input,
    requestSampleRate ?? 22050,
    requestF0Hz,
    requestHopLength ?? 512,
    {
      ...requestOptions,
      voiced: requestOptions.voiced ? toVoicedInt32(requestOptions.voiced) : undefined,
    },
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
    request.offsetSample ?? request.samples.length,
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
    request.offsetSample ?? request.samples.length,
    request.targetOnsetSample ?? 0,
  );
}
