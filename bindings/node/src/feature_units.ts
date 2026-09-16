import { resolveIntegerOption, resolvePositiveIntegerOption } from './_feature_options.js';
import { addon } from './native.js';

export interface ValuesRequest {
  values: Float32Array;
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
 * Every frequency with no note answers `"?"` rather than throwing: zero,
 * negative, past the representable MIDI range, and non-finite alike. A default
 * {@link pitchPyin} track fills unvoiced frames with NaN, so mapping one through
 * this yields `"?"` at those frames.
 */
export function hzToNote(hz: number): string {
  return addon.hzToNote(hz);
}

export function noteToHz(note: string): number {
  return addon.noteToHz(note);
}

export function framesToTime(request: { frames: number; sr?: number; hopLength?: number }): number;
export function framesToTime(frames: number, sr?: number, hopLength?: number): number;
export function framesToTime(
  frames: number | { frames: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof frames === 'number' ? { frames, sr, hopLength } : frames;
  // The conversion requires a positive hop, so positivity is the domain; a
  // fraction has to be refused here because the addon narrowing truncates it.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'framesToTime',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.framesToTime(request.frames, request.sr ?? 22050, resolvedHopLength);
}

/**
 * Seconds to a frame index at `sr` / `hopLength`.
 *
 * `time` must be finite: the conversion saturates into the int frame index, so
 * an infinity would return `INT_MAX` — a frame index nothing downstream can
 * tell from a real one.
 */
export function timeToFrames(request: { time: number; sr?: number; hopLength?: number }): number;
export function timeToFrames(time: number, sr?: number, hopLength?: number): number;
export function timeToFrames(
  time: number | { time: number; sr?: number; hopLength?: number },
  sr = 22050,
  hopLength = 512,
): number {
  const request = typeof time === 'number' ? { time, sr, hopLength } : time;
  // Positive hop, as framesToTime: the same conversion in the other direction.
  const resolvedHopLength = resolvePositiveIntegerOption(
    'timeToFrames',
    'hopLength',
    request.hopLength,
    512,
  );
  return addon.timeToFrames(request.time, request.sr ?? 22050, resolvedHopLength);
}

export function framesToSamples(request: {
  frames: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function framesToSamples(frames: number, hopLength?: number, nFft?: number): number;
export function framesToSamples(
  frames: number | { frames: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof frames === 'number' ? { frames, hopLength, nFft } : frames;
  // Integrality only: this conversion saturates rather than refusing, and `nFft`
  // at or below 0 is the documented "no centering offset" spelling, so a domain
  // here would reject values the core answers.
  const resolvedHopLength = resolveIntegerOption(
    'framesToSamples',
    'hopLength',
    request.hopLength,
    512,
  );
  const resolvedNFft = resolveIntegerOption('framesToSamples', 'nFft', request.nFft, 0);
  return addon.framesToSamples(request.frames, resolvedHopLength, resolvedNFft);
}

export function samplesToFrames(request: {
  samples: number;
  hopLength?: number;
  nFft?: number;
}): number;
export function samplesToFrames(samples: number, hopLength?: number, nFft?: number): number;
export function samplesToFrames(
  samples: number | { samples: number; hopLength?: number; nFft?: number },
  hopLength = 512,
  nFft = 0,
): number {
  const request = typeof samples === 'number' ? { samples, hopLength, nFft } : samples;
  // Integrality only, as framesToSamples: a non-positive hop answers 0 frames
  // rather than failing, so positivity is not this conversion's domain.
  const resolvedHopLength = resolveIntegerOption(
    'samplesToFrames',
    'hopLength',
    request.hopLength,
    512,
  );
  const resolvedNFft = resolveIntegerOption('samplesToFrames', 'nFft', request.nFft, 0);
  return addon.samplesToFrames(request.samples, resolvedHopLength, resolvedNFft);
}

export function powerToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function powerToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function powerToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.powerToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-10,
    request.topDb ?? 80,
  );
}

export function amplitudeToDb(
  request: ValuesRequest & { ref?: number; amin?: number; topDb?: number },
): Float32Array;
export function amplitudeToDb(
  values: Float32Array,
  ref?: number,
  amin?: number,
  topDb?: number,
): Float32Array;
export function amplitudeToDb(
  values: Float32Array | (ValuesRequest & { ref?: number; amin?: number; topDb?: number }),
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  const request = values instanceof Float32Array ? { values, ref, amin, topDb } : values;
  return addon.amplitudeToDb(
    request.values,
    request.ref ?? 1,
    request.amin ?? 1e-5,
    request.topDb ?? 80,
  );
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return addon.dbToAmplitude(values, ref);
}
