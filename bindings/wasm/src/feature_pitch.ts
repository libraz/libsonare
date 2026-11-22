import { toVoicedFloat32 } from './internal/effects_common.js';
import { refuse, refuseType, withEntry } from './internal/entry.js';
import { getSonareModule } from './module_state.js';
import type { NoteSegment, PiptrackResult, PitchResult } from './public_types.js';
import type { VoicedFlags } from './public_types_spectral.js';
import type { OptionsOf } from './validation.js';
import {
  assertAudioInput,
  assertFloat32Array,
  assertFrequencyBounds,
  requestOnly,
  sampleOneShot,
  toMatrix2D,
  tuningCentsFromNative,
} from './validation.js';

// ============================================================================
// Features - Pitch
// ============================================================================

/** YIN pitch request. */
export interface PitchYinRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Lowest pitch searched, in Hz. Default 65. */
  f0MinHz?: number;
  /** Highest pitch searched, in Hz. Default 2093. */
  f0MaxHz?: number;
  /** YIN threshold. Default 0.1. */
  threshold?: number;
}

/** pYIN pitch request. */
export interface PitchPyinRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Frame length in samples. Default 2048. */
  frameLength?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Lowest pitch searched, in Hz. Default 65. */
  f0MinHz?: number;
  /** Highest pitch searched, in Hz. Default 2093. */
  f0MaxHz?: number;
  /** YIN threshold. Default 0.1. */
  threshold?: number;
  /** Return 0 for unvoiced f0 frames instead of NaN. Default `false`. */
  fillNa?: boolean;
}

/** Spectral pitch-candidate request. */
export interface PiptrackRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
  /** Lowest frequency considered, in Hz. Default 150. */
  fminHz?: number;
  /** Highest frequency considered, in Hz. Default 4000. */
  fmaxHz?: number;
  /** Magnitude threshold relative to the column maximum. Default 0.1. */
  threshold?: number;
}

/**
 * Per-bin spectral pitch candidates and peak magnitudes (librosa.piptrack).
 *
 * @returns Result whose matrices have FFT bins on rows and frames on columns.
 */
export function piptrack(request: PiptrackRequest): PiptrackResult;
export function piptrack(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PiptrackRequest>,
): PiptrackResult;
export function piptrack(...args: unknown[]): PiptrackResult {
  return withEntry('piptrack', () => {
    const r = sampleOneShot<PiptrackRequest>('piptrack', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const fminHz = r.fminHz ?? 150;
    const fmaxHz = r.fmaxHz ?? 4000;
    assertFrequencyBounds(fminHz, fmaxHz, 'fminHz', 'fmaxHz');
    const native = getSonareModule().piptrack(
      r.samples,
      r.sampleRate,
      r.nFft ?? 2048,
      r.hopLength ?? 512,
      fminHz,
      fmaxHz,
      r.threshold ?? 0.1,
    );
    return {
      pitches: toMatrix2D(native.pitches, native.nBins, native.nFrames),
      magnitudes: toMatrix2D(native.magnitudes, native.nBins, native.nFrames),
    };
  });
}

// The frame count is the length of every per-frame array; the result does not repeat it.
function toPitchResult(native: PitchResult & { nFrames: number }): PitchResult {
  const { nFrames: _nFrames, ...result } = native;
  return result;
}

/** Detect pitch using the YIN algorithm. */
export function pitchYin(request: PitchYinRequest): PitchResult;
export function pitchYin(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PitchYinRequest>,
): PitchResult;
export function pitchYin(...args: unknown[]): PitchResult {
  return withEntry('pitchYin', () => {
    const r = sampleOneShot<PitchYinRequest>('pitchYin', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const f0MinHz = r.f0MinHz ?? 65.0;
    const f0MaxHz = r.f0MaxHz ?? 2093.0;
    assertFrequencyBounds(f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
    return toPitchResult(
      getSonareModule().pitchYin(
        r.samples,
        r.sampleRate,
        r.frameLength ?? 2048,
        r.hopLength ?? 512,
        f0MinHz,
        f0MaxHz,
        r.threshold ?? 0.1,
      ),
    );
  });
}

/** Detect pitch using pYIN (probabilistic YIN with HMM smoothing). */
export function pitchPyin(request: PitchPyinRequest): PitchResult;
export function pitchPyin(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<PitchPyinRequest>,
): PitchResult;
export function pitchPyin(...args: unknown[]): PitchResult {
  return withEntry('pitchPyin', () => {
    const r = sampleOneShot<PitchPyinRequest>('pitchPyin', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const f0MinHz = r.f0MinHz ?? 65.0;
    const f0MaxHz = r.f0MaxHz ?? 2093.0;
    assertFrequencyBounds(f0MinHz, f0MaxHz, 'f0MinHz', 'f0MaxHz');
    return toPitchResult(
      getSonareModule().pitchPyin(
        r.samples,
        r.sampleRate,
        r.frameLength ?? 2048,
        r.hopLength ?? 512,
        f0MinHz,
        f0MaxHz,
        r.threshold ?? 0.1,
        r.fillNa ?? false,
      ),
    );
  });
}

/**
 * Parameters for segmenting an F0 track into stable monophonic notes. Give
 * `voiced` or `voicedProb`; `voiced` takes precedence.
 */
export interface NoteSegmentsRequest {
  /** Per-frame F0 track in Hz, one value per frame, at `frameRate` frames per second. */
  f0Hz: Float32Array;
  /**
   * Per-frame voicing values in `[0, 1]`; anything below `voicedThreshold` is
   * unvoiced.
   *
   * Read only when `voiced` is omitted. Pass {@link PitchResult.voicedFlag} as
   * `voiced` instead. Do **not** pass {@link pitchPyin}'s `voicedProb`: that
   * value is the frame's voiced observation mass and rises with F0 for a fixed
   * `frameLength`, so a fixed threshold silently returns no segments at all for
   * low-register material (a steady tone below roughly C5 never reaches 0.5).
   */
  voicedProb?: Float32Array;
  /**
   * Per-frame voiced flags (truthy = voiced), the same length as `f0Hz`. Takes
   * precedence over `voicedProb`; `voicedThreshold` does not apply.
   */
  voiced?: VoicedFlags;
  /** Frames per second of the F0 track. */
  frameRate: number;
  /** Cents of pitch change that start a new segment. Default 50. */
  segmentationThresholdCents?: number;
  /** Shortest segment kept, in ms. Default 30. */
  minNoteMs?: number;
  /** Reference pitch each segment's cents are measured against. Default 440. */
  referenceHz?: number;
  /** Voicing threshold applied to `voicedProb`; defaults to `0.5`. */
  voicedThreshold?: number;
}

/**
 * Segment a caller-supplied monophonic F0 track into stable note regions.
 *
 * `f0Hz` and the voicing input (`voiced`, or `voicedProb` when `voiced` is
 * omitted) must have the same non-zero length. Zero-Hz frames and unvoiced frames (a `voicedProb`
 * below `voicedThreshold`, default `0.5`, or a zero flag) are treated as unvoiced.
 */
export function noteSegments(request: NoteSegmentsRequest): NoteSegment[];
export function noteSegments(...args: unknown[]): NoteSegment[] {
  return withEntry('noteSegments', () => {
    const r = requestOnly<NoteSegmentsRequest>('noteSegments', args);
    if (r.voiced == null && r.voicedProb == null) {
      throw refuseType('give voiced or voicedProb');
    }
    // The embind reader takes 0/1 bytes; voiced wins, so voicedProb is not sent beside it.
    const flags = r.voiced != null ? Uint8Array.from(toVoicedFloat32(r.voiced)) : null;
    const prob = flags === null ? r.voicedProb : null;
    if (prob != null && !(prob instanceof Float32Array)) {
      throw refuseType('voicedProb must be a Float32Array');
    }
    const voicing = (flags ?? prob) as Float32Array | Uint8Array;
    if (r.f0Hz?.length !== voicing.length) {
      throw refuse(`${flags ? 'voiced' : 'voicedProb'} must have the same length as f0Hz`);
    }
    return getSonareModule().noteSegments(r.f0Hz, prob ?? null, flags, r.frameRate, {
      segmentationThresholdCents: r.segmentationThresholdCents,
      minNoteMs: r.minNoteMs,
      referenceHz: r.referenceHz,
      voicedThreshold: r.voicedThreshold,
    });
  });
}

// ============================================================================
// Features - Tuning
// ============================================================================

/** Tuning-offset request over a set of frequencies. */
export interface PitchTuningRequest {
  /** Frequencies in Hz to estimate the tuning offset from. */
  frequencies: Float32Array;
  /** Resolution of the search, as a fraction of a bin. Default 0.01. */
  resolution?: number;
  /** Bins per octave the offset is measured in. Default 12. */
  binsPerOctave?: number;
}

/** Tuning-offset request over an audio signal. */
export interface EstimateTuningRequest {
  /** Mono audio samples. */
  samples: Float32Array;
  /** Sample rate of `samples` in Hz. */
  sampleRate: number;
  /** Resolution of the search, as a fraction of a bin. Default 0.01. */
  resolution?: number;
  /** Bins per octave the offset is measured in. Default 12. */
  binsPerOctave?: number;
  /** FFT size. Default 2048. */
  nFft?: number;
  /** Hop in samples. Default 512. */
  hopLength?: number;
}

/**
 * Estimate the global tuning offset from a set of frequencies
 * (librosa.pitch_tuning).
 *
 * @returns The offset in cents of a semitone, in `[-50, 50)` at 12 bins per octave.
 */
export function pitchTuning(request: PitchTuningRequest): number;
export function pitchTuning(...args: unknown[]): number {
  return withEntry('pitchTuning', () => {
    const r = requestOnly<PitchTuningRequest>('pitchTuning', args);
    assertFloat32Array(r.frequencies, 'frequencies');
    const binsPerOctave = r.binsPerOctave ?? 12;
    const bins = getSonareModule().pitchTuning(r.frequencies, r.resolution ?? 0.01, binsPerOctave);
    return tuningCentsFromNative((bins * 12) / binsPerOctave);
  });
}

/**
 * Reference frequency of an A4 raised by `tuningCents`: `a4Hz * 2 ** (tuningCents / 1200)`.
 * The converter from the analysis unit (`tuningCents`) to the Hz a pitch
 * reference is stated in, such as `referenceHz` and the streaming `referenceHz`.
 *
 * @param tuningCents - Finite offset in cents.
 * @param a4Hz - Finite positive concert pitch the tuning is measured from. Default 440.
 * @throws `RangeError` for a non-finite `tuningCents`, `SonareError` for a non-positive `a4Hz`.
 */
export function tuningToReferenceHz(tuningCents: number, a4Hz = 440): number {
  return withEntry('tuningToReferenceHz', () => {
    return getSonareModule().tuningToReferenceHz(tuningCents, a4Hz);
  });
}

/**
 * Tuning, in cents, of a recording whose A4 sits at `referenceHz`:
 * `1200 * log2(referenceHz / a4Hz)`. Inverse of {@link tuningToReferenceHz}.
 *
 * @param referenceHz - Finite positive reference frequency.
 * @param a4Hz - Finite positive concert pitch. Default 440.
 * @throws `RangeError` for a non-finite `referenceHz` or `a4Hz`, `SonareError` for a non-positive one.
 */
export function referenceHzToTuning(referenceHz: number, a4Hz = 440): number {
  return withEntry('referenceHzToTuning', () => {
    return getSonareModule().referenceHzToTuning(referenceHz, a4Hz);
  });
}

/**
 * Estimate the tuning offset of an audio signal (librosa.estimate_tuning).
 *
 * @returns The offset in cents of a semitone, in `[-50, 50)` at 12 bins per octave.
 *   Analysis measures the same quantity itself with `tuningCents: 'auto'`, and
 *   {@link tuningToReferenceHz} converts it to Hz.
 */
export function estimateTuning(request: EstimateTuningRequest): number;
export function estimateTuning(
  samples: Float32Array,
  sampleRate: number,
  options?: OptionsOf<EstimateTuningRequest>,
): number;
export function estimateTuning(...args: unknown[]): number {
  return withEntry('estimateTuning', () => {
    const r = sampleOneShot<EstimateTuningRequest>('estimateTuning', args);
    assertAudioInput(r.samples, r.sampleRate, r);
    const binsPerOctave = r.binsPerOctave ?? 12;
    const bins = getSonareModule().estimateTuning(
      r.samples,
      r.sampleRate,
      r.nFft ?? 2048,
      r.hopLength ?? 512,
      r.resolution ?? 0.01,
      binsPerOctave,
    );
    return tuningCentsFromNative((bins * 12) / binsPerOctave);
  });
}
