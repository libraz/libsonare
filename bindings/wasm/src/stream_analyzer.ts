import { getSonareModule } from './module_state';
import type { ChordQuality, PitchClass } from './public_types';
import type { WasmStreamAnalyzer } from './sonare.js';
import type {
  AnalyzerStats,
  FrameBuffer,
  StreamConfig,
  StreamFramesI16,
  StreamFramesU8,
} from './stream_types';

// ============================================================================
// StreamAnalyzer Class
// ============================================================================

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
  constructor(config: StreamConfig) {
    const module = getSonareModule();
    this.analyzer = new module.StreamAnalyzer(
      config.sampleRate ?? 44100,
      config.nFft ?? 2048,
      config.hopLength ?? 512,
      config.nMels ?? 128,
      config.fmin ?? 0,
      config.fmax ?? 0,
      config.tuningRefHz ?? 440,
      config.computeMagnitude ?? false,
      config.computeMel ?? true,
      config.computeChroma ?? true,
      config.computeOnset ?? true,
      config.computeSpectral ?? true,
      config.emitEveryNFrames ?? 1,
      config.magnitudeDownsample ?? 1,
      config.keyUpdateIntervalSec ?? 5,
      config.bpmUpdateIntervalSec ?? 10,
      config.window ?? 0,
      config.outputFormat ?? 0,
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

  readFramesU8(maxFrames: number): StreamFramesU8 {
    return this.analyzer.readFramesU8(maxFrames) as StreamFramesU8;
  }

  readFramesI16(maxFrames: number): StreamFramesI16 {
    return this.analyzer.readFramesI16(maxFrames) as StreamFramesI16;
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

  /**
   * Release resources. Call when done using the analyzer.
   */
  dispose(): void {
    this.analyzer.delete();
  }
}
