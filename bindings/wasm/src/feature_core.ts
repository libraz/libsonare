import { getSonareModule } from './module_state';
import type { TempogramMode } from './public_types';
import type {
  WasmCyclicTempogramResult,
  WasmFrameResult,
  WasmTempogramResult,
  WasmTrimResult,
} from './sonare.js';

function requireModule() {
  return getSonareModule();
}

// ============================================================================
// Core - Unit Conversion
// ============================================================================

/**
 * Convert frequency in Hz to Mel scale.
 *
 * @param hz - Frequency in Hz
 * @returns Mel frequency
 */
export function hzToMel(hz: number): number {
  return requireModule().hzToMel(hz);
}

/**
 * Convert Mel scale to frequency in Hz.
 *
 * @param mel - Mel frequency
 * @returns Frequency in Hz
 */
export function melToHz(mel: number): number {
  return requireModule().melToHz(mel);
}

/**
 * Convert frequency in Hz to MIDI note number.
 *
 * @param hz - Frequency in Hz
 * @returns MIDI note number (A4 = 440 Hz = 69)
 */
export function hzToMidi(hz: number): number {
  return requireModule().hzToMidi(hz);
}

/**
 * Convert MIDI note number to frequency in Hz.
 *
 * @param midi - MIDI note number
 * @returns Frequency in Hz
 */
export function midiToHz(midi: number): number {
  return requireModule().midiToHz(midi);
}

/**
 * Convert frequency in Hz to note name.
 *
 * @param hz - Frequency in Hz
 * @returns Note name (e.g., "A4", "C#5")
 */
export function hzToNote(hz: number): string {
  return requireModule().hzToNote(hz);
}

/**
 * Convert note name to frequency in Hz.
 *
 * @param note - Note name (e.g., "A4", "C#5")
 * @returns Frequency in Hz
 */
export function noteToHz(note: string): number {
  return requireModule().noteToHz(note);
}

/**
 * Convert frame index to time in seconds.
 *
 * @param frames - Frame index
 * @param sr - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Time in seconds
 */
export function framesToTime(frames: number, sr = 22050, hopLength = 512): number {
  return requireModule().framesToTime(frames, sr, hopLength);
}

/**
 * Convert time in seconds to frame index.
 *
 * @param time - Time in seconds
 * @param sr - Sample rate in Hz (default: 22050)
 * @param hopLength - Hop length in samples (default: 512)
 * @returns Frame index
 */
export function timeToFrames(time: number, sr = 22050, hopLength = 512): number {
  return requireModule().timeToFrames(time, sr, hopLength);
}

export function framesToSamples(frames: number, hopLength = 512, nFft = 0): number {
  return requireModule().framesToSamples(frames, hopLength, nFft);
}

export function samplesToFrames(samples: number, hopLength = 512, nFft = 0): number {
  return requireModule().samplesToFrames(samples, hopLength, nFft);
}

export function powerToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-10,
  topDb = 80.0,
): Float32Array {
  return requireModule().powerToDb(values, ref, amin, topDb);
}

export function amplitudeToDb(
  values: Float32Array,
  ref = 1.0,
  amin = 1e-5,
  topDb = 80.0,
): Float32Array {
  return requireModule().amplitudeToDb(values, ref, amin, topDb);
}

export function dbToPower(values: Float32Array, ref = 1.0): Float32Array {
  return requireModule().dbToPower(values, ref);
}

export function dbToAmplitude(values: Float32Array, ref = 1.0): Float32Array {
  return requireModule().dbToAmplitude(values, ref);
}

export function preemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  return requireModule().preemphasis(samples, coef, zi ?? null);
}

export function deemphasis(samples: Float32Array, coef = 0.97, zi?: number): Float32Array {
  return requireModule().deemphasis(samples, coef, zi ?? null);
}

export function trimSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): WasmTrimResult {
  return requireModule().trimSilence(samples, topDb, frameLength, hopLength);
}

export function splitSilence(
  samples: Float32Array,
  topDb = 60.0,
  frameLength = 2048,
  hopLength = 512,
): Int32Array {
  return requireModule().splitSilence(samples, topDb, frameLength, hopLength);
}

export function frameSignal(
  samples: Float32Array,
  frameLength: number,
  hopLength: number,
): WasmFrameResult {
  return requireModule().frameSignal(samples, frameLength, hopLength);
}

export function padCenter(values: Float32Array, targetSize: number, padValue = 0.0): Float32Array {
  return requireModule().padCenter(values, targetSize, padValue);
}

export function fixLength(values: Float32Array, targetSize: number, padValue = 0.0): Float32Array {
  return requireModule().fixLength(values, targetSize, padValue);
}

export function fixFrames(frames: Int32Array, xMin = 0, xMax = -1, pad = true): Int32Array {
  return requireModule().fixFrames(frames, xMin, xMax, pad);
}

export function peakPick(
  values: Float32Array,
  preMax: number,
  postMax: number,
  preAvg: number,
  postAvg: number,
  delta: number,
  wait: number,
): Int32Array {
  return requireModule().peakPick(values, preMax, postMax, preAvg, postAvg, delta, wait);
}

export function vectorNormalize(values: Float32Array, normType = 0, threshold = 0.0): Float32Array {
  return requireModule().vectorNormalize(values, normType, threshold);
}

export function pcen(
  values: Float32Array,
  nBins: number,
  nFrames: number,
  options: Record<string, number> = {},
): Float32Array {
  return requireModule().pcen(values, nBins, nFrames, options);
}

export function tonnetz(chromagram: Float32Array, nChroma: number, nFrames: number): Float32Array {
  return requireModule().tonnetz(chromagram, nChroma, nFrames);
}

export function tempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  mode: TempogramMode = 'autocorrelation',
): WasmTempogramResult {
  return requireModule().tempogram(onsetEnvelope, sampleRate, hopLength, winLength, mode);
}

export function cyclicTempogram(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  winLength = 384,
  bpmMin = 60.0,
  nBins = 60,
): WasmCyclicTempogramResult {
  return requireModule().cyclicTempogram(
    onsetEnvelope,
    sampleRate,
    hopLength,
    winLength,
    bpmMin,
    nBins,
  );
}

export function plp(
  onsetEnvelope: Float32Array,
  sampleRate = 22050,
  hopLength = 512,
  tempoMin = 30.0,
  tempoMax = 300.0,
  winLength = 384,
): Float32Array {
  return requireModule().plp(onsetEnvelope, sampleRate, hopLength, tempoMin, tempoMax, winLength);
}
