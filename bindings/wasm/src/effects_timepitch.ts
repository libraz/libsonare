/**
 * Time and pitch transforms over a whole buffer: stretching, shifting, and
 * correction onto a target pitch.
 */

import { keyModeName, keyModeOrdinal, pitchClassOrdinal } from './analysis_helpers.js';
import { assertPitchTrackLengths, toVoicedFloat32 } from './internal/effects_common.js';
import { resolveEffectFftOptions } from './internal/fft_options.js';
import { getSonareModule } from './module_state.js';
import type { AutoTuneOptions, AutoTuneResult, PitchCorrectOptions } from './public_types.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFloatValue,
  assertPositiveFloatValue,
  sampleOneShot,
} from './validation.js';

function requireModule() {
  return getSonareModule();
}

/**
 * Time-stretch request. `sampleRate` is required (Hz); `speedRatio` is required.
 * A missing field is a `TypeError`, a non-finite or out-of-range value a `RangeError`.
 */
export interface TimeStretchRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /**
   * Playback speed: `> 1` is faster and shorter (2 halves the duration), `< 1` is
   * slower and longer (0.5 doubles it). The opposite sense of a note's
   * `stretchRatio`, which is a duration ratio.
   */
  speedRatio: number;
  /** FFT size: an even integer >= 4. Default 2048. */
  nFft?: number;
  /** Hop in samples, in `(0, nFft / 2]`. Default `nFft / 4`, rounded down. */
  hopLength?: number;
}

/** Pitch-shift request. `sampleRate` (Hz) and `pitchSemitones` are required. */
export interface PitchShiftRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** Shift in semitones: +12 is one octave up, -12 one octave down. */
  pitchSemitones: number;
  /** FFT size: an even integer >= 4. Default 2048. */
  nFft?: number;
  /** Hop in samples, in `(0, nFft / 2]`. Default `nFft / 4`, rounded down. */
  hopLength?: number;
}

/** Constant-interval correction request. `sampleRate` (Hz), `currentMidi` and `targetMidi` are required. */
export interface PitchCorrectToMidiRequest {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required. */
  sampleRate: number;
  /** MIDI note the audio currently sits on, in `[0, 127]`. */
  currentMidi: number;
  /** MIDI note to move it to, in `[0, 127]`. */
  targetMidi: number;
}

/** Contour-following correction request; the fields are {@link PitchCorrectOptions} plus the audio. */
export interface PitchCorrectTimevaryingRequest extends PitchCorrectOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required. */
  sampleRate: number;
}

/** Auto-tune request; the fields are {@link AutoTuneOptions} plus the audio. `sampleRate` (Hz) is required. */
export interface AutoTuneRequest extends AutoTuneOptions {
  /** Mono input audio. */
  samples: Float32Array;
  /** Sample rate in Hz. Required. */
  sampleRate: number;
}

/**
 * Time-stretch audio without changing pitch.
 *
 * `options.nFft` is an even integer >= 4 (default 2048); `options.hopLength` is
 * in `(0, nFft / 2]` (default `nFft / 4`, rounded down). `speedRatio` is required.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - `speedRatio` (0.5 = double duration, 2.0 = half duration) and the FFT framing
 * @returns Time-stretched audio
 * @throws TypeError for a missing `sampleRate` or `speedRatio`, RangeError for an out-of-domain one
 */
export function timeStretch(request: TimeStretchRequest): Float32Array;
export function timeStretch(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<TimeStretchRequest>,
): Float32Array;
export function timeStretch(...args: unknown[]): Float32Array {
  const r = sampleOneShot<TimeStretchRequest>('timeStretch', args);
  assertAudioInput('timeStretch', r.samples, r.sampleRate, r);
  assertPositiveFloatValue('timeStretch', r.speedRatio, 'speedRatio');
  const fftOptions = resolveEffectFftOptions('timeStretch', r.nFft, r.hopLength);
  return requireModule().timeStretchEx(
    r.samples,
    r.sampleRate,
    r.speedRatio,
    fftOptions.nFft,
    fftOptions.hopLength,
  );
}

/**
 * Pitch-shift audio without changing duration.
 *
 * `options.nFft` is an even integer >= 4 (default 2048); `options.hopLength` is
 * in `(0, nFft / 2]` (default `nFft / 4`, rounded down). `pitchSemitones` is required.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - `pitchSemitones` (+12 = one octave up) and the FFT framing
 * @returns Pitch-shifted audio
 * @throws TypeError for a missing `sampleRate` or `pitchSemitones`, RangeError for an out-of-domain one
 */
export function pitchShift(request: PitchShiftRequest): Float32Array;
export function pitchShift(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<PitchShiftRequest>,
): Float32Array;
export function pitchShift(...args: unknown[]): Float32Array {
  const r = sampleOneShot<PitchShiftRequest>('pitchShift', args);
  assertAudioInput('pitchShift', r.samples, r.sampleRate, r);
  assertFloatValue('pitchShift', r.pitchSemitones, 'pitchSemitones');
  const fftOptions = resolveEffectFftOptions('pitchShift', r.nFft, r.hopLength);
  return requireModule().pitchShiftEx(
    r.samples,
    r.sampleRate,
    r.pitchSemitones,
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
 * {@link pitchCorrectTimevarying} for a caller-supplied pitch contour.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - `currentMidi` and `targetMidi`, both required
 * @returns Pitch-corrected audio
 * @throws TypeError for a missing `sampleRate`, `currentMidi` or `targetMidi`
 */
export function pitchCorrectToMidi(request: PitchCorrectToMidiRequest): Float32Array;
export function pitchCorrectToMidi(
  samples: Float32Array,
  sampleRate: number,
  options: OptionsOf<PitchCorrectToMidiRequest>,
): Float32Array;
export function pitchCorrectToMidi(...args: unknown[]): Float32Array {
  const r = sampleOneShot<PitchCorrectToMidiRequest>('pitchCorrectToMidi', args);
  assertAudioInput('pitchCorrectToMidi', r.samples, r.sampleRate, r);
  assertFloatValue('pitchCorrectToMidi', r.currentMidi, 'currentMidi');
  assertFloatValue('pitchCorrectToMidi', r.targetMidi, 'targetMidi');
  return requireModule().pitchCorrectToMidi(r.samples, r.sampleRate, r.currentMidi, r.targetMidi);
}

/**
 * Contour-following ("time-varying") pitch correction toward a fixed MIDI note
 * OR a musical scale, with tunable retune strength and vibrato preservation.
 *
 * It follows the caller-supplied per-frame `f0Hz` contour and retunes every
 * voiced frame, so vibrato/drift in the source is tracked rather than flattened.
 * `targetMode` selects a fixed-MIDI target (`'midi'`, default; `targetMidi`) or
 * scale quantisation (`'scale'`). For a constant transpose use
 * {@link pitchCorrectToMidi}. When `voiced` is supplied (truthy = voiced), it
 * takes precedence over `voicedProb`; omitting it or passing `null` uses the
 * probability array. When both are omitted, every frame is treated as voiced.
 * An `f0Hz` NaN is accepted only for a frame marked unvoiced, matching pYIN
 * output; the `voicedFlag` / `voicedProb` arrays of a {@link PitchResult} can be
 * passed through directly.
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - `f0Hz` (required), `hopLength` (F0 frame-center spacing in
 *   samples, default 512), target mode and retune knobs
 * @returns Pitch-corrected audio
 * @throws TypeError for a missing `sampleRate` or `f0Hz`, RangeError for an unknown `targetMode`
 */
export function pitchCorrectTimevarying(request: PitchCorrectTimevaryingRequest): Float32Array;
export function pitchCorrectTimevarying(
  samples: Float32Array,
  sampleRate: number,
  options: PitchCorrectOptions,
): Float32Array;
export function pitchCorrectTimevarying(...args: unknown[]): Float32Array {
  const r = sampleOneShot<PitchCorrectTimevaryingRequest>('pitchCorrectTimevarying', args);
  assertAudioInput('pitchCorrectTimevarying', r.samples, r.sampleRate, r);
  if (!(r.f0Hz instanceof Float32Array)) {
    throw new TypeError('pitchCorrectTimevarying: f0Hz must be a Float32Array');
  }
  const voicedF32 =
    r.voiced == null ? undefined : toVoicedFloat32('pitchCorrectTimevarying', r.voiced);
  assertPitchTrackLengths('pitchCorrectTimevarying', r.f0Hz, r.voiced, r.voicedProb);
  const {
    samples: _samples,
    sampleRate: _sampleRate,
    f0Hz: _f0Hz,
    hopLength,
    voiced: _voiced,
    voicedProb,
    ...rest
  } = r;
  const nativeOptions = {
    ...rest,
    voiced: voicedF32,
    voicedProb: r.voiced == null ? (voicedProb ?? undefined) : undefined,
  };
  return requireModule().pitchCorrectTimevarying(
    r.samples,
    r.sampleRate,
    r.f0Hz,
    hopLength ?? 512,
    nativeOptions,
  );
}

/**
 * Snaps the voiced pitch of a monophonic recording to a scale, offline.
 *
 * Chains key detection (when `key` is `'detect'`), `scaleMaskForMode`, pYIN
 * pitch tracking (2048-sample frames, 512-sample hop) and the time-varying scale
 * corrector, so the options are `pitchCorrectTimevarying`'s with `strength` as
 * `retuneAmount`. The output has the input's length. Unvoiced stretches and
 * notes the corrector cannot repitch pass through unchanged.
 *
 * `key` is `'detect'` (default) or a `{ root, mode }` as `detectKey`'s result
 * has it: `root` an integer pitch class `0..11` (`RangeError` otherwise,
 * `TypeError` for a non-number), `mode` a {@link KeyMode} token (`RangeError`
 * for an unknown one, `TypeError` for a non-string).
 *
 * @example
 * const { samples: tuned, key } = autoTune({ samples, sampleRate: 44100, strength: 0.8 });
 *
 * @param samples - Audio samples (mono, float32)
 * @param sampleRate - Sample rate in Hz
 * @param options - The key and the tuning knobs
 * @returns The corrected audio and the key it was tuned to
 * @throws TypeError for a missing `sampleRate` or a malformed `key`
 */
export function autoTune(request: AutoTuneRequest): AutoTuneResult;
export function autoTune(
  samples: Float32Array,
  sampleRate: number,
  options?: AutoTuneOptions,
): AutoTuneResult;
export function autoTune(...args: unknown[]): AutoTuneResult {
  const r = sampleOneShot<AutoTuneRequest>('autoTune', args);
  assertAudioInput('autoTune', r.samples, r.sampleRate, r);
  const { samples, sampleRate, key, ...options } = r;
  const keyOrDetect = key ?? 'detect';
  if (keyOrDetect !== 'detect' && typeof keyOrDetect !== 'object') {
    throw new TypeError("autoTune: key must be 'detect' or { root, mode }");
  }
  let nativeKey: { root: number; mode: string } | null = null;
  if (keyOrDetect !== 'detect') {
    // Checked for the refusal only; native reads the pitch class and mode token as given.
    pitchClassOrdinal('autoTune', keyOrDetect.root, 'key.root');
    keyModeOrdinal(keyOrDetect.mode, 'key.mode');
    nativeKey = { root: keyOrDetect.root, mode: keyOrDetect.mode };
  }
  const result = requireModule().autoTune(samples, sampleRate, nativeKey, options) as Omit<
    AutoTuneResult,
    'key'
  > & {
    key: Omit<AutoTuneResult['key'], 'mode'> & { mode: number };
  };
  return { ...result, key: { ...result.key, mode: keyModeName(result.key.mode) } };
}
