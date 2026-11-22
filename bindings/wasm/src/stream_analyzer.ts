import { chordQualityToken } from './analysis_helpers.js';
import { entryClass, refuse, refuseType, refuseUnknownKey } from './internal/entry.js';
import { assertHandleLive } from './internal/handle_guard.js';
import { isNonArrayObject } from './internal/value_guards.js';
import { windowOrdinal } from './internal/window_type.js';
import { getSonareModule } from './module_state.js';
import type { Key, PitchClass } from './public_types.js';
import type {
  WasmAnalyzerStats,
  WasmBarChord,
  WasmChordChange,
  WasmFrameBuffer,
  WasmStreamAnalyzer,
  WasmStreamFramesI16,
  WasmStreamFramesU8,
} from './sonare.js';
import type {
  StreamBarChord,
  StreamChordChange,
  StreamConfig,
  StreamFrames,
  StreamFramesI16,
  StreamFramesU8,
  StreamProgressiveEstimate,
  StreamQuantizeConfig,
  StreamStats,
} from './stream_types.js';
import { assertFloatValue, assertIntegerValue } from './validation.js';

const REFERENCE_HZ_RANGE = [220, 880] as const;
const CONFIG_KEYS: ReadonlySet<string> = new Set([
  'sampleRate',
  'nFft',
  'hopLength',
  'nMels',
  'fminHz',
  'fmaxHz',
  'referenceHz',
  'computeMel',
  'computeChroma',
  'computeOnset',
  'computeSpectral',
  'emitEveryNFrames',
  'maxPendingFrames',
  'maxProgressionEntries',
  'keyUpdateIntervalSec',
  'bpmUpdateIntervalSec',
  'window',
]);

// Feature bits of the native frame buffers.
const FEATURE_MEL = 1;
const FEATURE_CHROMA = 2;
const FEATURE_ONSET = 4;
const FEATURE_SPECTRAL = 8;

/** Native stream buffers hold frame-major data; the public layout is band-major. */
function bandMajor<T extends Float32Array | Uint8Array | Int16Array>(
  flat: T,
  nFrames: number,
  nBands: number,
): T {
  const out = new (flat.constructor as new (length: number) => T)(flat.length);
  for (let f = 0; f < nFrames; f++) {
    for (let b = 0; b < nBands; b++) {
      out[b * nFrames + f] = flat[f * nBands + b];
    }
  }
  return out;
}

interface NativeFrames<T extends Float32Array | Uint8Array | Int16Array> {
  nFrames: number;
  nMels: number;
  nChroma: number;
  featureFlags: number;
  timestampsSec: Float32Array;
  mel: T;
  chroma: T;
  onsetStrength: T;
  rmsEnergy: T;
  spectralCentroid: T;
  spectralFlatness: T;
}

/** The per-frame arrays every read path shares, absent where the feature was not computed. */
function sharedFrameArrays<T extends Float32Array | Uint8Array | Int16Array>(
  native: NativeFrames<T>,
): {
  onsetStrength?: T;
  rmsEnergy: T;
  spectralCentroid?: T;
  spectralFlatness?: T;
} {
  const flags = native.featureFlags;
  const out: {
    onsetStrength?: T;
    rmsEnergy: T;
    spectralCentroid?: T;
    spectralFlatness?: T;
  } = { rmsEnergy: native.rmsEnergy };
  if (flags & FEATURE_ONSET) {
    out.onsetStrength = native.onsetStrength;
  }
  if (flags & FEATURE_SPECTRAL) {
    out.spectralCentroid = native.spectralCentroid;
    out.spectralFlatness = native.spectralFlatness;
  }
  return out;
}

function quantizedFrames<T extends Uint8Array | Int16Array>(
  native: NativeFrames<T> & { chordRoot: Int8Array; chordQuality: Int8Array; chordConfidence: T },
): StreamFramesU8 | StreamFramesI16 {
  const out: {
    nFrames: number;
    timestampsSec: Float32Array;
    mel?: T;
    chroma?: T;
    chordRoot?: Int8Array;
    chordQuality?: Int8Array;
    chordConfidence?: T;
  } = {
    nFrames: native.nFrames,
    timestampsSec: native.timestampsSec,
  };
  if (native.featureFlags & FEATURE_MEL) {
    out.mel = bandMajor(native.mel, native.nFrames, native.nMels);
  }
  if (native.featureFlags & FEATURE_CHROMA) {
    out.chroma = bandMajor(native.chroma, native.nFrames, native.nChroma);
    out.chordRoot = native.chordRoot;
    out.chordQuality = native.chordQuality;
    out.chordConfidence = native.chordConfidence;
  }
  return { ...out, ...sharedFrameArrays(native) } as StreamFramesU8 | StreamFramesI16;
}

function chordChange(c: WasmChordChange): StreamChordChange {
  return {
    root: c.root as PitchClass,
    quality: chordQualityToken(c.quality),
    name: c.name,
    startSec: c.startSec,
    confidence: c.confidence,
  };
}

function barChord(c: WasmBarChord): StreamBarChord {
  return {
    barIndex: c.barIndex,
    root: c.root as PitchClass,
    quality: chordQualityToken(c.quality),
    name: c.name,
    startSec: c.startSec,
    confidence: c.confidence,
  };
}

function estimateOf(e: WasmAnalyzerStats['estimate']): StreamProgressiveEstimate {
  const out: StreamProgressiveEstimate = {
    bpmCandidateCount: e.bpmCandidateCount,
    chordProgression: e.chordProgression.map(chordChange),
    barChordProgression: e.barChordProgression.map(barChord),
    votedPattern: (e.votedPattern || []).map(barChord),
    patternLength: e.patternLength,
    allPatternScores: (e.allPatternScores || []).map((p) => ({ name: p.name, score: p.score })),
    accumulatedSec: e.accumulatedSec,
    usedFrames: e.usedFrames,
    updated: e.updated,
  };
  if (e.bpm > 0) {
    out.bpm = e.bpm;
    out.bpmConfidence = e.bpmConfidence;
  }
  if (e.keyName !== null && e.keyShortName !== null && e.key >= 0) {
    const key: Key = {
      root: e.key as PitchClass,
      mode: e.keyMinor ? 'minor' : 'major',
      confidence: e.keyConfidence,
      name: e.keyName,
      shortName: e.keyShortName,
    };
    out.key = key;
  }
  if (e.chordRoot >= 0) {
    out.chord = {
      root: e.chordRoot as PitchClass,
      quality: chordQualityToken(e.chordQuality),
      name: e.chordName,
      startSec: e.chordStartSec,
      confidence: e.chordConfidence,
    };
  }
  if (e.currentBar >= 0) {
    out.currentBar = e.currentBar;
    out.barDurationSec = e.barDurationSec;
  }
  if (e.detectedPatternName) {
    out.detectedPatternName = e.detectedPatternName;
    out.detectedPatternScore = e.detectedPatternScore;
  }
  return out;
}

/** Refuses an A4 reference outside what native accepts, as a RangeError. */
function assertReferenceHz(referenceHz: unknown): asserts referenceHz is number {
  if (typeof referenceHz !== 'number') {
    throw refuseType(`referenceHz must be a number`);
  }
  if (
    !Number.isFinite(referenceHz) ||
    referenceHz < REFERENCE_HZ_RANGE[0] ||
    referenceHz > REFERENCE_HZ_RANGE[1]
  ) {
    throw refuse(
      `referenceHz must be within ${REFERENCE_HZ_RANGE[0]}..${REFERENCE_HZ_RANGE[1]} Hz`,
    );
  }
}

/**
 * Real-time streaming audio analyzer.
 *
 * @example
 * ```ts
 * import { init, StreamAnalyzer } from '@libraz/libsonare';
 *
 * await init();
 *
 * const analyzer = new StreamAnalyzer({ sampleRate: 44100 });
 *
 * // In audio processing callback
 * analyzer.process(samples);
 *
 * // Get current analysis state
 * const stats = analyzer.stats();
 * console.log('BPM:', stats.estimate.bpm);
 * console.log('Key:', stats.estimate.key?.name);
 * console.log('Chord progression:', stats.estimate.chordProgression);
 * ```
 *
 * The native analyzer supports one serialized producer (`process` or
 * `finalize`) concurrently with one serialized consumer
 * (`availableFrames`, a `readFrames*` method, `stats`, `frameCount`, or
 * `currentTimeSec`) when a threaded WASM host shares the native instance.
 * Publication is allocation-free release/acquire. `reset`, setters, and
 * deletion require both roles to be stopped. A full pending ring drops the
 * newly produced output frame while analysis totals keep advancing. Ordinary
 * browser builds remain single-threaded unless the host explicitly provisions
 * shared-memory worker support.
 */
@entryClass('StreamAnalyzer', ['process', 'analyzer'])
export class StreamAnalyzer {
  private analyzerHandle: WasmStreamAnalyzer;
  private released = false;

  private get analyzer(): WasmStreamAnalyzer {
    assertHandleLive(this.released, 'StreamAnalyzer');
    return this.analyzerHandle;
  }

  /**
   * Create a new StreamAnalyzer.
   *
   * @param config - Configuration; `sampleRate` is required (TypeError when
   *   absent, RangeError when not a supported integer rate). An unknown key is a
   *   TypeError and an out-of-domain value a RangeError.
   */
  constructor(config: StreamConfig) {
    if (!isNonArrayObject(config)) {
      throw refuseType('config must be an object with a sampleRate');
    }
    for (const key of Object.keys(config)) {
      if (!CONFIG_KEYS.has(key)) {
        throw refuseUnknownKey(key, [...CONFIG_KEYS]);
      }
    }
    if (config.window !== undefined) {
      windowOrdinal(config.window);
    }
    if (config.fminHz !== undefined) {
      assertFloatValue(config.fminHz, 'fminHz');
    }
    if (config.fmaxHz !== undefined) {
      if (
        typeof config.fmaxHz === 'number' &&
        !(Number.isFinite(config.fmaxHz) && config.fmaxHz > 0)
      ) {
        throw refuse('fmaxHz must be finite and greater than 0; omit it for Nyquist');
      }
      assertFloatValue(config.fmaxHz, 'fmaxHz');
    }
    if (config.referenceHz !== undefined) {
      assertReferenceHz(config.referenceHz);
    }
    for (const key of [
      'nFft',
      'hopLength',
      'nMels',
      'emitEveryNFrames',
      'maxPendingFrames',
      'maxProgressionEntries',
    ] as const) {
      if (config[key] !== undefined) {
        assertIntegerValue(config[key], key);
      }
    }
    // An absent field reaches the native constructor as undefined and keeps the library default.
    const module = getSonareModule();
    this.analyzerHandle = new module.StreamAnalyzer(
      config.sampleRate,
      config.nFft,
      config.hopLength,
      config.nMels,
      config.fminHz,
      config.fmaxHz,
      config.referenceHz,
      config.computeMel,
      config.computeChroma,
      config.computeOnset,
      config.computeSpectral,
      config.emitEveryNFrames,
      config.maxPendingFrames,
      config.maxProgressionEntries,
      config.keyUpdateIntervalSec,
      config.bpmUpdateIntervalSec,
      config.window === undefined ? undefined : windowOrdinal(config.window),
    );
  }

  /**
   * Process audio samples.
   *
   * Without `startSample` the analyzer tracks the stream offset itself. With
   * it, the chunk is anchored at that cumulative sample count; a gap, seek, or
   * switch between the two forms requires `reset()` first. Feeding a finalized
   * analyzer is an invalid-state error; call `reset()` first to start a new
   * stream.
   *
   * @param samples - Audio samples (mono, float32)
   * @param startSample - Cumulative sample count at the start of this chunk
   */
  process(samples: Float32Array, startSample?: number): void {
    this.analyzer.process(samples, startSample);
  }

  /**
   * Drain any high-rate resampler tail, then zero-pad the final partial frame.
   *
   * Repeating a successful call is a no-op, and a call that fails leaves the
   * stream un-finalized so a retry resumes from the same point. Call `reset()`
   * before reusing the analyzer for another stream: more audio fed to a
   * finalized analyzer is rejected rather than silently analyzed without the
   * overlap context the finalized tail consumed.
   */
  finalize(): void {
    this.analyzer.finalize();
  }

  /** Number of frames available to read. */
  get availableFrames(): number {
    return this.analyzer.availableFrames();
  }

  /**
   * Read processed frames. A feature array is absent when its feature is not
   * computed.
   *
   * @param maxFrames - Maximum number of frames to read
   * @returns Frames; `mel` and `chroma` are band-major matrices (rows are
   *   bands, cols are frames)
   */
  readFrames(maxFrames: number): StreamFrames {
    const native: WasmFrameBuffer = this.analyzer.readFramesSoa(maxFrames);
    const flags = native.featureFlags;
    const out: StreamFrames = {
      nFrames: native.nFrames,
      timestampsSec: native.timestampsSec,
      ...sharedFrameArrays(native),
    };
    if (flags & FEATURE_MEL) {
      out.mel = {
        data: bandMajor(native.mel, native.nFrames, native.nMels),
        rows: native.nMels,
        cols: native.nFrames,
      };
    }
    if (flags & FEATURE_CHROMA) {
      out.chroma = {
        data: bandMajor(native.chroma, native.nFrames, native.nChroma),
        rows: native.nChroma,
        cols: native.nFrames,
      };
    }
    if (native.featureFlags & FEATURE_CHROMA) {
      out.chordRoot = native.chordRoot;
      out.chordQuality = native.chordQuality;
      out.chordConfidence = native.chordConfidence;
    }
    return out;
  }

  /**
   * Read frames as uint8-quantized arrays; `mel` and `chroma` are band-major
   * (rows are bands, cols are frames).
   *
   * @param maxFrames - Maximum number of frames to read
   * @param quantizeConfig - Optional quantization ranges; widen these for a
   *   stream louder or quieter than the defaults (omitted keeps the defaults)
   */
  readFramesU8(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesU8 {
    const native = this.analyzer.readFramesU8(maxFrames, quantizeConfig) as WasmStreamFramesU8;
    return quantizedFrames(native) as StreamFramesU8;
  }

  /**
   * Read frames as int16-quantized arrays; `mel` and `chroma` are band-major
   * (rows are bands, cols are frames).
   *
   * @param maxFrames - Maximum number of frames to read
   * @param quantizeConfig - Optional quantization ranges; widen these for a
   *   stream louder or quieter than the defaults (omitted keeps the defaults)
   */
  readFramesI16(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesI16 {
    const native = this.analyzer.readFramesI16(maxFrames, quantizeConfig) as WasmStreamFramesI16;
    return quantizedFrames(native) as StreamFramesI16;
  }

  /**
   * Reset the analyzer state.
   *
   * @param startSample - Sample the new stream starts at (default 0)
   */
  reset(startSample = 0): void {
    this.analyzer.reset(startSample);
  }

  /**
   * Get current statistics and progressive estimates.
   *
   * @returns Analyzer statistics including BPM, key, and chord progression
   */
  stats(): StreamStats {
    const s = this.analyzer.stats();
    return {
      totalFrames: s.totalFrames,
      totalSamples: s.totalSamples,
      durationSec: s.durationSec,
      pendingFrames: s.pendingFrames,
      droppedOutputFrames: s.droppedOutputFrames,
      droppedChordProgressionEntries: s.droppedChordProgressionEntries,
      droppedBarProgressionEntries: s.droppedBarProgressionEntries,
      nonFiniteDiscardBlocks: s.nonFiniteDiscardBlocks,
      estimate: estimateOf(s.estimate),
    };
  }

  /** Total frames processed. */
  get frameCount(): number {
    return this.analyzer.frameCount();
  }

  /** Current time position in seconds. */
  get currentTimeSec(): number {
    return this.analyzer.currentTimeSec();
  }

  /** Sample rate in Hz. */
  get sampleRate(): number {
    return this.analyzer.sampleRate();
  }

  /**
   * Set the expected total duration for pattern lock timing.
   *
   * @param durationSec - Total duration in seconds
   */
  setExpectedDuration(durationSec: number): void {
    this.analyzer.setExpectedDuration(durationSec);
  }

  /**
   * Set normalization gain for loud/compressed audio.
   *
   * Throws for a value outside 0.01..100 rather than clamping into it. The
   * usual recipe (`gain = targetLevel / measuredLevel`) can land outside that
   * range for a buffer that is not in the conventional ±1 float domain — an
   * integer-scaled one asks for about 3e-4 — and no getter exposes the
   * effective gain, so a clamped request would leave the analysis far off
   * target undetectably. Convert such a buffer before feeding it instead.
   *
   * @param gain - Gain factor to apply (e.g., 0.5 for -6dB reduction, range
   *   0.01..100)
   */
  setNormalizationGain(gain: number): void {
    this.analyzer.setNormalizationGain(gain);
  }

  /**
   * Set the A4 reference frequency for non-standard tuning.
   *
   * A wrong-typed value is a TypeError; one outside 220..880 Hz is a
   * RangeError rather than being clamped, so this and `referenceHz` at create
   * time accept exactly the same range.
   *
   * @param referenceHz - Reference frequency for A4 in Hz (default 440)
   * @example
   * ```ts
   * declare const analyzer: StreamAnalyzer;
   * // If audio is 1 semitone sharp (A4 = 466.16 Hz)
   * analyzer.setReferenceHz(466.16);
   * ```
   */
  setReferenceHz(referenceHz: number): void {
    assertReferenceHz(referenceHz);
    this.analyzer.setReferenceHz(referenceHz);
  }

  /** Release the underlying WASM object. Idempotent, as the Node facade is. */
  destroy(): void {
    if (this.released) {
      return;
    }
    this.released = true;
    this.analyzerHandle.delete();
  }

  /** Releases the handle through {@link destroy}, so the instance works with `using`. */
  [Symbol.dispose](): void {
    this.destroy();
  }
}
