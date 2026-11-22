import { refuse, refuseType, withEntry } from './internal/entry.js';
import { assertMatrix2D } from './internal/feature_matrix.js';
import { resolveIntegerOption, resolvePositiveIntegerOption } from './internal/feature_options.js';
import { addon } from './native.js';
import type { Matrix2D } from './types_analysis.js';
import { assertPositiveInteger, requestOnly } from './validation.js';

/** Request for {@link powerToDb}. */
export interface PowerToDbRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Power values to convert, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference power. Default 1. */
  ref?: number;
  /** Floor under which a power is clamped. Default 1e-10. */
  amin?: number;
  /** Dynamic range kept below the peak, in dB: at least 0, or `Infinity` for no clipping. Default 80. */
  topDb?: number;
}

/** Request for {@link amplitudeToDb}. */
export interface AmplitudeToDbRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Amplitude values to convert, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference amplitude. Default 1. */
  ref?: number;
  /** Floor under which an amplitude is clamped. Default 1e-5. */
  amin?: number;
  /** Dynamic range kept below the peak, in dB: at least 0, or `Infinity` for no clipping. Default 80. */
  topDb?: number;
}

/** Request for {@link dbToPower}. */
export interface DbToPowerRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Decibel values to convert to power, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference power. Default 1. */
  ref?: number;
}

/** Request for {@link dbToAmplitude}. */
export interface DbToAmplitudeRequest<T extends Float32Array | Matrix2D = Float32Array | Matrix2D> {
  /** Decibel values to convert to amplitude, as a flat array or a matrix; the result takes the same form. */
  values: T;
  /** Reference amplitude. Default 1. */
  ref?: number;
}

/**
 * Frequency in Hz to the Slaney mel scale.
 *
 * A total function, matching the C ABI and librosa: a non-finite `hz`
 * propagates rather than throwing, and a magnitude past the 32-bit float range
 * saturates to an infinity the same way the C conversion does. Out-of-audio
 * frequencies are not refused either — the mapping is defined over the whole
 * real line.
 */
export function hzToMel(hz: number): number {
  return addon.hzToMel(hz);
}

/**
 * Slaney mel scale to frequency in Hz. Total over the same domain as
 * {@link hzToMel}.
 */
export function melToHz(mel: number): number {
  return addon.melToHz(mel);
}

/**
 * Frequency in Hz to a fractional MIDI note number (A4 = 440 Hz = 69).
 *
 * Total, like {@link hzToMel}. A non-positive `hz` returns `-Infinity`, the log2
 * limit, and {@link midiToHz} maps that back to 0. A NaN propagates, which is
 * what makes a default {@link pitchPyin} track — whose unvoiced frames are NaN —
 * safe to map through.
 */
export function hzToMidi(hz: number): number {
  return addon.hzToMidi(hz);
}

/**
 * Fractional MIDI note number to frequency in Hz. Total, like {@link hzToMel};
 * `-Infinity` bottoms out at 0 rather than propagating its sign.
 */
export function midiToHz(midi: number): number {
  return addon.midiToHz(midi);
}

/**
 * Nearest note name for `hz` (`"A4"`, `"C#5"`).
 *
 * @throws `RangeError` for a frequency with no
 * note: zero, negative, past the representable MIDI range, and non-finite alike.
 * A default {@link pitchPyin} track fills unvoiced frames with NaN, so drop
 * those frames before mapping the track through this.
 */
export function hzToNote(hz: number): string {
  return addon.hzToNote(hz);
}

/**
 * Frequency in Hz of a note name (`"A4"`, `"C#5"`, `"Db3"`; the octave defaults to 4).
 *
 * @throws `RangeError` for a name that does not
 * parse or whose octave is out of range.
 */
export function noteToHz(note: string): number {
  return addon.noteToHz(note);
}

/**
 * Frame index to seconds at `sampleRate` / `hopLength`.
 *
 * @param sampleRate Default 22050.
 * @param hopLength Positive integer in samples. Default 512.
 * @throws `RangeError` for a hop that is not a positive integer.
 */
export function framesToTime(frames: number, sampleRate = 22050, hopLength = 512): number {
  return withEntry('framesToTime', () => {
    return addon.framesToTime(frames, sampleRate, hopLength);
  });
}

/**
 * Seconds to a frame index at `sampleRate` / `hopLength`.
 *
 * `timeSec` must be finite: the conversion saturates into the int frame index, so
 * an infinity would return `INT_MAX` — a frame index nothing downstream can
 * tell from a real one.
 */
export function timeToFrames(timeSec: number, sampleRate = 22050, hopLength = 512): number {
  return withEntry('timeToFrames', () => {
    return addon.timeToFrames(timeSec, sampleRate, hopLength);
  });
}

/** Absent `nFft` leaves out the centring offset; the offset must otherwise be a positive integer. */
function resolveCentringNFft(nFft: number | null | undefined): number {
  if (nFft === undefined || nFft === null) {
    return 0;
  }
  const resolved = resolveIntegerOption('nFft', nFft, 0);
  if (resolved === 0) {
    throw refuse('nFft must not be 0; omit it for no centring');
  }
  assertPositiveInteger(resolved, 'nFft');
  return resolved;
}

/**
 * Frame index to the sample it starts at, optionally shifted by the centring
 * offset of an `nFft`-sample window.
 *
 * @param hopLength Positive integer in samples. Default 512.
 * @param nFft Absent means no centring offset.
 * @throws `RangeError` for a hop or an explicit `nFft` that is not a positive integer.
 */
export function framesToSamples(frames: number, hopLength = 512, nFft?: number): number {
  return withEntry('framesToSamples', () => {
    const resolvedHopLength = resolvePositiveIntegerOption('hopLength', hopLength, 512);
    return addon.framesToSamples(frames, resolvedHopLength, resolveCentringNFft(nFft));
  });
}

/**
 * Sample count to the frame count it spans, optionally shifted by the centring
 * offset of an `nFft`-sample window.
 *
 * @param hopLength Positive integer in samples. Default 512.
 * @param nFft Absent means no centring offset.
 * @throws `RangeError` for a hop or an explicit `nFft` that is not a positive integer.
 */
export function samplesToFrames(samples: number, hopLength = 512, nFft?: number): number {
  return withEntry('samplesToFrames', () => {
    const resolvedHopLength = resolvePositiveIntegerOption('hopLength', hopLength, 512);
    return addon.samplesToFrames(samples, resolvedHopLength, resolveCentringNFft(nFft));
  });
}

/**
 * Applies an elementwise conversion to a flat array or to a matrix's data,
 * returning the same form.
 */
function convertElementwise(
  values: unknown,
  convert: (data: Float32Array) => Float32Array,
): Float32Array | Matrix2D {
  if (values instanceof Float32Array) {
    return convert(values);
  }
  if (typeof values !== 'object' || values === null || Array.isArray(values)) {
    throw refuseType('values must be a Float32Array or a Matrix2D');
  }
  assertMatrix2D(values, 'values', false);
  return { data: convert(values.data), rows: values.rows, cols: values.cols };
}

/** Power to dB. Request-only; `values` is a power spectrogram or series. */
export function powerToDb<T extends Float32Array | Matrix2D>(request: PowerToDbRequest<T>): T;
export function powerToDb(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('powerToDb', () => {
    const r = requestOnly<PowerToDbRequest>('powerToDb', args);
    return convertElementwise(r.values, (data) =>
      addon.powerToDb(data, r.ref ?? 1, r.amin ?? 1e-10, r.topDb ?? 80),
    );
  });
}

/** Amplitude to dB. Request-only; `values` is a magnitude spectrogram or series. */
export function amplitudeToDb<T extends Float32Array | Matrix2D>(
  request: AmplitudeToDbRequest<T>,
): T;
export function amplitudeToDb(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('amplitudeToDb', () => {
    const r = requestOnly<AmplitudeToDbRequest>('amplitudeToDb', args);
    return convertElementwise(r.values, (data) =>
      addon.amplitudeToDb(data, r.ref ?? 1, r.amin ?? 1e-5, r.topDb ?? 80),
    );
  });
}

/** dB to power. Request-only. */
export function dbToPower<T extends Float32Array | Matrix2D>(request: DbToPowerRequest<T>): T;
export function dbToPower(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('dbToPower', () => {
    const r = requestOnly<DbToPowerRequest>('dbToPower', args);
    return convertElementwise(r.values, (data) => addon.dbToPower(data, r.ref ?? 1));
  });
}

/** dB to amplitude. Request-only. */
export function dbToAmplitude<T extends Float32Array | Matrix2D>(
  request: DbToAmplitudeRequest<T>,
): T;
export function dbToAmplitude(...args: unknown[]): Float32Array | Matrix2D {
  return withEntry('dbToAmplitude', () => {
    const r = requestOnly<DbToAmplitudeRequest>('dbToAmplitude', args);
    return convertElementwise(r.values, (data) => addon.dbToAmplitude(data, r.ref ?? 1));
  });
}
