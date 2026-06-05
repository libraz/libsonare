import { getSonareModule } from './module_state';
import type { ChordQuality, PitchClass } from './public_types';
import type { WasmStreamAnalyzer } from './sonare.js';
import type {
  AnalyzerStats,
  FrameBuffer,
  StreamConfig,
  StreamConfigDefaults,
  StreamFramesI16,
  StreamFramesU8,
  StreamQuantizeConfig,
} from './stream_types';

// ============================================================================
// StreamAnalyzer Class
// ============================================================================

export function streamAnalyzerConfigDefaults(): StreamConfigDefaults {
  return getSonareModule().streamAnalyzerConfigDefault();
}

/**
 * Real-time streaming audio analyzer.
 *
 * @example
 * ```typescript
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
 * console.log('Key:', stats.estimate.key);
 * console.log('Chord progression:', stats.estimate.chordProgression);
 * ```
 */
export class StreamAnalyzer {
  private analyzer: WasmStreamAnalyzer;

  /**
   * Create a new StreamAnalyzer.
   *
   * @param config - Configuration options
   */
  constructor(config: StreamConfig = {}) {
    if (config.computeMagnitude) {
      throw new Error(
        'computeMagnitude is not supported because magnitude frames are not exposed by StreamAnalyzer read paths.',
      );
    }
    const module = getSonareModule();
    const defaults = streamAnalyzerConfigDefaults();
    this.analyzer = new module.StreamAnalyzer(
      config.sampleRate ?? defaults.sampleRate,
      config.nFft ?? defaults.nFft,
      config.hopLength ?? defaults.hopLength,
      config.nMels ?? defaults.nMels,
      config.fmin ?? defaults.fmin,
      config.fmax ?? defaults.fmax,
      config.tuningRefHz ?? defaults.tuningRefHz,
      config.computeMagnitude ?? defaults.computeMagnitude,
      config.computeMel ?? defaults.computeMel,
      config.computeChroma ?? defaults.computeChroma,
      config.computeOnset ?? defaults.computeOnset,
      config.computeSpectral ?? defaults.computeSpectral,
      config.emitEveryNFrames ?? defaults.emitEveryNFrames,
      config.magnitudeDownsample ?? defaults.magnitudeDownsample,
      config.keyUpdateIntervalSec ?? defaults.keyUpdateIntervalSec,
      config.bpmUpdateIntervalSec ?? defaults.bpmUpdateIntervalSec,
      config.window ?? defaults.window,
      config.outputFormat ?? defaults.outputFormat,
    );
  }

  /**
   * Process audio samples.
   *
   * @param samples - Audio samples (mono, float32)
   */
  process(samples: Float32Array): void {
    this.analyzer.process(samples);
  }

  /**
   * Process audio samples with explicit sample offset.
   *
   * @param samples - Audio samples (mono, float32)
   * @param sampleOffset - Cumulative sample count at start of this chunk
   */
  processWithOffset(samples: Float32Array, sampleOffset: number): void {
    this.analyzer.processWithOffset(samples, sampleOffset);
  }

  /**
   * Flush the final partial frame with zero-padding.
   */
  finalize(): void {
    this.analyzer.finalize();
  }

  /**
   * Get the number of frames available to read.
   */
  availableFrames(): number {
    return this.analyzer.availableFrames();
  }

  /**
   * Read processed frames as Structure of Arrays.
   *
   * @param maxFrames - Maximum number of frames to read
   * @returns Frame buffer with analysis results
   */
  readFrames(maxFrames: number): FrameBuffer {
    return this.analyzer.readFramesSoa(maxFrames);
  }

  /**
   * Read frames as uint8-quantized arrays.
   *
   * @param maxFrames - Maximum number of frames to read
   * @param quantizeConfig - Optional quantization ranges; widen these for a
   *   stream louder or quieter than the defaults (omitted keeps the defaults)
   */
  readFramesU8(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesU8 {
    return this.analyzer.readFramesU8(maxFrames, quantizeConfig) as StreamFramesU8;
  }

  /**
   * Read frames as int16-quantized arrays.
   *
   * @param maxFrames - Maximum number of frames to read
   * @param quantizeConfig - Optional quantization ranges; widen these for a
   *   stream louder or quieter than the defaults (omitted keeps the defaults)
   */
  readFramesI16(maxFrames: number, quantizeConfig?: StreamQuantizeConfig): StreamFramesI16 {
    return this.analyzer.readFramesI16(maxFrames, quantizeConfig) as StreamFramesI16;
  }

  /**
   * Reset the analyzer state.
   *
   * @param baseSampleOffset - Starting sample offset (default 0)
   */
  reset(baseSampleOffset = 0): void {
    this.analyzer.reset(baseSampleOffset);
  }

  /**
   * Get current statistics and progressive estimates.
   *
   * @returns Analyzer statistics including BPM, key, and chord progression
   */
  stats(): AnalyzerStats {
    const s = this.analyzer.stats();
    return {
      totalFrames: s.totalFrames,
      totalSamples: s.totalSamples,
      durationSeconds: s.durationSeconds,
      estimate: {
        bpm: s.estimate.bpm,
        bpmConfidence: s.estimate.bpmConfidence,
        bpmCandidateCount: s.estimate.bpmCandidateCount,
        key: s.estimate.key as PitchClass,
        keyMinor: s.estimate.keyMinor,
        keyConfidence: s.estimate.keyConfidence,
        chordRoot: s.estimate.chordRoot as PitchClass,
        chordQuality: s.estimate.chordQuality as ChordQuality,
        chordConfidence: s.estimate.chordConfidence,
        chordStartTime: s.estimate.chordStartTime,
        chordProgression: s.estimate.chordProgression.map((c) => ({
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        barChordProgression: s.estimate.barChordProgression.map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        currentBar: s.estimate.currentBar,
        barDuration: s.estimate.barDuration,
        votedPattern: (s.estimate.votedPattern || []).map((c) => ({
          barIndex: c.barIndex,
          root: c.root as PitchClass,
          quality: c.quality as ChordQuality,
          startTime: c.startTime,
          confidence: c.confidence,
        })),
        patternLength: s.estimate.patternLength,
        detectedPatternName: s.estimate.detectedPatternName || '',
        detectedPatternScore: s.estimate.detectedPatternScore || 0,
        allPatternScores: (s.estimate.allPatternScores || []).map((p) => ({
          name: p.name,
          score: p.score,
        })),
        accumulatedSeconds: s.estimate.accumulatedSeconds,
        usedFrames: s.estimate.usedFrames,
        updated: s.estimate.updated,
      },
    };
  }

  /**
   * Get total frames processed.
   */
  frameCount(): number {
    return this.analyzer.frameCount();
  }

  /**
   * Get current time position in seconds.
   */
  currentTime(): number {
    return this.analyzer.currentTime();
  }

  /**
   * Get the sample rate.
   */
  sampleRate(): number {
    return this.analyzer.sampleRate();
  }

  /**
   * Set the expected total duration for pattern lock timing.
   *
   * @param durationSeconds - Total duration in seconds
   */
  setExpectedDuration(durationSeconds: number): void {
    this.analyzer.setExpectedDuration(durationSeconds);
  }

  /**
   * Set normalization gain for loud/compressed audio.
   *
   * @param gain - Gain factor to apply (e.g., 0.5 for -6dB reduction)
   */
  setNormalizationGain(gain: number): void {
    this.analyzer.setNormalizationGain(gain);
  }

  /**
   * Set tuning reference frequency for non-standard tuning.
   *
   * @param refHz - Reference frequency for A4 (default 440 Hz)
   * @example
   * // If audio is 1 semitone sharp (A4 = 466.16 Hz)
   * analyzer.setTuningRefHz(466.16);
   * // If audio is 1 semitone flat (A4 = 415.30 Hz)
   * analyzer.setTuningRefHz(415.30);
   */
  setTuningRefHz(refHz: number): void {
    this.analyzer.setTuningRefHz(refHz);
  }

  /** Release the underlying WASM object. Safe to call only once. */
  delete(): void {
    this.analyzer.delete();
  }

  /** Alias for {@link delete}, kept for backward compatibility (historical name). */
  dispose(): void {
    this.delete();
  }
}
