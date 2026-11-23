import { toVoicedFloat32 } from './internal/effects_common.js';
import { refuseType, withEntry } from './internal/entry.js';
import { sampleNeed, stftBins, stftFrames, withHeapNeed } from './internal/memory_budget.js';
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

/** Whether `value` is an integer of at least `min`. */
function isIntegerAtLeast(value: unknown, min: number): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value >= min;
}

/**
 * Runs a spectral pitch call under the heap guard: the heap copy of the input, then the complex
 * spectrogram, its magnitude cache and the two result matrices held together. A size or hop
 * outside the core's domain skips the guard so the native refusal stays the one that speaks.
 */
function guardedPeakTrack<T>(
  samples: Float32Array,
  sampleRate: number,
  nFft: unknown,
  hop: unknown,
  call: () => T,
): T {
  const bins = stftBins(nFft, hop);
  if (bins === undefined) {
    return call();
  }
  return withHeapNeed(
    sampleNeed(
      'samples',
      samples.length,
      sampleRate,
      (n) => 4 * n + 20 * bins * stftFrames(n, hop as number),
    ),
    call,
  );
}

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
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const native = guardedPeakTrack(r.samples, r.sampleRate, nFft, hopLength, () =>
      getSonareModule().piptrack(
        r.samples,
        r.sampleRate,
        nFft,
        hopLength,
        fminHz,
        fmaxHz,
        r.threshold ?? 0.1,
      ),
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
    const frameLength = r.frameLength ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const call = () =>
      getSonareModule().pitchYin(
        r.samples,
        r.sampleRate,
        frameLength,
        hopLength,
        f0MinHz,
        f0MaxHz,
        r.threshold ?? 0.1,
      );
    if (!isIntegerAtLeast(frameLength, 1) || !isIntegerAtLeast(hopLength, 1)) {
      return toPitchResult(call());
    }
    // The input copy, its centered padded copy, and the f0 and voiced-probability tracks.
    return toPitchResult(
      withHeapNeed(
        sampleNeed(
          'samples',
          r.samples.length,
          r.sampleRate,
          (n) => 4 * n + 4 * (n + frameLength) + 8 * (1 + Math.floor(n / hopLength)),
        ),
        call,
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
    const frameLength = r.frameLength ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const call = () =>
      getSonareModule().pitchPyin(
        r.samples,
        r.sampleRate,
        frameLength,
        hopLength,
        f0MinHz,
        f0MaxHz,
        r.threshold ?? 0.1,
        r.fillNa ?? false,
      );
    if (!isIntegerAtLeast(frameLength, 1) || !isIntegerAtLeast(hopLength, 1)) {
      return toPitchResult(call());
    }
    // The core returns early, holding no observation matrix, when the period window is empty;
    // the matrix is counted only when rounding cannot empty it.
    const minPeriod = Math.floor((r.sampleRate / f0MaxHz) * (1 + 1e-6));
    const maxPeriod = Math.min(Math.ceil((r.sampleRate / f0MinHz) * (1 - 1e-6)), frameLength - 1);
    const states =
      minPeriod < maxPeriod
        ? 2 * (Math.max(0, Math.floor(120 * Math.log2(f0MaxHz / f0MinHz) - 0.001)) + 1)
        : 0;
    // The input copy, its centered padded copy, the f0 and voiced-probability tracks, and the
    // observation matrix of doubles, frames x states.
    return toPitchResult(
      withHeapNeed(
        sampleNeed('samples', r.samples.length, r.sampleRate, (n) => {
          const frames = 1 + Math.floor(n / hopLength);
          const track = states > 0 ? 8 * frames : 0;
          return 4 * n + 4 * (n + frameLength) + track + 8 * frames * states;
        }),
        call,
      ),
    );
  });
}

/**
 * Parameters for segmenting an F0 track into stable monophonic notes. Give
 * exactly one of `voiced` and `voicedProb`; both or neither is a TypeError.
 */
export interface NoteSegmentsRequest {
  /** Per-frame F0 track in Hz, one value per frame, at `frameRate` frames per second. */
  f0Hz: Float32Array;
  /**
   * Per-frame voicing values in `[0, 1]`; anything below `voicedThreshold` is
   * unvoiced.
   *
   * Given in place of `voiced`. Pass {@link PitchResult.voicedFlag} as
   * `voiced` instead. Do **not** pass {@link pitchPyin}'s `voicedProb`: that
   * value is the frame's voiced observation mass and rises with F0 for a fixed
   * `frameLength`, so a fixed threshold silently returns no segments at all for
   * low-register material (a steady tone below roughly C5 never reaches 0.5).
   */
  voicedProb?: Float32Array;
  /**
   * Per-frame voiced flags (truthy = voiced), the same length as `f0Hz`, given in
   * place of `voicedProb`; `voicedThreshold` does not apply.
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
 * `f0Hz` and the one voicing input given (`voiced` or `voicedProb`) must have
 * the same non-zero length. Zero-Hz frames and unvoiced frames (a `voicedProb`
 * below `voicedThreshold`, default `0.5`, or a zero flag) are treated as unvoiced.
 */
export function noteSegments(request: NoteSegmentsRequest): NoteSegment[];
export function noteSegments(...args: unknown[]): NoteSegment[] {
  return withEntry('noteSegments', () => {
    const r = requestOnly<NoteSegmentsRequest>('noteSegments', args);
    // The embind reader takes 0/1 bytes. Both voicing inputs are passed as given, so the core
    // refuses both and neither as two spellings of one input.
    const flags = r.voiced != null ? Uint8Array.from(toVoicedFloat32(r.voiced)) : null;
    const prob = r.voicedProb ?? null;
    if (prob != null && !(prob instanceof Float32Array)) {
      throw refuseType('voicedProb must be a Float32Array');
    }
    const call = () =>
      getSonareModule().noteSegments(r.f0Hz, prob, flags, r.frameRate, {
        segmentationThresholdCents: r.segmentationThresholdCents,
        minNoteMs: r.minNoteMs,
        referenceHz: r.referenceHz,
        voicedThreshold: r.voicedThreshold,
      });
    if (!(r.f0Hz instanceof Float32Array)) {
      return call();
    }
    // The f0 copy, the probability copy when given, and the flag bytes beside their widened copy.
    return withHeapNeed(
      {
        subject: 'f0Hz',
        size: r.f0Hz.length,
        bytes: (n) => 4 * n + (prob !== null ? 4 * n : 0) + (flags !== null ? 5 * n : 0),
        monotone: true,
      },
      call,
    );
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
    const bins = withHeapNeed(
      {
        subject: 'frequencies',
        size: r.frequencies.length,
        bytes: (n) => 4 * n,
        monotone: true,
      },
      () => getSonareModule().pitchTuning(r.frequencies, r.resolution ?? 0.01, binsPerOctave),
    );
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
    const nFft = r.nFft ?? 2048;
    const hopLength = r.hopLength ?? 512;
    const bins = guardedPeakTrack(r.samples, r.sampleRate, nFft, hopLength, () =>
      getSonareModule().estimateTuning(
        r.samples,
        r.sampleRate,
        nFft,
        hopLength,
        r.resolution ?? 0.01,
        binsPerOctave,
      ),
    );
    return tuningCentsFromNative((bins * 12) / binsPerOctave);
  });
}
