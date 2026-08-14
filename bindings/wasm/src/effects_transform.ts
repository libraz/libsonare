import { getSonareModule } from './module_state';
import type {
  HpssResult,
  NoteMoveOptions,
  NoteStretchOptions,
  PitchCorrectOptions,
  SpectralEditOptions,
  SpectralRegionOp,
  VoicedFlags,
} from './public_types';
import type { ValidateOptions } from './validation';
import { assertSampleRate, assertSamples } from './validation';

function requireModule() {
  return getSonareModule();
}

// The embind layer reads the companion voicing array as Float32Array. A flag is
// a decision, not a magnitude: collapse to 1/0 on truthiness, which is the same
// reduction the Node facade applies, so both surfaces agree on every accepted
// input type.
function toVoicedFloat32(voiced: VoicedFlags): Float32Array {
  const out = new Float32Array(voiced.length);
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
  if (resolvedNFft < 2 || resolvedNFft > 2 ** 30) {
    throw new RangeError(`${fnName}: nFft must be an even power of two >= 2`);
  }
  if ((resolvedNFft & (resolvedNFft - 1)) !== 0) {
    throw new RangeError(`${fnName}: nFft must be an even power of two >= 2`);
  }
  if (typeof resolvedHopLength !== 'number' || !Number.isInteger(resolvedHopLength)) {
    throw new TypeError(`${fnName}: hopLength must be an integer`);
  }
  if (resolvedHopLength <= 0 || resolvedHopLength > 2 ** 31 - 1) {
    throw new RangeError(`${fnName}: hopLength must be a positive integer`);
  }
  return { nFft: resolvedNFft, hopLength: resolvedHopLength };
}

export type NormalizeMode = 'peak' | 'rms';

function resolveNormalizeMode(value: unknown): NormalizeMode {
  if (value === undefined) {
    return 'peak';
  }
  if (typeof value !== 'string') {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  if (value !== 'peak' && value !== 'rms') {
    throw new RangeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  return value;
}

function resolveHardMask(value: unknown, fnName: string): boolean {
  if (value === undefined) {
    return false;
  }
  if (typeof value !== 'boolean') {
    throw new TypeError(`${fnName}: hardMask must be a boolean`);
  }
  return value;
}

/** Canonical request form for HPSS. */
export interface HpssRequest {
  samples: Float32Array;
  sampleRate?: number;
  kernelHarmonic?: number;
  kernelPercussive?: number;
  nFft?: number;
  hopLength?: number;
  hardMask?: boolean;
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
  mode?: NormalizeMode;
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
  const resolvedHardMask = resolveHardMask(request.hardMask, 'hpss');
  return requireModule().hpssEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.kernelHarmonic ?? 31,
    request.kernelPercussive ?? 31,
    fftOptions.nFft,
    fftOptions.hopLength,
    resolvedHardMask,
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
  const fftOptions = resolveEffectFftOptions('timeStretch', request.nFft, request.hopLength);
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
  const fftOptions = resolveEffectFftOptions('pitchShift', request.nFft, request.hopLength);
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
 * the input buffer length. Use {@link pitchCorrectToMidiTimevarying} for a
 * caller-supplied pitch contour.
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

/**
 * Time-stretch a note region between two sample offsets without changing pitch.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param onsetSample - Note onset position in samples
 * @param offsetSample - Note offset position in samples
 * @param stretchRatio - Stretch ratio (0.5 = half duration, 2.0 = double duration)
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
    request.offsetSample ?? request.samples.length,
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
    request.offsetSample ?? request.samples.length,
    request.targetOnsetSample ?? 0,
  );
}

/**
 * Normalize audio to a target peak or RMS level.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz (default: 22050)
 * @param targetDb - Finite target at or below 0 dBFS (default: 0 dB = full scale).
 *   For `mode: 'peak'`, this is the peak target; for `mode: 'rms'`, this is the RMS target.
 * @param mode - Normalization mode: `'peak'` (default) or `'rms'`.
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
  samples: Float32Array,
  sampleRate: number,
  targetDb?: number,
  mode?: NormalizeMode,
  options?: ValidateOptions,
): Float32Array;
export function normalize(
  samples: Float32Array | NormalizeRequest,
  sampleRate?: number,
  targetDb = 0.0,
  modeOrOptions: NormalizeMode | ValidateOptions = 'peak',
  options: ValidateOptions = {},
): Float32Array {
  if (
    modeOrOptions !== undefined &&
    modeOrOptions !== null &&
    typeof modeOrOptions !== 'string' &&
    typeof modeOrOptions !== 'object'
  ) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  if (modeOrOptions === null) {
    throw new TypeError("normalize: mode must be the string 'peak' or 'rms'");
  }
  const positionalOptions =
    typeof modeOrOptions === 'object' && modeOrOptions !== null ? modeOrOptions : options;
  const positionalMode = typeof modeOrOptions === 'string' ? modeOrOptions : undefined;
  const request: NormalizeRequest =
    samples instanceof Float32Array
      ? { samples, sampleRate, targetDb, mode: positionalMode, ...positionalOptions }
      : samples;
  assertSamples('normalize', request.samples, request.validate !== false);
  const mode = resolveNormalizeMode(request.mode);
  return requireModule().normalizeEx(
    request.samples,
    request.sampleRate ?? 22050,
    request.targetDb ?? 0.0,
    mode,
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
